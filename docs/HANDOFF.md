# Handoff — resume here

## ▶▶ NEWEST (2026-08-09) — Fable2Native renders REAL LEVEL GEOMETRY ★ START HERE
**MILESTONE (screenshot-verified):** the cooked childhood level (`chapter2slums`) renders UPRIGHT
buildings in the D3D12 World state. Full story + repro + next steps: memory
`fable2native-level-cook-renders` + [`docs/NATIVE_LEVEL_COOK_PLAN.md`](NATIVE_LEVEL_COOK_PLAN.md).
Commit `e16428d`.
- **Cook:** extract the nested body bnk `worlds\albion\bwsslums\chapter2slums\chapter2slums_models.bnk`
  from `data/streaming.bnk` (via BNKReader), then
  `python Fable2Native/tools/cook_levels.py <chapter2slums.engine_level> --cook out.f2scene
  --header-bnk Fable2Recomp/assets/game/data/Globals/globals_model_headers.bnk
  --body-bnk <extracted chapter2slums_models.bnk> --types 2`. Glue recipe = `ghidra_out/model_glue_lmp_format.txt`.
- **Run:** `f2native_frontend.exe --start-world --scene out.f2scene --game-dir <assets/game>`
  (`--start-world` = new test flag). Debug layer: env `FABLE2NATIVE_D3D_DEBUG=1` → `d3d_debug.log`.
- **✅ DEPTH + LIGHTING DONE (2026-08-10, D3D12, screenshot-verified):** (a)+(c) shipped together — depth
  occlusion is invisible on flat-grey surfaces, so both were needed to turn the grey silhouette into real
  3D (Fairfax castle/towers, townhouses, bridge/wall). D32 DSV in `native_frontend_app.cpp` (World-only
  bind); per-vertex normal + sun lambert in `native_world_renderer.cpp`. Debug layer clean. Details in
  `NATIVE_LEVEL_COOK_PLAN.md` + memory `fable2native-level-cook-renders`.
- **✅ ALBEDO TEXTURES DONE (2026-08-10, D3D12, screenshot-verified):** (b) partially — buildings whose
  albedo is in `globals_textures.bnk` (comp-1 LhTex) now render textured (brick/timber/stone). The decoder
  (`f2native_cook_lh_tex`) + runtime DDS loader already existed; wired `cook_levels.py _cook_textures()` to
  extract+cook each albedo `.tex`→DDS and emit an absolute loose-DDS path (no renderer change). 15/27
  chapter2slums albedos cook; the rest live in comp-7 tiled `1024mip0_textures.bnk` (+ header bnk). See
  `NATIVE_LEVEL_COOK_PLAN.md` (b).
- **★ FULL 2026-08-10 SESSION (supersedes the two lines above — screenshot-verified, each grounded in a
  validated ghidra_out spec, NO guessing):** on top of depth+lighting, also shipped: **43/43 albedo textures**
  (found the shared_6281/2445 banks via level.vfsconfig + fixed the space-name bug); **foliage** grass/trees
  (MDL stride decode) + alpha cutout; **terrain** ground (heightfield→mesh); **normal-mapped shading**
  (comp-3 BC5 cook → 2-SRV/material → derivative-TBN); **child hero** in the scene; **procedural sky**
  (gradient default / atmosphere toggle); and the **title reveal matched to retail** frame-by-frame
  (objective per-frame pixel A/B via FABLE2NATIVE_TITLE_TIME). Full detail: memory
  `fable2native-level-cook-renders` (2026-08-10 block) + `NATIVE_LEVEL_COOK_PLAN.md`.
- **✅ TERRAIN GROUND ALBEDO (2026-08-11, D3D12, screenshot-verified):** OPEN ITEM 2 cause (1) fixed —
  the terrain was a blown-out WHITE plane (no albedo) making the textured buildings read "dark by
  contrast". `cook_levels.py` now cooks the level's DOMINANT ground texture from the `.ehf` splat map
  (new `f2tool ehf` dump → `_terrain_ground_texture()` picks the most-painted LOD; chapter2slums =
  `cobbles_curvy_dirt` @91%), tiled at its LOD base_scale. New flag `--terrain-ehf`. Details +
  Rung-2 (full splat composite) in `NATIVE_LEVEL_COOK_PLAN.md` OPEN ITEM 2. ⚠ `f2tool` gained an `ehf`
  subcommand (source in `Fable2AssetBrowser`, untracked like the other dump tools — rebuild via cmake).
- **✅ TERRAIN SPLAT COMPOSITE / RUNG 2 (2026-08-11, D3D12, screenshot-verified):** terrain now bakes the
  game's full ground painting (grass/rock/dirt/cobble/path regions blended by the `.ehf` splat map), not
  one tiled texture — default when `--terrain-ehf` given. New baker `Fable2Native/tools/terrain_splat_bake.cpp`
  (verbatim port of AssetBrowser `LevelLoader.cpp` bake-composite) + `cook_levels.py _terrain_splat_composite()`
  with whole-terrain normalized UVs. Full detail + the ⚠ stale-Release-exe/white-terrain gotcha (uncompressed
  RGBA8 DDS needs the post-2026-08-10 frontend build) in `NATIVE_LEVEL_COOK_PLAN.md` OPEN ITEM 2.
- **NEXT (specs in hand):** Phase-1 sky colour match (reads brown); WATER (`water_system_re.txt`); SPEC maps
  (t2; cooker already emits material=); exact hero PlayerStart XYZ; foliage LOD/wind; terrain .ehf splat;
  frontend fidelity drifts (`frontend_visual_fidelity_re.txt` P3-5); Vulkan world-renderer parity.
- New decomp specs this session: `ghidra_out/{model_glue_lmp_format,gdb_component_schemas,worldmap_travel_minigames,ingame_menu_live_achievements}.txt`.

## ▶▶ ACTIVE TRACK (2026-08-06) — RETAIL FRONTEND DECOMP → Fable2Native fidelity

**Directive (user):** the Fable2Native frontend (UI setup/positioning, behaviour, animations,
movies, effects, sounds) is being rebuilt from the **retail game decompilation FIRST**, not by
approximation. Track = "both, staged" (recomp stays the playable oracle; Fable2Native is the shipping
target). The XEX/PPC decomp is the PRIMARY source; the GUI LuaQ scripts are a thin event-firing top.

**★ Read first:** [`docs/RETAIL_FRONTEND_SPEC.md`](RETAIL_FRONTEND_SPEC.md) — the consolidated,
evidence-cited spec (state machine, input, movies, title, menu layout/animation, options, save/load,
sound, loading) with exact addresses + the reusable RE machinery. Also memory
`fable2-retail-frontend-re-workflow` and [`FRONTEND_FULL_PASS_NEXT_SESSION.md`](FRONTEND_FULL_PASS_NEXT_SESSION.md).

**Machinery proven this session (all repeatable):**
- Ghidra headless decompile: `& "D:\Subuwu\tools\ghidra-public\support\analyzeHeadless.bat" "D:\Documents\Fable2RE\ghidra_proj" Fable2_TU1 -process -noanalysis -readOnly -scriptPath "D:\Documents\Fable2RE\tools\ghidra_label" -postScript DecompFuncs.java 0xADDR …` (program=`default_tu1.xex`; ~30s/run).
- Discovery: `FindStrHits.java <keywords…>` (string→xref), `DumpFuncsInRange.java lo hi`.
- LuaQ: `python tools\lua_mod\script_index.py --bnk Fable2Recomp\assets\game\data\guiscripts.bnk {list|search|disasm <name>}`.
- Whole-game PPC→C++ cross-ref: `Fable2Recomp/generated/Fable2_recomp.*.cpp` (299 MB; `sub_<addr>`).
- Launch the REAL native frontend (not the black `--ui-only` placeholder): `f2native_frontend.exe --game-dir "D:\Documents\Fable2RE\Fable2Recomp\assets\game" --ui-root "D:\Documents\Fable2RE\ghidra_out\title_ui_re" --skip-intro [--start-menu]`. Screenshot flip-model window with `PrintWindow(hwnd,hdc,2)`, NOT GDI CopyFromScreen.

**NEXT (open GAPs, all exe/PPC — see spec §11):**
1. ✅ **DONE 2026-08-07** — movie player `0x823C0D90` full body decompiled (spec §3): one-at-a-time
   `mgr+0x2c` guard, `mgr+0xc` state, movies at `GAME:\videos\<name>` (fallbacks `game:\data\`,`epi:\`),
   player obj `*(*(DAT_83496ab8+0xc)+0x8c)` activated via `822C3F50`+`82182C30`. **Sub-gaps CLOSED
   2026-08-07** (`frontend_movie_statemachine.txt`): state `mgr+0x2c` `0→1→2→3→4→5→0`; GetState
   9=PLAYING/10=FINISHING; duration=`.bik` frames/30; skip=`setter(mgr,5)`; serialized one-at-a-time queue.
2. ✅ **DONE 2026-08-07 (incl. literal names)** — frontend menu **sound**: fired via `82371E80(out,
   sndmgr, &slot)`→`82265220`→`82c028e8`(lvl3)→`82C05EF8`; slots are `lh_string`s filled at boot with
   `SE_GUI_*` names. Nav-up=`SE_GUI_SLIDE_MENU_UP`(DD8), nav-down=`SE_GUI_SLIDE_MENU_DOWN`(DDC),
   select-A=`SE_GUI_MENU_BOX_SELECT`(E50), cancel-B=`SE_GUI_MENU_BOX_CANCEL`(E4C); full `SE_GUI_*`
   table @`0x820B0700` (spec §8). No runtime breakpoint needed — recovered statically.
3. ✅ **DONE 2026-08-07** — CFrontEndManager map: `frontend_mgr_funcs.txt` + real bodies decompiled
   (`frontend_mgr_bodies.txt`): dispatch table `826D8008` (stride 0x18, key +0x10), input `826D5480`,
   accept `826D5AF0`, save/load `826D9620`/`826DA038`, gender `826E0D60`, OUTRO-swap `826E13F0` (spec §1).
4. ✅ **DONE 2026-08-07** — `QuitToFrontEnd`: trampoline `823BB790` → real body `0x82312530` (sets
   `subsystem+0x104=1`, resets gameflow) (`frontend_quit_real.txt`).
5. ✅ **DONE 2026-08-07** — `gui.adb` format reversed + `tools/parse_gui_adb.py` (`gui_adb_format.txt`);
   event→wav not closable from adb+bnk alone (sample GUIDs). CFrontEndManager full method map
   (`frontend_mgr_methodmap.txt`).

**Reusable tooling added:** `tools/ghidra_label/ClearNoReturn.java` + `ReformDecomp.java` — recover
frontend functions Ghidra truncated via a `savegprlr` helper mis-flagged noReturn. Recipe:
`ClearNoReturn.java 0x<helper>` then `ReformDecomp.java 0x<start> 0x<end>` in **write mode (no `-readOnly`)**.

New raw dumps: `ghidra_out/frontend_{mgr_funcs,mgr_bodies,movie_player_decomp,movie_realbody_decomp,movie_tramp_disasm,movie_reform,movie_strings,sound_callers,sound_globals,sound_init,sound_ctor,sound_slotfill,sound_names,quit}.txt`.
2026-08-07 additions: `ghidra_out/frontend_movie_statemachine.txt` (+ `movie_tick_reform`, `movie_elem_reform`
= the reformed tick/driver), `frontend_quit_real.txt` (real quit body `0x82312530`), `frontend_mgr_methodmap.txt`
(full `0x826D4000–0x826E2000` map) + `frontend_mgr_bodies2.txt`, `gui_adb_format.txt` + `tools/parse_gui_adb.py`.

### NATIVE FRONTEND — asset pipeline DONE (2026-08-07)
- **GUI audio wired + base-game-cooked** (spec §8, memory `fable2-native-asset-cook-pipeline`):
  `NativeFrontendSound` keyed to `SE_GUI_*`; `tools/cook_gui_audio.py` decodes the user's `gui.bnk`
  XMA2 → PCM (48 kHz) into `native_audio/` + manifest. Runtime default audio-root = `<ui-root>/native_audio`.
- **Cookers wired into the installer**: `f2native_installer --iso <iso> --out <dir>` OR
  `--game <extracted-dir> [--package <dir>]` extracts+validates then runs the offline cookers
  (`native_cook.cpp` → `cook_videos.py` + `cook_gui_audio.py`) → user-local package
  (`videos/` + `native_audio/`). Launch runtime with `--ui-root <package>`. Ships no assets.

### ▶ NATIVE FRONTEND — REMAINING decomp/auto-RE to FINISH (tracked so it's not forgotten)
Evidence layers: Ghidra headless (`DecompFuncs`/`ReformDecomp`), the 299 MB generated PPC→C++, the
recomp oracle, and the `auto-re-agent/` (AI+Ghidra) for bulk method sweeps.
1. ✅ **DONE 2026-08-07** — Movie sub-gaps (§3, `ghidra_out/frontend_movie_statemachine.txt`): reformed
   the tick `MoviePlayer_FrameUpdate@0x823C1658` + driver `0x822A9D60` + setter `0x823C1CB0`. Manager
   state `mgr+0x2c` = `0 IDLE→1 QUEUED→2 START→3 PLAYING→4 FINISHED→5 STOP→0`. **GetState 9=PLAYING /
   10=FINISHING** (the GUI movie element, matching manager 3 / 4→5; 10 is literally the stop code).
   **Duration** = `.bik` numFrames/30. **Skip** = forced `setter(mgr,5)` (`0x822A9F18`). **Sequence** =
   serialized one-at-a-time queue (PlayMovie refuses while `mgr+0x2c!=0`; order in caller). Native wiring
   already matches.
2. ✅ **DONE 2026-08-07** — `QuitToFrontEnd` (`ghidra_out/frontend_quit_real.txt`): `823BB790` is a
   trampoline → **real body `0x82312530`** (logs `QuitToFrontEnd(%s)`, flushes Live stats, dismisses the
   current screen, **sets `subsystem+0x104=1`** = return-to-frontend request, resets gameflow via
   `82312DD0`; optional target name stashed at `*(*(DAT_83496ab8+8)+0x24)+0x10`).
3. ✅ **DONE 2026-08-07** — `gui.adb` parsed (`tools/parse_gui_adb.py` + `ghidra_out/gui_adb_format.txt`):
   format reversed (18-byte event records `[FNV1(event)][sampleDefHash][type]`, name table, `Waveforms`
   section); 97 events + all frontend-6 resolved. **LIMIT (measured):** `sampleDefHash` is a sound-build
   GUID, not a `.wav`-name hash → event→wav not closable from adb+bnk alone; cooker map stays as the
   working approximation. Correction noted: the 4 move events have 4 distinct defs (recommendation only,
   `cook_gui_audio.py` NOT edited).
4. **Options page population/apply** (§6) — native owners for each setting; Resolution/AA → real
   swapchain/renderer backend (currently memory-only labels).
5. ✅ **DONE 2026-08-07** — CFrontEndManager method map complete (`ghidra_out/frontend_mgr_methodmap.txt`,
   spec §1): every real body in `0x826D4000–0x826E2000` decompiled + role-labelled (confirm `826D5914`,
   cancel `826D5A60`, screen tick `826D5D00`, hash-insert `826D6AF8`, dtors, singleton bind `826D9FC8`,
   screen-ready `826E1C50`, sound LUT `826D4808`). Remaining `0x8` entries are tail-call thunks.
6. **Save/Load card-fan + new-game gender** (§7) — native behavior parity + verify against the RE.
7. **Frontend music** (`menu_interlude`) — identify its base-game bank + add a cooker step.
8. **Visual/BGF cooker** (the big visual-parity piece): textures from `gui_textures.bnk` + layout/effects
   from the BGF hierarchy & PM4 draw-truth (`RETAIL_FRONTEND_DRAW_TRUTH.md`, `TITLE_SCREEN_FIDELITY.md`)
   → one authoritative render path, verified numerically vs `resources/options/*.png` + title captures.

---

## ▶▶ NATIVE RENDERER TRACK (2026-08-01 midday) — black loaded world (recomp oracle; paused behind frontend work)
**Bug:** native (`--gpu_plugin native`) loaded world renders BLACK (UI+bloom only); xenos renders it fully lit.
**Frontier (evidence-locked this session):** the ~232 material draws WRITE a dark HDR (`0x19C67000` peak 1.06/mean 0.08); inputs (textures, float constants, coverage) + resolve + compositor + exposure/LUT are ALL proven fine. **#1 lead: native's Xenos→DXBC PREDICATE/control-flow translation likely leaves the exported color register `r8` at its `l(0,0,0,0)` init → black** (the lit color is written only inside `(p0)` blocks; ucode `setp_ne_push`/`setp_gt_push`/`kill_gt`/`(!p0) jmp L19`). Details: RESULT block below + memory `fable2-blackworld-compositor-exposure` UPDATE 16 + ADDENDUM.

**DO THIS NEXT (pick one; anti-circle rule: every hypothesis dies to a native-vs-xenos MEASUREMENT, never an eyeball):**
1. **Cheap A/B (autonomous, PIL-measurable):** in the Xenos→DXBC translator, force the material pixel predicate TRUE (or force `r8` nonzero) for 1120-wide HDR draws → rebuild → screenshot → PIL brightness. World lights ⇒ predicate/kill translation IS the bug; still black ⇒ lighting MATH is dark (audit dp3/log/exp/_sat translation). Predicate emission lives in `rexglue-src/src/graphics/pipeline/shader/dxbc_translator*.cpp` (search `setp`, `predicate`, `kill`, `SetExecConditionals`).
2. **RenderDoc pixel-debug** one 1120-wide native material draw (PS key `98B4F32B94897A9C`): is `r8`=0 at export (predicate skipped lighting) or dim-nonzero (math dark)?
3. Audit `setp_ne_push`/`setp_gt_push`/`kill_gt`/predicated-`exec`/`(!p0) jmp` translation vs Xenia for ucode `D613E8F33B9FB891`.

**EXACT COMMANDS:**
- Build native DLL: `cmd /c D:\Documents\Fable2RE\rexglue-src\build_native.cmd` (→ `BUILD_EXIT=0`, outputs `rexglue-src/out/win-amd64/Release/rexgpu-native.dll`).
- Stage it: `Copy-Item rexglue-src/out/win-amd64/Release/rexgpu-native.dll Fable2Recomp/out/build/win-amd64-nightly/rexgpu-native.dll -Force`.
- Run the black repro (reaches loaded world, ~150s): `Fable2Recomp/tools/artifact_repro.ps1 -Arm loadsave -GpuPlugin native -TargetMorphs 48976 -Seconds 150` (exit-1 at the very end is a cosmetic format error, ignore; check the newest `Fable2Recomp/out/build/win-amd64-nightly/logs/Fable2_*.log`). Env vars set in the SAME PowerShell call reach the game (Start-Process inherits parent env; do NOT split across tool calls).
- Screenshot+measure: `tools/winshot_confirm.ps1` (captures the Fable2 window) + PIL mean of the gameplay region — numbers, not eyeball. **Important:** the normal capture path calls `ShowWindow`/`SetForegroundWindow` before `CopyFromScreen`; this changes window visibility/activation and is not a passive renderer observation. Use `-NoActivate` to capture the desktop/window rectangle without foregrounding, or use the repro harness, which now explicitly focuses Fable2 at launch so visual runs are deterministic.

**DIAGNOSTICS SHIPPED THIS SESSION (all default-OFF, env-gated; DLL staged, normal runs unchanged):**
- `REXGPU_NATIVE_DUMP_PS_KEY=<hex translator_cache_key>` + `REXGPU_NATIVE_COMPOSITOR_DUMP_PATH=<dir>` → dumps that PS's ucode+DXBC (code ~`d3d12_pm4_backend.cpp:2210`).
- `REXGPU_NATIVE_HDR_INPUT_TRACE=1` now also dumps the full gathered PS float-constant vector for `DUMP_PS_KEY` (`"HDR input PSCONST"` lines, code ~`:3958`).
- Artifacts already on disk: `D:\Documents\Fable2RE\material_shader_dump\` (ucode.frag + `shader_D613E8F33B9FB891.dxbc.disasm.txt`, 1373 lines). Disassemble more DXBC via the D3DDisassemble PowerShell P/Invoke (pattern in this session's transcript).

**CODE/BUILD STATE:** `rexglue-src` has UNCOMMITTED changes (the two default-OFF diagnostics above in `src/graphics/native/d3d12_pm4_backend.cpp` + `dumped_material_shader_keys_` member in `.h`). No hardwires; nothing to revert. Staged nightly DLL is the clean-with-diagnostics build (Aug 1 09:46). `git status` in `rexglue-src` (untracked `src/graphics/native/`).

## ▶▶▶ RESULT (2026-08-01 midday) — frontier EVIDENCE-LOCKED to MATERIAL SHADER EXECUTION; RenderDoc-vs-in-engine contradiction CLOSED
Full detail in memory `fable2-blackworld-compositor-exposure` **UPDATE 16**. Consolidation this session (static code + log audit, no new run):
- The in-engine compositor-bind readback is FORMAT-CORRECT — bound HDR `fmt=10` (R16G16B16A16_FLOAT), proper half_to_float → **maxRGB=1.0605, meanMaxRGB=0.0811 is a REAL measurement**, not a UNORM-clamp artifact. So RenderDoc's 0.317/16 (UPDATE 10) was a NON-representative frame; steady-state resolved `0x19C67000` is genuinely dark.
- The HDR resolve (`source_format=3`=k_2_10_10_10_FLOAT 7e3, 2x MSAA) uses `ResolveSubresourceRegion(...AVERAGE)`/CopyResource, which **cannot** turn peak-16 samples into peak-1.06 ⇒ **the 232 material draws WROTE a dark HDR into EDRAM.** Resolve exonerated.
- Inputs HEALTHY (HDR_INPUT_TRACE, Fable2_1409): albedo/normal fmt49 128x128 `zero_words=0/4096`; GPU targets bound; ps_constants real & nonzero (0.8/1.0/~310). 232 textured / 0 textureless / 0 missing.
- Ruled out STATICALLY: color_exp_bias (draws carry 0), PSI FixedPointColor flag (7e3 correctly float, cache.h:112), 7e3 ROV pre-clamp (`clamp_rgb_high=31.875f`, cache.cpp:79), AND native doesn't use ROV at all — direct oC0 export to a real RGBA16F RT (`draw_render_target_format_`), so NO output saturate.
⇒ **ROOT CAUSE narrowed & evidence-locked: native's translated MATERIAL PIXEL SHADERS compute ~15x-too-dark lighting despite correct inputs & no clamp.** Candidates: (a) DXBC translation defect in a heavily-used lighting instruction; (b) a light-constant VALUE wrong (count right, values unchecked); (c) blend / retained-draw replay ORDER overwrites lit content. Single-pass-forward evidence (each draw = distinct PS + 8–11 textures, not additive) favors (a). Dominant material PS keys: 98B4F32B94897A9C (8 tex), 393DF45D685C7632, 512F234B32F719B4, 7FF0E3219C7B980C.
**OPTION A EXECUTED this session → new #1 lead = PREDICATE/control-flow translation leaves the exported color register at ZERO.** Shipped two default-OFF diagnostics (rebuilt clean): `REXGPU_NATIVE_DUMP_PS_KEY`=<hex translator_cache_key> dumps that PS's ucode+DXBC to `REXGPU_NATIVE_COMPOSITOR_DUMP_PATH` (~d3d12_pm4_backend.cpp:2210); and a full gathered pixel-float-constant dump for the target PS inside HDR_INPUT_TRACE (~:3958, "HDR input PSCONST"). Dumped the dominant material PS `98B4F32B94897A9C` (ucode `D613E8F33B9FB891`, normal-map+specular+cubemap forward-lighting) to `material_shader_dump/` and disassembled the DXBC (D3DDisassemble P/Invoke → `.dxbc.disasm.txt`). Results: **(1) float lighting constants are HEALTHY** (Fable2_1412 PSCONST: c19={310.5,52.2,100.7,1}, c20={0.8,0.8,1,1}, normalized light dirs, no zero/NaN) — closes UPDATE 8's "values unchecked" gap; **(2) DXBC export is faithful** (`mul r8,r8,CB0[0][15].x; mov o0,r8`; CB0=Xenia SYSTEM cbuffer, [15]=color_exp_bias≈1.0 for bias-0 draws, NOT the darkener); **(3) ★ the lead** — DXBC line 168 `mov r8.xyzw,l(0,0,0,0)` initializes the exported color to ZERO and the lit value is written to r8/r2 ONLY inside PREDICATED blocks (ucode instrs 19-87 nearly all `(p0)`; p0 from setp_ne_push/setp_gt_push/kill_gt + `(!p0) jmp L19`). **If native's Xenos→DXBC translation of the predicate/kill/`(!p0) jmp` makes p0 false, the lighting is skipped → r8 stays 0 → black**, with correct inputs & scale — fits the symptom exactly. NEXT (runtime shader debug): (i) RenderDoc pixel-debug one 1120-wide material draw on native — is r8=0 (predicate skipped lighting) or dim-nonzero (lighting math)? (ii) cheap A/B: force the material predicate true / force r8 nonzero in the native DXBC translation, rebuild, screenshot — world lights ⇒ predicate translation bug; (iii) audit setp_ne_push/setp_gt_push/kill_gt/predicated-exec translation vs Xenia. DEAD (do not revisit): constant values, textures, exposure/LUT/tfetch1D, compositor bind/rebind, resolve gen/exp_bias/7e3-clamp, coverage, depth. Full detail: memory UPDATE 16 + ADDENDUM.

## ▶▶▶ RESULT (2026-08-01 morning) — tf1 measured; HDR is already dark at resolve
The decisive compositor-bind measurement is now implemented in
`rexglue-src/src/graphics/native/d3d12_pm4_backend.cpp`, gated by
`REXGPU_NATIVE_COMPOSITOR_BINDING_TRACE=1`. It reads the exact bound `tf1` host resource when
already resident, and records the converted upload, resource identity, cache hash, and LUT texels
10/40 for first-use resources.

Hero001 loaded-world repro (`Fable2_1403.log`) aligned the inputs at the same final compositor draw
(sequence 652958):
- **tf0 HDR**: bound identity 1999, peak RGB ≈1.0605, meanMaxRGB ≈0.08109. This is real but dark,
  not the RenderDoc resolved-scene measurement (mean ≈0.317, peak ≈16).
- **tf1 exposure LUT**: new bound identity 2002 was `pending_upload=true`; the converted upload for
  that exact draw was finite 512/512 with max ≈0.14954 and mean ≈0.13559. Earlier resident readbacks
  (identity 1735, max ≈2.58e-5) were an earlier generation and must not be mistaken for the final
  draw's input.
- `REXGPU_NATIVE_COMPOSITOR_REBIND=1` is not a fix: the bound HDR identity equals the freshest
  `resolved_targets_[0x19C67000]` identity. The freshest entry itself is dark.

**Conclusion:** LUT stale-content is not the final gate. The native black-world frontier is now the
HDR resolve/generation/content path that produces or exposes `0x19C67000` to the compositor. Compare
the resolve-time resource identity/content against the compositor-bound identity/content at the same
sequence; do not return to tfetch1D or exposure theories without a new measurement. The latest staged
diagnostic DLL SHA-256 begins `4846CFD597023D...`.

The ordering ambiguity is now closed by `Fable2_1405.log` with
`REXGPU_NATIVE_DEBUG_RESOLVE_BASE=19C67000` and min sequence 650000: resolve sequence 650499
created identity 2001, and compositor sequence 650817 bound identity 2001 with
`identity_matches_readback=true`; the bound HDR still measured meanMaxRGB ≈0.08109. At that same
compositor draw the converted tf1 upload was finite 512/512, max ≈0.14954, `idx10≈0.0745`, and
`idx40≈0.0966`.

**Refined conclusion:** LUT sampling/binding and compositor resource generation are not the current
gate. The native black-world frontier is upstream: native HDR scene rendering/lighting produces an
under-lit `0x19C67000` before tonemap. Trace native-vs-xenos HDR material/light draws and retained-draw
coverage; do not return to tfetch1D or exposure theories without new evidence.

The next coverage/input pass is now measured (`Fable2_1408.log`, `Fable2_1409.log`): the loaded-world
HDR resolve has **232 textured draws, 0 textureless draws, 232 default-submittable draws, and no
missing texture descriptors**. The retained HDR input trace shows finite/nonzero representative PS
constants and nonzero raw DXN normal-map snapshots. `REXGPU_NATIVE_KEEP_TEXTURELESS=1` therefore has
no effect on this repro. An env-gated DXN `BC5_SNORM` A/B (`Fable2_1410.log`) did not brighten the
scene (meanMaxRGB was ~0.074 versus the ~0.081 baseline), so normal-map signedness is not the dominant
gate. The remaining frontier is native textured material execution/color semantics (shader output,
blend, or HDR render-target format expansion), not retained draw coverage or compositor binding.

New diagnostics are opt-in: `REXGPU_NATIVE_HDR_RESOLVE_TRACE=1`,
`REXGPU_NATIVE_HDR_INPUT_TRACE=1`, and `REXGPU_NATIVE_DXN_SNORM=1`. They are diagnostic only; the
default DXN view remains BC5_UNORM and no hardwired exposure behavior is enabled.

## ▶▶▶ RESUME (2026-07-31 late night) — read memory `fable2-blackworld-compositor-exposure` UPDATE 14 FIRST.
Black world still unsolved but SHARPENED + a decisive test is IN FLIGHT. Fast facts:
- Loaded-world exposure LUT (in-engine upload readback) = real rising curve: index0=0, index1=0.048 … max 0.1495. Output
  black ⇒ effective exposure ~6e-5 = below index1 ⇒ shader effectively samples ~INDEX 0. Points at a tfetch1D COORD bug —
  BUT our translator is a faithful Xenia port that statically looks correct (2 surviving 1D traps: unnormalized_coordinates
  flag, or wrong 1D size-field read). See UPDATE 14 for exact file:lines.
- DECISIVE TEST RUNNING: LUT hardwired to a CONSTANT 8.0 (coordinate-independent). BRIGHT scene ⇒ exposure is the crusher,
  fix the tfetch1D index-0 coord. STILL BLACK ⇒ the HDR into the tonemap tf0 is itself ~0 (not exposure). Measure the
  screenshot NUMERICALLY (PIL mean of gameplay region) — do NOT eyeball, bloom fools the eye.
- ⚠CODE: rexglue-src has a TEMP HARDWIRE `exposure_ramp_ = true;` (~d3d12_pm4_backend.cpp:1724) + the ramp branch writes
  const 8.0 (~:8900). REVERT after the test. KEEP the stale-snapshot per-range fix at :3543. Env diag flags don't reach
  the game through winshot's Start-Job — hardwire or launch artifact_repro directly.
- Xenia GummiFableII + femtofork = NO tonemap fix ⇒ our black is a native-backend bug, not a Fable2 quirk.

## ▶▶ START HERE (2026-07-31 — EXTERNAL RESET: the shader translator is a CLEAN Xenia port; stop blaming tfetch1D)

**Read this before touching the LUT/tonemap code again.** An outside research pass (4 web tasks +
Xenia/XenosRecomp source + a fresh audit of OUR actual code) overturns the framing of the last ~50 runs.
The recycled premise "native reads exposure-LUT index ~0 because of a broken tfetch1D coordinate" is an
**inference from arithmetic, never a measurement** — and the code audit shows it is almost certainly WRONG.

### What the code audit actually found (file:line, all in `rexglue-src/`)
Our native backend's shader translator is a **faithful Triang3l/Xenia `dxbc_shader_translator_fetch`
port**, NOT the XenosRecomp AOT translator the web research assumed. Every "smoking gun" the external
tasks proposed is ALREADY correctly implemented here:

- **tfetch1D coordinate is correct.** `src/graphics/pipeline/shader/dxbc_translator_fetch.cpp`:
  k1D normalizes X only (`normalized_components=0b0001`, line 1010), pads Y/Z to 0.0 (line 1149-1151),
  applies the `OffsetX=0.5` half-texel + `1.5/1024` rounding epsilon (line 715-716), and reads width
  from **fetch-constant word 2 as a 24-bit UBFE(24,0)** (line 841) — exactly Xenia's k1D path, NOT the
  13-bit 2D layout. Sampling is `OpSampleL`/`OpSampleD` at explicit LOD (line 1836/1841), so no
  tail-mip / LOD-clamp collapse. **This refutes Web Tasks A/B/D's core hypothesis.**
- **exp_adjust (the strongest external lead, Task C) is ALREADY applied.** Compile-time constant path
  `dxbc_translator_fetch.cpp:437-439` (`std::ldexp(1, exp_adjust)`), runtime path lines 1956-1963
  (`IBFE(6,13)` → `<<23` → `OpMul` into the result), plus the SPIR-V and interpreter mirrors. Task C's
  "XenosRecomp drops 2^exp_adjust" does NOT apply to us — we are the Xenia lineage. **Refuted in-code.**
- **SRV + resource are correct.** `k_32_FLOAT` → resource `R32_TYPELESS` (d3d12_pm4_backend.cpp:8552)
  → SRV `R32_FLOAT` (line 1056-1057) → `TEXTURE1DARRAY, ArraySize=1` (line 1092-1095). 1D resource is
  `D3D12_RESOURCE_DIMENSION_TEXTURE1D, Width=512, Height=1` (line 8509-8513) — untiled, height-1, exactly
  what Xenia's `texture_util` mandates. **Refutes the "2D-tiling of the 1D LUT" upload hypothesis.**
- **Fetch constants reach the shader as REAL guest raw dwords**, copied at `slot*6` dword stride
  (d3d12_pm4_backend.cpp:3156) into the shader-visible CB (line 9166-9168). The shader's
  `RequestTextureFetchConstantWord(tf1_index, 2)` therefore reads the genuine guest width.

**Conclusion: the bug is NOT in shader-side 1D-fetch translation.** Chasing tfetch1D/exp_adjust/tiling
further is a 4th dead end. The arithmetic ("effective exposure ~200x below LUT[0.075]") is real, but its
attribution to "samples index 0" is unproven and the code makes it unlikely.

### #1 ROOT-CAUSE HYPOTHESIS (fresh, externally-grounded)
**The LUT texture the compositor SAMPLES is not the LUT that was measured in RenderDoc — it is a
different generation/instance of the 0x1FC20000 host-texture entry whose CONTENT is near-zero (an early
or stale snapshot), while RenderDoc captured the good one.** I.e. the failure is in the **host-texture
cache identity / generation for the CPU-written LUT**, one level ABOVE the shader:
- The LUT at 0x1FC20000 is **CPU-written every frame** by the recompiled game, then snapshotted into a
  host texture and cached by `content_hash` (`host_texture_cache_` keyed on `source.cache_key`,
  d3d12_pm4_backend.cpp:8485). The HDR (0x19C67000) is a GPU `resolved_target` and is provably FRESH.
  These are two DIFFERENT binding paths — the team verified the HDR path but has **never** verified which
  LUT *entry* the tonemap draw actually binds.
- A snapshot taken before the game has written the frame's exposure curve (or a cache entry keyed to a
  stale hash / a `ready==false` entry that slipped the eviction at line 8486) would hold the near-zero
  LOW end of the curve → `exposure ≈ LUT[0]` ≈ 6e-5 → post-tonemap 0.0072 → black. This reproduces the
  EXACT arithmetic **without any coordinate bug** — the coord is fine, the *texels it lands on are the
  near-zero ones because those are the only texels this particular cached copy contains.*
- This is consistent with the one measurement gap the team admits: RenderDoc read the LUT resource it
  found in the capture; nobody has confirmed that resource is the SRV bound at the tonemap draw, nor
  dumped the sampled value in-engine.

### 2. THE DECISIVE EXPERIMENT (do this first — it is a MEASUREMENT, not a fix)
Add the LUT-side twin of the existing compositor-bind readback (the HDR one lives at
`d3d12_pm4_backend.cpp:8646-8778`). In the same `draw.shader_pair.pixel == kFableFinalCompositorPixelShaderKey`
block, find the bound texture with `fetch.slot == tf1` (the LUT, base 0x1FC20000), read back its 512
R32F texels, and log: (a) resource identity, (b) min/mean/max, (c) the value at the **exact sampled
coordinate** `idx = round(0.25 * mean_luminance * 512)` (~38-41). Compare that identity + curve against
the RenderDoc capture and against xenos.
- **If the bound LUT's curve is near-zero / identity ≠ the RenderDoc one** → confirmed: stale/wrong-gen
  snapshot. Fix = force the LUT host-texture entry to re-snapshot from guest RAM every frame it is bound
  as a compositor source (invalidate its cache entry, or bypass the content-hash cache for CPU-written
  compositor-input textures), mirroring how the resolved HDR stays fresh.
- **If the bound LUT curve MATCHES RenderDoc (0→0.19) AND texel[~40]≈0.011 but the pixel is still black**
  → the LUT is fine and sampled fine; the bug is the MULTIPLY, i.e. `saturate(exposure*HDR)` or a
  constant (recheck c31/c47/c77/c78 in the fuller tonemap, see note below) — a different investigation.
This single readback disambiguates "stale-snapshot" vs "sampling" vs "post-multiply" in ONE run and
ends the guessing.

### NOTE — the tonemap shader is MORE than the in-house summary
`compositor_shader_dump_1177/shader_FBE91459C01C61BF.ucode.frag` is a full filmic curve, not just
`saturate(exposure*HDR)`. It has TWO `tfetch2D` (tf0=HDR, tf2=bloom/other) + the tf1 LUT fetch, a
saturation/desaturation block (dp3 luma, `add r2,-r0.zzzz`, `mad ... c77`), a `log/mul c78/exp` gamma
tail, and constants c31, c46(=0.25), c47, c77, c78. When you rule out the LUT, the next suspects are
these constants (are `pixel_float_constants` c31/c47/c77/c78 uploaded correctly for the native draw?
they flow through d3d12_pm4_backend.cpp:9159-9163) — a zeroed/wrong c31 or c78 also yields black.

### 3. What the FORCE_EXPOSURE_ONE test (flat-1.0 LUT, currently running) will tell us
- **Result = BRIGHT/lit** → the tonemap MATH and constant plumbing are correct; the ONLY thing wrong is
  the exposure value delivered by the LUT sample. That NARROWS it to {stale LUT snapshot content} vs
  {sample coordinate} — and the decisive readback in §2 then separates those two. (Given the code audit,
  expect "stale snapshot content," not "coordinate.")
- **Result = STILL BLACK** → the exposure multiply is NOT the gate. The LUT is a red herring entirely and
  the black comes from a DOWNSTREAM constant/op in the filmic tail (c31/c77/c78, the log/exp gamma, or
  the bloom `tfetch2D tf2`) or from the HDR input to THIS shader differing from the RenderDoc-measured
  0x19C67000 (e.g. the compositor binds a different/earlier HDR generation into tf0). This would redirect
  the whole investigation off the LUT.
Either outcome is decisive — run it and record the result at the top of this section.

### 4. Ranked hypotheses (honest confidence)
1. **Stale/wrong-generation host-texture snapshot of the CPU-written LUT (sampled texels are near-zero
   low end).** ~45%. Fits the arithmetic with zero coordinate bug; the CPU-written-texture cache path is
   the one part NOT yet verified with a readback; consistent with the codebase's own admission that the
   LUT bind was never confirmed. Supported by Xenia's texture-cache invalidation model
   (https://github.com/xenia-project/xenia/blob/master/src/xenia/gpu/texture_util.cc) and the general
   CPU-written-texture write-watch problem noted in our own memory `fable2-ram-memory-model-map` (P2
   PAUSED on "game CPU-writes texture data, plain host tokens break CPU-filled textures").
2. **A tonemap CONSTANT (c31/c47/c77/c78) is zero/wrong in the native pixel float-constant upload**, so
   the filmic tail crushes to black regardless of exposure. ~20%. Cheap to check; the shader is more
   complex than the "saturate(exposure*HDR)" summary. Path: d3d12_pm4_backend.cpp:9159-9163.
3. **tf0 (HDR) bound into THIS shader is a different generation than the 0x19C67000 RenderDoc measured**
   (the compositor draw binds an earlier/darker HDR). ~15%. FORCE_EXPOSURE_ONE=still-black would point
   here.
4. **Sampler state wrong for the LUT** (addressing wraps/borders to 0, or a filter that the point-mag/min
   1D fetch doesn't expect). ~10%. Check the sampler built for tf1 (mag/min=point per the ucode) — not
   the coordinate, the D3D12_SAMPLER_DESC. Xenia ref:
   https://github.com/xenia-project/xenia/blob/master/src/xenia/gpu/dxbc_shader_translator_fetch.cc
5. **tfetch1D coordinate / exp_adjust / 1D upload tiling** (the old theory + all 4 web tasks' leads).
   ~10% and DROPPING — the code audit above shows all of these are correctly implemented as Xenia ports.
   Only revisit if §2's readback proves the sampled texel index is genuinely ~0 despite correct content.
   Refs the web pass leaned on (now shown to already be handled in our port):
   https://github.com/hedge-dev/XenosRecomp (README: 1D unimplemented — but that's XenosRecomp, NOT us),
   https://github.com/xenia-project/xenia/blob/master/src/xenia/gpu/dxbc_shader_translator_fetch.cc.

**Bottom line for the next session:** stop editing the shader translator. Add the LUT-slot readback at the
compositor draw (§2), run it alongside FORCE_EXPOSURE_ONE (§3), and let those two measurements pick the
branch. The most likely fix is "re-snapshot the CPU-written LUT every frame it's bound," not any shader change.

---

## (SUPERSEDED framing below) 2026-07-31 NIGHT — black world = exposure-LUT SAMPLING; ALL exposure-magnitude theories DEAD

**The single most useful thing to read: memory `fable2-blackworld-compositor-exposure` UPDATE 12.**
This session used RenderDoc to capture the SAME Hero001 Bowerstone scene under BOTH the native and
xenos backends and read the actual buffers. That closed a huge stack of wrong theories with hard
numbers:

- **Native HDR is FINE.** Resolved HDR `0x19C67000` (1120x720 RGBA16F): native mean 0.317 / **peak 16**
  vs xenos 0.542 / peak 32 — only ~2x dimmer, NOT the 12x we thought. (The old "native under-renders
  HDR" number came from a stale in-engine readback; RenderDoc is authoritative.)
- **HDR bind is FRESH, not stale.** In-engine diagnostic (env `REXGPU_NATIVE_COMPOSITOR_REBIND` infra,
  submit-bind ~d3d12_pm4_backend.cpp:8399): `bound_identity == freshest_identity` every frame → the
  A1 "stale resolved_target" rebind is a NO-OP for the HDR. Don't ship it.
- **The exposure LUT MATCHES xenos.** LUT `0x1FC20000` (512x1 R32F): native max ~0.15 vs xenos max
  **0.19** (read via `ghidra_out/title_ui_re/renderdoc_lut.py`). Same tone curve.
- **Final output is still BLACK** (native 0x1ECFF000 mean 0.0072) with the SAME LUT + a real HDR + the
  same tonemap shader as xenos.

⇒ **DEAD ENDS (do NOT revisit): depth/tiling, under-lit HDR, stale-HDR bind, exposure LUT
magnitude/NaN, luminance histogram/writeback, SANITIZE_EXPOSURE, memexport.** Confirmed this session:
SANITIZE_EXPOSURE=1 + LUMINANCE_WRITEBACK=1 BOTH engage and the world stays black.

**THE ACTUAL FRONTIER:** native produces a correct LUT (0.19) + real HDR (peak 16) + same shader, yet
44x darker output. So the bug is in **how native SAMPLES/BINDS the 512x1 CPU-written LUT**. Tonemap
decoded: `exposure = LUT[0.25*luminance]; color = saturate(exposure*HDR); out = pow(color, 1/2.2)`
(c77.y=1.0, not the problem). The LUT is CPU-written → bound via the host_texture_snapshot/cached_target
path (NOT resolved_target). Three suspects: (1) STALE LUT snapshot (near-zero early gen); (2) WRONG
tfetch1D sample COORDINATE (512x1 SRV dims / sampler addressing / normalized coord); (3) 1D-LUT UPLOAD
TILING scrambling the 512x1 R32F. **DECISIVE NEXT STEP: measure the compositor's ACTUAL sampled
exposure value in-engine** (or RenderDoc pixel-debug the tonemap tf1 fetch on both backends) — that
picks 1 vs 2 vs 3 and yields the fix.

**PERFORMANCE (secondary, user-reported 5-20 fps gameplay vs 60 menu):** native retained draw-proof
backend costs ~78us CPU/draw (2000 draws/frame → ~150ms → ~6fps). Causes: per-texture guest-memory
snapshot + locked generation hashing (native_graphics_system.cpp:324-396), a 1ms sleep-poll floor in
AwaitNativeSubmissionBounded (d3d12_pm4_backend.cpp:332), synchronous mid-frame readback stalls.
⚠ The fence-event-wait fix HUNG BOOT (calling `TrySignalEnqueueing()` from the passive wait corrupts
submission-signal accounting) — REVERTED. A safe version must wait passively on the fence event WITHOUT
enqueueing signals (only when the signal is already queued).

**CODE/BUILD STATE:** staged `Fable2Recomp/out/build/win-amd64-nightly/rexgpu-native.dll` SHA
`a6f31dc9` boots clean; only env-gated default-OFF diagnostics added (compositor-bind SUBMIT log +
`REXGPU_NATIVE_COMPOSITOR_REBIND`). Tracker (`d3d12_submission_tracker.*`) fully reverted to original.
Build DLL: `rexglue-src/build_native.cmd` then `cp out/win-amd64/Release/rexgpu-native.dll` over nightly.
Repro: `Fable2Recomp/tools/artifact_repro.ps1 -Arm loadsave -GpuPlugin native -TargetMorphs 48976`
(Hero001 = the black Bowerstone Cemetery). RenderDoc capture: add `-RenderDocCapture` (now has a xenos
branch). Readers: `ghidra_out/title_ui_re/renderdoc_{hdr_mean,lut,compositor}.py` via
`"C:\Program Files\RenderDoc\qrenderdoc.exe" --python`.

---

**★★★ (SUPERSEDED by START HERE above) 2026-07-31 late — EXPOSURE NaN FIXED; black world is MULTI-CAUSE:** The CPU
tonemap function was found (`sub_8217E428`, via cdb write-watch) and its NaN root cause fixed. It
divided by an exposure/curve-fit range (`this[+0x4198..]`) that collapsed to all-zero (div-by-zero
→ −NaN exposure LUT at guest `0x1FC20000` → `saturate(NaN)=0` → black) because the game's eye-
adaptation never gets a valid GPU scene-luminance measurement in our env. FIX SHIPPED:
`Fable2Recomp/src/ExposureGuard.cpp` — `REX_HOOK_RAW(sub_8217E428)` seeds the degenerate range to the
game's own ctor constants `[0,6,1,1]` (workflow decrypted the XEX to read them) + the adapted
exposure before the curve builds (env `FABLE2_EXPOSURE_GUARD=0` disables; default-on, only engages
on the degenerate/bug case). VERIFIED: the loaded-world exposure LUT is now **finite**
(`nonfinite 512→0`, ramp 0→0.15) — the NaN is gone. BUT the world is still not correctly visible:
guard-only = pure black; guard + `REXGPU_NATIVE_GAMEPLAY_DEPTH_FRESH=1` = flat blue-gray gradient
(depth-fresh overdraw), not real geometry; and the LUT magnitude (0.15) is invariant to range tuning
(driven by other params + the missing adaptation). ⇒ **The black world has ≥2 stacked causes:
(1) exposure NaN [NOW FIXED], (2) the compositor's HDR-content / resolve-tiling path (the earlier
depth frontier) and/or exposure MAGNITUDE.** REAL FIX for the remainder = populate the GPU scene-
luminance readback in rexgpu-native (the surface the game locks via `sub_821AB4E0`, left zero-filled)
so the game's own exposure computes at the right magnitude AND re-examine why the compositor's HDR
input is flat guard-only. Rebuild Fable2.exe: `/tmp/build_exe.cmd` pattern (vcvars + `cmake --build
out/build/win-amd64-nightly --target Fable2`). Tools: `tools/exposure_writewatch.ps1` (cdb
write-watch), `tools/winshot_confirm.ps1` (Fable2-window capture), `tools/ghidra_label/FixDecomp.java`.

**★ GPU LUMINANCE READBACK IMPLEMENTED (2026-07-31, env `REXGPU_NATIVE_LUMINANCE_WRITEBACK=1`):** the
CPU auto-exposure reads a small k_32_FLOAT scene-luminance downsample RTT (per-frame ping-pong,
dynamic guest addr e.g. phys 0x1A5D6000/0x1A60C000, 288x180) that our backend left zero. Generalized
the player/dog `guest_cpu_readback` in `rexglue-src/.../d3d12_pm4_backend.cpp` to write those small
k_32_FLOAT resolves back to guest RAM (tiled R32F, big-endian). VERIFIED: writeback fires, and with
`FABLE2_EXPOSURE_GUARD=0` the game's OWN exposure LUT is now **finite** (no NaN) — the clean fix works
mechanically. **BUT the world is STILL BLACK** ⇒ **the DOMINANT cause is NOT exposure** — it's the
COMPOSITOR's HDR input being flat/black at present time (the resolve-tiling/depth frontier,
`fable2-blackworld-depth-diagnosis`): a finite exposure LUT (guard OR luminance) still doesn't reveal
the scene; only sanitize(flat 1.0)+depth-fresh showed structure. **★★ COMPOSITOR-FLAT-HDR HYPOTHESIS REFUTED
(2026-07-31):** a decisive in-engine binding-trace (env `REXGPU_NATIVE_COMPOSITOR_BINDING_TRACE=1`,
logs the resource_identity + magnitude + hash of the EXACT host texture the compositor's slot-0 SRV
binds for 0x19C67000) proved the compositor binds a REAL FULL HDR entry: `nonzero_texels=806145/
806400`, `identity_matches_capture=true`, **maxRGB=1.06, meanMaxRGB=0.081, all finite**. So the
compositor is NOT reading a flat/partial/wrong-generation HDR — it reads a real but genuinely **DARK**
scene (mean 0.08). Exposure LUT max ~0.15 (invariant to range AND forced adapt=16 → still 0.149).
So `0.15*0.08≈0.01` = black scene; `0.15*1.06≈0.16` = the faint glows. **The black world is now fully
explained: a real-but-dark HDR (mean 0.08) × a too-low exposure (0.15).** ★★ RESOLVED (2026-07-31): the
XENOS ORACLE decides it — the SAME save under `-GpuPlugin xenos` renders a FULLY LIT torch-lit interior
(`xenos_same_save_reference_live.png`) while our native backend HDR is mean 0.08. Same recompiled game
⇒ **PATH A: the NATIVE backend UNDER-PRODUCES the HDR lighting (~12x too dark).** RULED OUT: exposure
NaN [fixed], compositor-flat/wrong-entry [refuted, binds real full content], HDR RT/resolve
format/scale/MSAA [full-precision RGBA16F, matches Xenia], DROPPED TEXTURELESS LIGHT PASSES [refuted:
`REXGPU_NATIVE_KEEP_TEXTURELESS=1` → meanMaxRGB unchanged 0.081]. ⇒ the MATERIAL draws that render
compute **dark forward-lighting** — most likely a LIGHT CONSTANT / lighting-input (light color/dir,
a constant buffer, a light/shadow SRV) zero-or-wrong in the native material-shader binding but correct
under xenos. NEXT: (1) confirm the number via RenderDoc on a xenos run (read the final compositor
slot-0 RGBA16F SRV meanMaxRGB, expect ~1.0); (2) diff the material-draw CONSTANT/SRV binding
native-vs-xenos to find the zero light input. Do NOT pursue exposure/tile/format/dropped-pass fixes
(all refuted). Do NOT pursue the resolve-tile/seed-replace fix (the binding
diagnostic refuted its flat/partial premise). Durable tooling: the binding trace, LUMINANCE_WRITEBACK,
`src/ExposureGuard.cpp` (+`FABLE2_EXPOSURE_FORCE_ADAPT`), `src/LuminanceDiag.cpp`. See memory
`fable2-blackworld-compositor-exposure` UPDATE 6.


**★★★ LATEST RESUME (2026-07-31, black world REDIRECTED to the COMPOSITOR/EXPOSURE —
start with [SESSION_SNAPSHOT_2026-07-31.md](SESSION_SNAPSHOT_2026-07-31.md)):**
The black loaded world is **NOT** depth / occlusion / coverage / VS-Z. Proven this session by
measurement: (1) an ultracode workflow (wb0ip8ntb) refuted VS-Z, depth-lifetime, and
depth-writeback; (2) a NEW tile-mapping diagnostic proves composite **coverage is complete**
every frame (upper `19C67000` 1120x576 + lower `1A153000` 1120x144 fold into ONE shared 720
texture via `reused=true`), refuting the coverage theory; (3) a fixed window-capture harness
(`winshot_confirm.ps1`) shows the real world = flat near-black + UI + glows only; (4)
`REXGPU_NATIVE_DEBUG_GAMEPLAY_COLOR_DEPTH_ALWAYS=1` (no rebuild) forces depth to always-pass
and the frame is **IDENTICAL** → **depth is not the gate**; (5) per-draw log shows 236/236
material draws drawn with full color-write; (6) an in-engine readback of `0x19C67000` (the
compositor's HDR input) shows a **fully populated real scene** (BMP visibly shows the Hero
character + glows; first pixel RGBA ≈ 1.0/0.56/0.97/2.93). ⇒ **The world renders correctly;
the compositor/tonemap/EXPOSURE pass crushes it to black.** **ROOT CAUSE CONFIRMED END-TO-END:**
the final-compositor auto-exposure LUT at guest `0x1FC20000` (512×1 R32_FLOAT) is **entirely
NaN** (`entries=512/512 non-finite`); the compositor microcode does `mul_sat(exposure*color)`
and `saturate(NaN)=0` zeroes the scene, leaving only bloom (= the glow orbs). A flag-gated
diagnostic `REXGPU_NATIVE_SANITIZE_EXPOSURE=1` (overwrite non-finite LUT entries with 1.0) made
scene geometry APPEAR (`win_v1_t203_t203.png`), confirming the chain. **NEXT = the REAL fix (see UPDATE 2 in
[SESSION_SNAPSHOT_2026-07-31.md](SESSION_SNAPSHOT_2026-07-31.md)):** memexport was investigated and
RULED OUT as the exposure producer — the only memexport shader (a VS) exports float4 transform data
to `0x1BBF`–`0x1BC2`, never `0x1FC20000` (memexport IS dropped by the backend though — a real
separate gap). The exposure LUT `0x1FC20000` is **written every frame by the recompiled game CPU**
(its memory generation increments per frame; no GPU op targets it) **with NaN** — so the CPU
tonemap/exposure producer RUNS but computes NaN (−NaN uniform curve = a divide-by-zero, most likely
from a zero/NaN input). **The CPU tonemap function is now FOUND = guest `sub_8217E428`** (via cdb
write-watch `tools/exposure_writewatch.ps1`; cdb IS installed at the WinDbg WindowsApps path). It
writes the exposure LUT (−NaN) per frame, driven by `sub_821C3858` (MorphFillPump) → … →
`sub_822767E0` → `sub_8217E428`. It reads float exposure params from its `this` object (r3) and
does 17 `fdivs`; the −NaN curve is a **division by degenerate/uninitialized params** (e.g.
`const / this[+268]`, and `… / (this[+16800]-this[+16792])²`). REAL FIX NEXT = dump those `this`
floats at the store to find which param is 0/NaN and its source, then fix it. Stopgap =
`REXGPU_NATIVE_SANITIZE_EXPOSURE=1`. See memory `fable2-blackworld-compositor-exposure` UPDATE 3.
**Stop working depth.** Staged DLL SHA-256
`DBA78A30087EDA0822DC15330A86E1F3B04956D7EDCDFBB6ACDCA143BC873736` (adds only default-OFF/log-only
diagnostics; normal runs unchanged).

**PRIOR RESUME (2026-07-30 evening, black world = EDRAM TILING; GEOMETRY NOW
APPEARS — SUPERSEDED by 2026-07-31 above; the "geometry appears / occlusion" framing was a
flag-gated diagnostic artifact, not the real path — start with
[SESSION_SNAPSHOT_2026-07-30.md](SESSION_SNAPSHOT_2026-07-30.md)):**
Root cause found via an ultracode workflow + RenderDoc scissor/tile analysis. The
1120x720 HDR is rendered as an **upper 576-row tile → guest 0x19C67000** + a
**lower 144-row tile → 0x1A153000** (same linear buffer); the compositor samples
0x19C67000 (bulk of screen). RenderDoc proved the presented cycle's draws are
scissored to (0,0,1120,144) while earlier cycles use (0,0,1120,720). The color
tiles were seeding **stale** depth. My depth-fresh flag had gated on `height==720`
but the real tiles are 576/144 — a gate bug. **run 1341**: broadening the gate to
576/144 + dropping the stale seed (clear depth to reverse-Z far) produced the
**FIRST non-black gameplay** (`ghidra_out/title_ui_re/depthfix/depthfix_v1_t173.png`
— real geometry in the upper third). It's over-drawn (no occlusion) because the
tile passes have no inline depth writers, so clear-to-0 = GEQUAL pass-all. **NEXT
= correct occlusion.** UPDATE (6 build/run iters + 2 ultracode workflows): the
gameplay RENDER TARGET is 720 (the 576/144 tiling is scissor/resolve-rectangle
only), so color+depth already share ONE 720 depth resource. A diagnostic log
proved the depth prepass renders **225** depth writers into **depth_identity=1695**
and the color resolve reuses the **SAME identity** (232 draws) — yet reusing that
shared depth STILL over-draws (pass-all). So depth is far/empty at color-read time
despite being populated by the prepass ⇒ the remaining cause is **resolve ORDERING**
(color resolve runs before the depth resolve populates 1695 this frame) or
**far-biased prepass depth**. NEXT: RenderDoc capture WITH the flag ON, readback
depth identity 1695 at color-resolve time + check resolve submission order.
Adversarial verifiers correctly KILLED a wrong "separate 576/144 depth / decouple
height key" fix. **DEFINITIVE (after ~13 iters, 3 workflows): ALL resolve/binding/
depth-content causes ELIMINATED by measurement — per-draw log proves material draws
bind the SAME depth resource 1695, no clear, GEQUAL, test-enabled; a `debug depth
readback statistics` of the depth resolve shows CORRECT populated scene depth
(min=0.00235 max=0.25448, zero=0). Material draws test GEQUAL vs correct depth yet
pass everywhere ⇒ the material VS emits Z >= the depth-prepass VS's stored Z for the
same geometry: a prepass-VS-vs-material-VS clip-Z DISAGREEMENT (SHADER-TRANSLATION
bug in rexglue-src's VS Z path), NOT a backend/resolve/depth-state bug. This explains
both the original black (material Z << prepass) and the current overdraw (material Z
>= prepass). REAL FIX = diff the depth-only VS and material VS translated
SV_Position.z + vertex constants for one matched draw (renderdoc_shadercmp.py /
Xenos->DXBC translator) — find the viewport-Z/W-divide/half-pixel/constant difference
between the two paths. All backend-state avenues are exhausted.** Staged dll is a
flag-gated DIAGNOSTIC (overdraw, default OFF).

**PRIOR (2026-07-30, black-world RenderDoc pixel-history diagnosis —
[SESSION_SNAPSHOT_2026-07-30.md](SESSION_SNAPSHOT_2026-07-30.md)):**
The loaded world is still black. Decisive RenderDoc evidence
(`ghidra_out/title_ui_re/renderdoc_black_world/fable2_frame2054.rdc`, replay
scripts `renderdoc_pixhistory.py` / `renderdoc_blackpass.py`): reverse-Z GEQUAL
depth test rejects this frame's geometry because the 1120x720 gameplay depth
target holds **stale, pre-existing coherent depth** (~0.25, present before any
draw). The Rosetta pixel (820,520) proves color renders correctly where the
material depth agrees with the stored depth (`[2.43,1.18,0.56]`), and the same VS
hash passes at one pixel / fails at another (so NOT a shader bug). The recurring
"black" events are `ClearRenderTargetView`, so the frame is a clear→replay
sequence that ends on a black clear + failing draws. Implemented opt-in
`REXGPU_NATIVE_GAMEPLAY_DEPTH_FRESH=1` (drops the stale persistent-depth seed for
the gameplay depth prepass AND the color pass); both confirmed firing on the
right identity but the world stayed black — so stale-seeded depth is a real
sub-bug but NOT the sole cause. Next: verify the presented cycle's depth
generation and attack the clear-replay-every-resolve architecture. Staged
`rexgpu-native.dll` SHA-256
`9BE34501D2133BA50BC6B2D511CA62E8BCC4839A171DC8414DA1C0ECC3944D31` (flag
default-OFF → normal runs unchanged).

**LATEST RESUME (2026-07-29 late night, authoritative visual/performance
correction):** native loaded gameplay is currently black except for UI and
glows, the post-load world path is genuinely slow, and the Lionhead intro
flashes white between valid frames. These are open regressions. The earlier
entries below describing fully fixed native gameplay or acceptable cadence
are historical and do not describe the current Hero001 result.

Clean desktop capture proves the loading screen is correct before the world
turns black:

- `../ghidra_out/title_ui_re/clean_native_20260729_222238_desktop.mp4`;
- `../ghidra_out/title_ui_re/clean_native_45s.png`;
- `../ghidra_out/title_ui_re/clean_native_55s.png`;
- `Fable2_1333.log`.

The final compositor uses the native resolved `0x19C67000` HDR resource, so
this is not merely stale guest RAM. A narrowly scoped `LEQUAL` diagnostic
fills the HDR surface with pale overdraw, while normal reverse-depth `GEQUAL`
leaves only fragments. A positive depth bias of 8 under `GEQUAL` made no
visible change. Full depth/color fingerprints show all 243 paired draws are a
constant 269 guest sequences apart, ruling out a random cross-frame pairing.
The likely fault is the exact depth-only/material shader precision or the
persistent-depth load/store generation, not missing geometry.

Performance counters simultaneously show about 1.41 million
`SubmitDrawProof` calls, 1.43 million retained-draw submissions, 1.10 million
historical draws skipped, and 18,198 draw-slot waits in the late run. The
pending list is bounded near 872, but the renderer still replays huge
historical populations. Fix visuals first, then replace retained resolve
replay with explicit native pass ownership.

RenderDoc launch capture and unattended `qrenderdoc --python` replay now work.
The first `.rdc` was mistimed and captured the loader, not gameplay. The
harness now waits for the first native 1120x720 depth resolve before pressing
F12. Full evidence, exact switches, source state, failed A/Bs, and resume order
are in
[SESSION_SNAPSHOT_2026-07-29_LATE.md](SESSION_SNAPSHOT_2026-07-29_LATE.md).

**LATEST RESUME (2026-07-29, misleading host FPS / choppy frame delivery
diagnosed and renderer stalls reduced):** the top-right ~196 FPS value is not
the rate of fresh Fable frames. The D3D12 host presenter is intentionally
uncapped (`Present(0, ...ALLOW_TEARING)`), while the guest normally submits
about 30 fresh swaps per second.

Run 1182 exposed a real secondary slowdown under loaded-world resolve replay:
checkpoint 1024 to 2048 took 42.820 seconds (23.91 fresh swaps/s), while
`draw_slot_waits` rose from 29 to 175. The captured tiled HDR batches can replay
well over 1,000 draws, exhausting the 32-entry draw allocator ring.
`kDrawSubmissionSlotCount` is now 64. Keep normal rolling per-draw fence
signals: an attempted chunked-fence implementation raised burst waits to 571
and was removed. With the 64-entry rolling ring, runs 1184/1188 report zero
draw-slot waits through checkpoint 2048. The directly comparable run 1188
interval took 40.657 seconds for nearly the same target-switch workload
(6,685 versus 6,691), about 5% faster; run 1184's slightly lighter interval
took 38.320 seconds.

A separate resolve-ring counter was added. Testing both 32 and 64 resolve slots
reported zero waits and no measurable difference, so the final build retains
32. The first 512 postprocess probes are now sampled (first four plus powers of
two), reducing run 1188 from 512 per-frame INFO writes to 11 without removing
the diagnostic.

Once the world is settled, 511 consecutive producer intervals averaged
33.757 ms; only seven exceeded 40 ms. GPU utilization sampled about 2.5%, and
the native GPU worker used roughly 31% of one CPU core. Thus the remaining
steady visual cadence is Fable II's ~30 Hz production, not a hidden 196 FPS
stream. The official revision-2 one-byte "60 FPS" recomp equivalent and a
120 Hz native-vblank experiment both left production near 33.5 ms and were
fully reverted; do not restore them without finding the missing game-side
timing dependency.

Final release validation passes 530 assertions in 29 PM4 cases plus
`native_plugin_smoke`. Staged/source `rexgpu-native.dll` SHA-256 is
`5E564F76A93662E56BBDD674712560DDA2E99A38DA4C923C65D046F46995113C`.
Run 1188 confirmed the real Hero000 load, restored the 26-byte
`mystartup.lua`, and left no Fable2 process.

**LATEST RESUME (2026-07-29, final-compositor peach wash fixed):** the
remaining full-screen peach/overexposed result was caused by the native
backend's diagnostic render-target clear color, not Fable's HDR or exposure
data.

Run 1176 captured the final compositor's upper input at guest `0x19C67000`.
The corrected half-float readback showed a fully populated 1120x576 scene
surface with real blue/green image data. Runs 1177-1179 then dumped and
disassembled final pixel shader `FBE91459C01C61BF` (translator key
`F1ABBFF68DE8DBFD`) and logged its exact loaded-world inputs:

- slot 0: `0x19C67000`, 1120x720, `16_16_16_16_EXPAND`/half-float;
- slot 1: `0x1FC20000`, 512x1 R32_FLOAT exposure, 512/512 finite;
- slot 2: `0x0C786000`, 280x180 packed 10:10:10:2 bloom.

The texture formats, swizzles, exponent adjustment, constants, and translated
shader math were consistent. Capturing slot 2 in run 1180 exposed the actual
contamination: every uncovered bloom pixel inherited
`kDrawProofClearColor = {0.015, 0.035, 0.08, 1}`. The bloom shader multiplies
that colored sentinel by 2.9764 and the final compositor gamma-corrects it,
which accounts directly for the uniform peach result.

`kDrawProofClearColor` is now neutral opaque black `{0, 0, 0, 1}`. Transparent
black was also tested in run 1181: it removed the peach wash but changed
destination-alpha blending and left only emissive geometry, so alpha 1 is
required. Run 1182 with opaque black produced the full naturally colored,
textured caravan frame with normal contrast and no peach wash. The proof is
`../native_opaque_black_validation.png`. The earlier horizontal compositor
upscale fix remains active, so the right edge is covered.

Release validation passes 530 assertions in 29 PM4 cases plus
`native_plugin_smoke`. The complete `unit_tests.exe` suite still has unrelated
pre-existing environment/template failures (33 failures); it is not the
renderer validation target. Staged/source `rexgpu-native.dll` SHA-256 is
`1876919CB11C118AED19EE1F284355E4F08B5675980A9C77B6474C8F83E16A20`.
The run 1182 harness confirmed the real Hero000 load, restored the 26-byte
`mystartup.lua`, and left no Fable2 process. Shader dumps are in
`../compositor_shader_dump_1177`; targeted diagnostic state remains one-shot,
and shader dumping is only enabled by
`REXGPU_NATIVE_COMPOSITOR_DUMP_PATH`.

**LATEST RESUME (2026-07-29, tiled HDR retention and final-compositor width
fixed; exposure diagnosis corrected):** two independent loaded-world defects
are fixed in the native D3D12 path.

First, `SubmitDrawProof` now detects the final compositor
(`F1ABBFF68DE8DBFD`) consuming a resolved slot-0 color input whose height
matches the output but whose width is smaller, and uses the input width for
the horizontal guest-viewport-to-NDC transform. This expands Fable's observed
1120x720 HDR compositor triangle across the 1280x720 output without hardcoding
either resolution. The rule is limited to this compositor and resolved input.

`Fable2_1166.log` is the live Hero000 validation. At sequence 346164 it logs
`final compositor horizontal upscale enabled (input=1120x720,
output=1280x720)`. The 2560x1440 client capture
`../native_upscale_validation.png` has continuous compositor output through
the right edge; the former 160-pixel-equivalent black strip is gone. The world
is still peach/overexposed, so compositor coverage is fixed but visual parity
is not.

Second, partial color resolves that clear their source now retire their
matching producers immediately. Fable resolves its 1120x720 HDR target as a
576-line upper tile followed by a 144-line lower tile, clearing after each
resolve and repeating the scene draws with a new window offset. Previously the
upper 1,536 draws stayed in the per-target retention cap, so the lower draw
packets were rejected and the second resolve replayed stale top-scissored work.
The corrected RGBA16_FLOAT debug readback proved the result:

- Before producer retirement, `Fable2_1170.log` sequence 362947 reported all
  161,280 lower-tile pixels exactly equal to clear.
- After producer retirement, `Fable2_1173.log` reports all 161,280 pixels
  changed; `rexgpu_native_resolve_1A153000_08.bmp` visibly contains world
  geometry across the 1120x144 strip. Later frames retain roughly 1,200 fresh
  lower-tile draws rather than the stale upper batch.

The debug resolve dumper itself was fixed to decode `DXGI_FORMAT_R16G16B16A16_FLOAT`
at 8 bytes per pixel, hash both dwords, and convert half floats for BMP output.
The earlier magenta/blue striped dump was a 4-byte packed-10:10:10
misinterpretation, not GPU content.

The earlier exposure conclusion was timing-dependent and is now disproven for
the loaded world. The title draw at sequence 13190 still observes 512 NaNs, but
the loaded-world compositor at sequence 346164 observes 512/512 finite values
(0 through 0.61108625). Run 1165 independently saw 512/512 finite values (0
through 0.14434986). No resolve or captured memexport overlaps guest
`0x1FC20000`; endian conversion is correct. Do not pursue "512 NaNs cause the
peach world" without new loaded-world evidence.

The broad temporary memexport producer probe was removed. One-shot exposure
and upscale diagnostics remain. Release validation passes 530 assertions in
29 PM4 cases plus `native_plugin_smoke`; staged/source `rexgpu-native.dll`
SHA-256 is
`71CD9A03DDD317316A796EF9E7DDEB5365A8F341BA9EBB72DE015EE5F9E73162`.
The harness confirmed the real Hero000 load, skipped `NewBeginnings.bik`,
restored the 26-byte `mystartup.lua`, and left no Fable2 process.

`../native_final_visual_validation.png` is the final normal run 1175 capture.
It proves the quest/HUD and lower frame survive and the right edge is covered,
but the world remains peach/overexposed. Next: capture/decode the upper
1120x576 HDR tile and inspect translated compositor shader math. Do not return
to the exposure producer without new evidence. The correct lower-tile batch
also increases the already-known resolve replay cost, so optimize replay after
visual diagnosis. Black hero/dog atlas materialization remains separate.

**PREVIOUS RESUME (2026-07-28 late night):** start with
[SESSION_SNAPSHOT_2026-07-28.md](SESSION_SNAPSHOT_2026-07-28.md). The loaded
world's native final compositor consumes a 512x1 R32_FLOAT exposure resource
containing 512 NaNs, while its 1120x720 HDR input is not correctly scaled to
the 1280x720 frontbuffer. The post-skip slowdown begins at the first
1536-draw world/depth batch and is dominated by repeated resolve replay/target
switching, not audio or texture-cache eviction. Run1159 is the xenos visual
oracle, not a native success. Black hero skin and dog artifacts remain, and
Hero000 did not emit the currently recognized `0x12704000` selective
materialization event.

**LATEST RESUME (2026-07-28, selective hero/dog materialization, stable
frontbuffer ownership, and realtime-probe cleanup):**

The black-skin investigation has a decisive emulator-side ground truth. The
Fable-specific Xenia femtofork fixes the bug by readback-resolving exactly one
guest destination, `0x12704000`, only when the player/dog atlas regeneration
draw occurs. Its upstream commit is
`746213fb8d13b79ff985c69cb45c81d7ce5dbc5c`; the general readback-resolve path
is intentionally not enabled because it is too expensive.

The native backend now implements that same narrow dependency without
restoring EDRAM:

- `Pm4Backend` has a raw guest-memory writer owned by
  `NativeGraphicsSystem`.
- A color resolve to `0x12704000` in `k_8_8_8_8` is the only ordinary resolve
  that waits for the GPU and crosses back to guest memory.
- The full preserved host atlas is converted from linear RGBA into the Xenos
  32x32 tiled layout, applies destination R/B swap and endian mode, and is
  written to guest physical memory. Native texture generations are invalidated
  after the write.
- Candidate, rejected-candidate, and successful materialization events are
  explicitly logged. The stable Hero000 automation does not regenerate this
  adult/player atlas, so the exact live event still needs an adult Hero001
  gameplay run; implementation and packet predicate match the upstream fix.

The late-world display corruption had two measured ownership failures. The
64-base resolved-target cache evicted `unordered_map::begin()`, eventually
discarding the active `1ECFF000` 1280x720 frontbuffer. It now evicts the oldest
non-presentation base while protecting the live frontbuffer, debug target, and
player/dog atlas. Also, newer pending work may displace a cached resolve only
when every draw forms one homogeneous full-width target. Planar video retains
its explicit ownership exception. Internal 1x/4x render passes are no longer
replayed as one presentation frame.

Realtime investigation probes were removed after establishing their results:
the SDL submit path no longer scans every audio sample twice; XMA output and
context buffers are no longer rescanned/logged; audio kernel synchronization
and `MmGetPhysicalAddress` probes are gone. The audio pseudo-physical address
fix and inline XMA decode remain. Mixed-MSAA diagnostics now report four
aggregate counts instead of formatting hundreds of entries.

Verification:

- Focused suite: 530 assertions in 29 cases; native plugin smoke passes.
- `Fable2_1085.log`: first staged pass reached the world with zero
  D3D12/DXGI/device errors and zero retired audio/XMA/kernel probe lines. It
  exposed the arbitrary frontbuffer eviction at cache capacity.
- `Fable2_1086.log`: cache-protection probe reached checkpoint 2048 with
  `matched=true`, then reproduced the heterogeneous 729-draw fallback
  (`559x 1x`, `170x 4x`) that motivated the ownership guard.
- `Fable2_1087.log`: final guard reached the same late-world miss window and
  cache capacity. It logged `frontbuffer=1ECFF000, protected=true`, evicted
  oldest non-presentation base `1A60C000`, and emitted **zero** mixed-presenter
  batches, D3D12/DXGI errors, or device loss.
- Staged `rexgpu-native.dll`: 1,582,592 bytes, SHA-256
  `74208AD6A2B696A3A49F5463BE32CC8A73F81F243D3348896DBD92310DCDCE68`.
- Staged `rexruntime.dll`: 10,466,304 bytes, SHA-256
  `8F6F564CBF6323BFA50CD7C0812E4449EC8CFBF3AC5DE5F8ECE81669E0E30CD1`.

Next visual validation should use adult Hero001 and look for
`observed player/dog resolve candidate` followed by
`selectively materialized player/dog resolve`. If the candidate is rejected,
the same log now reports pending/matching/complete counts. Do not broaden this
to general resolve readback.

**LATEST RESUME (2026-07-28, EDRAM backing and compatibility gate retired):**
the native renderer now owns independent persistent color and depth attachments
keyed by the guest target identities. The guest EDRAM base is frontend
identity/alias metadata only: there is no 10 MiB backing allocation, no
guest-memory render-target round trip, and no duplicate host snapshot texture
between a draw attachment and its persistent resolve source.

Color and depth activation are independent. A color-only resolve does not
switch or recreate depth, a depth-only pass does not disturb color, and
compatible cached attachments are rebound without clearing their contents.
Resolve registry entries reference the actual draw color/depth resources, so
the former seed/store `CopyResource` pairs are gone. Resolved destinations
remain generation-safe and are not recycled while a retained draw pins an
older generation.

`REXGPU_NATIVE_RESOLVE_HEAP` has been removed from the native backend. The host
render graph is unconditional; setting the obsolete name to `0` has no effect.

Verification:

- `Fable2_1082.log`: D3D12 debug+DRED, Hero000 real load ->
  `NewBeginnings.bik` -> skip -> late-world rendering on the direct-owned,
  split-attachment path. It crossed native resolve 16,384 with 16,263
  destination reuses and 11,321 persistent reuses. At checkpoint 2048 it had
  17 color / 15 depth attachments and 14,352 target switches, down from 23,557
  before selective attachment activation. There were zero D3D12/DXGI
  barrier/state errors, zero device loss, zero draw-retention warnings, and
  zero cache evictions.
- `Fable2_1083.log`: launched under the D3D12 debug layer with the removed
  `REXGPU_NATIVE_RESOLVE_HEAP=0` name deliberately present. The backend still
  reported `native host render graph enabled`, reached resolve 256 with 255
  destination reuses, and emitted no D3D12/DXGI/barrier/device error.
- The focused suite still passes 530 assertions in 29 cases; native plugin
  smoke passes.
- Staged `rexgpu-native.dll`: 1,577,984 bytes, SHA-256
  `AE6AB75C8540C42C9A94935C201DCF8A08F0714477FFF1C21CA715F1BBD32209`.

The EDRAM-bypass slice is complete. Remaining render-graph work is broader
pass dependency/lifetime/eviction policy and exact visual-parity debugging,
not restoration of an emulated EDRAM heap.

**LATEST RESUME (2026-07-28, late-world visual completeness and streaming
hitch pass):** the automated load path now reaches the dense world transition
without dropping the end of the retained frame, thrashing the texture cache,
or emitting a synchronous fixed-allocation log storm.

The first long post-skip run exposed two late issues that shorter checkpoint
runs missed:

- `Fable2_1068.log` reached the old 768 retained-draw cap during the world
  transition, so later geometry/effect draws were intentionally left
  validation-only. Raising the aggregate cap to 1024 still overflowed with 691
  draws on one target (`Fable2_1070.log`). A 1536 aggregate / 1024 per-target
  probe then retained 1357 draws before that target itself reached 1024
  (`Fable2_1071.log`).
- The 2 GiB texture budget began LRU eviction during world streaming. The old
  eviction loop rescanned the complete cache for each victim. Startup also
  synchronously formatted and wrote 3,856 expected failed fixed-placement
  probes.

The native renderer now keeps a bounded 2048 draws per multi-target frame and
1536 per color target. This is measured headroom around the observed 1357+
draw population, not an unbounded retention rule. Same-configuration switches
between different guest depth/color EDRAM identities now clear the shared host
target, preventing stale tiles from bleeding between passes; presentation
checkpoints report `identity_clears`.

The host texture budget is now one quarter of dedicated VRAM, clamped to
256 MiB–4 GiB (4,063 MiB on the current adapter). Eventual LRU eviction gathers
and sorts all fence-safe candidates once rather than rescanning the full map
per victim. Fixed-address conflict diagnostics are logarithmically sampled:
the same 3,856-probe startup search now writes 12 lines and completes in about
2 ms instead of the previous roughly 30 ms logging burst.

Verification:

- `Fable2_1072.log`: D3D12 debug layer plus DRED, Hero000 real load ->
  `NewBeginnings.bik` -> skip -> late world transition. It crosses the first
  same-configuration depth identity change with zero retention-bound warning,
  zero cache eviction, zero D3D12/DXGI warning or error, and no device loss.
- `Fable2_1068.log` plus `postskip_desktop.mkv` provide a 35-second visible
  post-skip capture; sampled storybook frames remain coherent while the cache
  stays eviction-free.
- The focused suite passes 530 assertions in 29 cases.
- Staged `rexgpu-native.dll`: 1,572,864 bytes, SHA-256
  `44B85B05E4781926546480920CD6D7185CAF9F6143B2F66B8A9962431E2EABF5`.
- Staged `rexruntime.dll`: 10,476,544 bytes, SHA-256
  `FE2B306563266F28CE48F8CE4AD242342F26C7E7DCD8477462C27E9AD2D87212`.

The user's exact remaining gameplay artifact still needs a screenshot or
foreground capture to map its shape to a pass. Do not reduce the new bounds
back to 768/1024: the logs prove those settings truncate live world frames.
Do not admit the synthetic `0x10000000` 1x1 transition packets; the new fix
retains the missing real tail without weakening that black-square guard.

**LATEST RESUME (2026-07-28, native draw-submission and texture-streaming
performance pass):** the gameplay/menu-specific bottlenecks have been measured
and reduced without changing the already-smooth video path.

Presentation checkpoints now report texture-cache occupancy, hits/misses and
evictions, draw-slot waits, submission batches, and target switches. The first
instrumented run (`Fable2_1057.log`) showed that the original three-slot draw
ring, not presentation or video, was the dominant steady-state limiter:
checkpoint 1024 had 4,388 slot waits and 40,680 submission batches.

The native backend now:

- uses 32 independently fenced draw slots;
- grows and reuses vertex/index upload buffers by power-of-two capacity;
- stages all new textures for a draw through one grow-only upload arena owned
  by that fenced slot, replacing one committed upload resource and map/unmap
  pair per texture;
- sizes the host texture cache to one eighth of dedicated VRAM, clamped to
  256 MiB–2 GiB (2,031 MiB on the current 16 GiB adapter);
- retains streamed textures until that budget is needed; and
- submits large heterogeneous frames in fenced chunks of up to 32 command
  lists. A 768-draw frame now needs 24 queue signals rather than 768, while
  keeping descriptor, upload, and render-target ownership fence-safe.

Verification:

- `Fable2_1066.log`: Hero000 real load -> `NewBeginnings.bik` -> skip completed
  normally. Versus the matched 16-slot `Fable2_1064.log`, checkpoint 1024 slot
  waits fell from 2,635 to 1,237 (53%) and submission batches from 3,649 to
  2,252 (38%); checkpoint 2048 waits fell from 3,565 to 1,681 (53%) and batches
  from 5,603 to 3,721 (34%). Checkpoints 512->1024 still took 17.09 seconds,
  confirming this menu segment is now capped near its 30 Hz presentation rate
  rather than by CPU fence pressure. The cache held 3,132 entries / 1,497 MiB
  with zero evictions at checkpoint 2048.
- `Fable2_1067.log`: the same real load and cinematic skip under the D3D12
  debug layer plus DRED reached checkpoint 2048 with no D3D12 validation
  error/warning, DXGI error, device loss, or deleted-in-flight object.
- The focused suite passes 530 assertions in 29 cases, including exact chunk
  counts for 0, 1, 16, 17, and 768 draws with both 16- and 32-slot rings and
  the unstable-target fallback.
- The staged `rexgpu-native.dll` is 1,567,232 bytes with SHA-256
  `C0FE1F0DB3EC84875779FF1173D83DEAABBF3E777F3259F75FB5D37EA549AA2C`.

The remaining performance question is shader/texture creation during genuinely
new world streaming; the steady-state slot and per-texture upload-allocation
cliffs are no longer present. Do not judge the new chunking from the raw
`draw_slot_waits` counter alone: it counts each safe slot rollover, including
short fence catches. Use elapsed checkpoint time and `batches` together.

**LATEST RESUME (2026-07-28, native menu/gameplay synchronization
performance pass):** the save-load lifetime fix remains intact, and the
high-frequency host-GPU synchronization it introduced has been removed.

The slow path was configuration churn. Menus/loading/gameplay alternate
10:10:10:2, RGBA16F, RGBA8, and 1x/2x/4x targets, while video generally remains
on one target. Every change previously waited for the entire GPU, destroyed the
shared color/depth/resolve textures, rebuilt them, and prevented heterogeneous
draws from sharing a fence.

The native backend now caches a complete target set per
width/height/format/MSAA configuration. Each set owns its RTV/DSV heaps as well
as its color, depth, and optional resolve textures, so descriptor handles and
resources referenced by queued command lists remain valid. Configuration
switches reuse these sets without a GPU-wide wait; cached targets are cleared
on activation to preserve the prior discard-on-switch behavior. This stable
ownership also safely restores one-fence batching for heterogeneous frame
draws.

EDRAM capture/presentation is now an ordered direct-queue chain rather than
three guest-CPU blocking waits per resolve. Multisample-depth compute resolves
use a descriptor heap owned by each of the 32 resolve command slots, and
resolved-target presentation uses those fenced slots rather than the singleton
utility list. Only explicit diagnostic readback still waits synchronously.

Verification:

- `Fable2_1054.log`: Hero000 load -> `NewBeginnings.bik` -> skip, followed by a
  35-second post-skip run with the D3D12 debug layer enabled. It has no D3D12
  validation error, device loss, resource-lifetime warning, or DRED failure.
- `Fable2_1055.log`: normal-speed comparison. Presentation checkpoints
  512->1024 take 22.080 seconds versus approximately 29 seconds in the
  pre-optimization `Fable2_1050.log` (24% less elapsed time / about 31% more
  throughput). Checkpoints 1024->2048 take 36.141 seconds versus approximately
  43 seconds (16% less elapsed time), sustaining 28.3 presentations/second
  near the original game's 30 Hz target.
- The focused suite passes 521 assertions in 29 cases.
- The staged `rexgpu-native.dll` is 1,564,160 bytes with SHA-256
  `4E0044BF5CA1F398080759C1505A4A9D7F925068BD209B4A414A6D04A97B58E1`.

The next visual milestone remains the first complete gameplay/world frame
after the storybook loader. Do not restore the removed resolve waits: same-queue
execution and explicit barriers provide ordering, while per-slot/per-target
ownership provides CPU-side lifetime safety.

**PREVIOUS RESUME (2026-07-28, native save-load GPU loss fixed and post-skip
loading presentation verified):** the native D3D12 process no longer exits
during the Hero000 load. The failure was a host resource-lifetime race, not a
bad save or guest-side load failure.

The decisive debug-layer/DRED run reported
`OBJECT_DELETED_WHILE_STILL_IN_USE`: command list
`draw sequence=319660 submission=56476` referenced a resource that had already
been final-released, then the device was removed with
`DXGI_ERROR_DEVICE_HUNG`. Heterogeneous presenter batches grouped command lists
under one later fence signal even when adjacent draws changed MSAA/color target
configuration. `EnsureDrawProofRenderTarget` therefore waited only through the
previous signaled submission and replaced the shared color/depth targets while
earlier lists in the current unsignaled group were still queued.

That first corrective build permitted one-fence grouping only when every draw
used the same MSAA and color-target format. The newer cached-target design
above supersedes that temporary restriction without reintroducing the
lifetime race. The native presenter also notifies the backend before its fatal
GPU-loss callback, enabling live debug-queue and DRED diagnostics; command
lists carry guest sequence/submission names.

Verification:

- `Fable2_1050.log`: Hero000 real load confirmed, `NewBeginnings.bik` opened
  and was skipped, and native remained alive for the complete 45-second
  observation window.
- `Fable2_1051.log`: the same path with the D3D12 debug layer and forced DRED
  completed a 15-second post-skip window with no validation error, device loss,
  or deleted-in-flight resource report.
- `native_post_skip_fixed.png`: direct desktop capture five seconds after the
  skip shows the complete animated storybook loading scene, character art,
  particles, and subtitle. Presentation is no longer dark or frozen on the
  cinematic.
- The focused suite passes 520 assertions in 29 cases.
- Source and staged `rexgpu-native.dll` are both 1,556,480 bytes with SHA-256
  `C960C562D3DFEEA94582A57875B302A16941ACBC7F68AC2B4F52575DDC394DA1`.

The next visual milestone is the first rendered gameplay/world frame after the
storybook loader. Do not reopen the save-load crash or rectangle-list/alpha
hypotheses without new evidence: the debug layer identified the lifetime fault,
and per-draw readbacks plus the direct window capture prove the loading
composition itself is intact.

**LATEST RESUME (2026-07-27 night, run 1007 stopped; run 1008 trace build staged):**
the remaining failure is now narrowly classified as a full-resolution render-graph dependency
break, not a stalled game or a video decoder failure.

User-visible state at the end of the session:

- Loading a save does not visually replace the save-card screen with the expected loading screen.
- The following `NewBeginnings.bik` cinematic does render. If it is skipped, game audio continues
  but presentation remains frozen on the last cinematic frame. In an earlier path without that
  retained video owner, the loaded level was black.
- Intermittent black-square artifacts are still reported.

What is proven/fixed:

- Real planar YUV draws use render-target transition mask `0x20`. A narrow three-plane 4:2:0
  exception retains them, so intro/cinematic video is visually active again.
- Drawless resolves can replay persistent host EDRAM; the 320x180 postprocess chain captures and
  advances.
- Stale resolved-frontbuffer ownership can yield to newer planar/pending work.
- Partial resolved targets preserve prior compatible host content, including the observed guest
  number/endian aliases. Resolve/fetch aliases 2/10, 3/12, and RGBA16F
  `k_16_16_16_16_FLOAT` -> `k_16_16_16_16_EXPAND` are accepted.
- Transition draws whose inputs are already ready resolved targets are admitted only for masks
  `0x40/0x41/0x60/0x61`, concrete geometry, base color target zero, 1x MSAA, and nontrivial 2D
  inputs. A second guarded tier admits concrete texture-backed scene draws after boot while
  rejecting textureless packets and all-1x1 synthetic marker draws.

Run evidence:

- `Fable2_1005.log` proved the original full-size break: depth destination `0x1A2B1000`
  (1120x720) and color destination `0x0C6E4000` (560x360) repeatedly had no captured producer,
  while the 320x180 `0x0C6E4000` chain captured successfully.
- `Fable2_1006.log` proved the resolved-input rule is safe and active, but it starts downstream
  of the missing producer.
- `Fable2_1007.log` proved nontrivial texture-backed transition draws are retained. After sequence
  80000, the remaining sampled rejections are textureless state/clear packets or the synthetic
  `0x10000000` 1x1 fetch. The full-size depth/color resolves still miss, so do not widen the
  transition guard again without using the new dependency trace.
- All focused tests pass: 519 assertions in 29 cases. The game is stopped. The latest source and
  staged `rexgpu-native.dll` SHA-256 is
  `1FD19A2FBA5BD45694DED6D1A38D729211D4E8708FAFC915E4FE6720A781D601`.

Resume tomorrow:

1. Launch the staged build as run 1008 with `REXGPU_NATIVE_RESOLVE_HEAP=1` and
   `REXGPU_NATIVE_D3D12_DEBUG=1`; reproduce load -> cinematic -> skip.
2. Inspect the first 128 `postprocess resolve probe` records. The probe now logs every resolve
   after sequence 70000 rather than two hard-coded addresses, including pending/matching/complete
   counts.
3. Correlate those with the first 64 `texture-backed transition probe` records, which include
   color/depth EDRAM bases, formats, sample count, geometry, and source textures.
4. Find the exact earlier resolve that consumes or separates the two pending full-size producers.
   Fix that ownership/identity edge; do not admit the textureless transition packets or the
   `0x10000000` 1x1 marker draws.
5. Verify that a new full-size resolve displaces the last video frame immediately after skip,
   then remove the two temporary probe blocks and rerun the 519-assertion suite.

Build quirk remains deterministic: VS 2022 compiles but its link fails only on
`__std_find_*`; rerun the link with the VS 18 developer environment.

Unicorn Engine assessment: do not integrate it for this work. Unicorn is a QEMU-derived CPU
emulation framework, not a GPU/EDRAM model. Its current public header explicitly marks PPC64 mode
unsupported, and it has no Xenon VMX128 or Xenos PM4 implementation. It may be useful someday as
a standalone PPC32 instruction oracle, but it cannot diagnose this D3D12 resolve graph and an
embedded integration would also require a deliberate GPL/LGPL review. Sources:
<https://github.com/unicorn-engine/unicorn> and
<https://raw.githubusercontent.com/unicorn-engine/unicorn/master/include/unicorn/unicorn.h>.

**PREVIOUS RESUME (2026-07-27, stable presentation plus format-correct complete gameplay pass):**
the owned D3D12 path now fixes three independently proven sources of intermittent/incomplete
frames.

- The frontbuffer underfill guard no longer promotes any arbitrary ninth short resolve. It
  requires a stable lower-complexity draw count, so varying transient collapses retain the last
  complete image while legitimate scene changes still publish.
- Native color targets and PSOs now follow the live Xenos draw format. Frontend 2/10 remains
  `R10G10B10A2_UNORM`; gameplay 3/12 uses `R16G16B16A16_FLOAT`; format 0 uses
  `R8G8B8A8_UNORM`. Multisample region resolves use the active resource format.
- Resolve ownership now matches EDRAM base, sample count, and compatible host storage. The
  legitimate 2/10 and 3/12 aliases may share a batch, while incompatible passes cannot
  contaminate each other. New draw color/depth attachments use deterministic initialized
  allocations rather than `CREATE_NOT_ZEROED`.

The gameplay transition also exposed stale pending work and the old compatibility bound.
Incompatible ownership transitions now retire superseded draws (206 in the final run). The
bounded batch is 768 draws; `Fable2_995.log` proves the live 2x-MSAA HDR Bower Lake pass resolves
all 599 retained format-3/12 draws with zero bound warnings. That run also covers format-0 4x
MSAA (85 draws), native format-12 HDR, and the title/frontend path.

`Fable2_995.log` has zero D3D12 validation errors, GPU errors, device removal, or draw-retention
overflow. PM4 tests pass with 519 assertions in 29 cases. Source and staged
`rexgpu-native.dll` SHA-256 is
`520B61810C8EA163927166DAEE3C6EB10B45E9F6ADF77A1FB7DD6E5BBFAF673C`.
The debug-layer validation process is intentionally left running as PID 17524 for visual
inspection.

The immediately preceding MSAA slice is also complete: draw targets/PSOs use guest 1x/2x/4x
sample counts, color resolves use exact average modes, translated shader system constants carry
the sample count, and multisample depth resolves use an exact compute shader selecting the guest
sample. `Fable2_983.log` reached more than 2,048 native resolves without a GPU error.

**PREVIOUS RESUME (2026-07-27, native raster/sampling state parity and real mip chains):**
the owned D3D12 path now carries the guest states that were most likely to produce one-frame
visual instability rather than treating every draw as the same generic host pass.

Completed and live:

- Alpha testing captures `RB_COLORCONTROL` / `RB_ALPHAREF` and supplies the translated pixel
  shader's alpha-test system constants.
- Screen/window scissor state is captured and intersected into the submitted D3D12 scissor.
- Front/back Xenos polygon offset is converted with the mature integer/subpixel scaling rules and
  included in PSO identity. `Fable2_970.log` proves the real textureless biased companion at
  sequence 2577775 is retained with `DepthBias=1680`; the exception remains narrowly limited to
  the proven EDRAM transition `0x40`.
- Guest blend constants are captured, hashed, coalesced, and submitted through
  `OMSetBlendFactor`. The long `Fable2_971.log` run found no retained draw using a constant blend
  factor, so this is completeness rather than the current glitch source.
- Packed/tiled mip chains are no longer discarded. Base and mip memory are snapshotted as separate
  coherent ranges, both participate in the host content key, packed-tail sublevels use the mature
  Xenos X/Y offsets, every guest level gets a D3D12 subresource, SRVs expose the uploaded levels,
  and samplers can select them. `Fable2_973.log` first proves a tiled packed 128x128 DXT5 chain
  with levels 0..3 (`16384` base bytes + `49152` mip bytes). Later inventory observes active
  chains through level 8 without a layout overrun, incoherent draw, or device error.
- Guest base-map-only filtering and anisotropy are honored. `Fable2_975.log` proves both are live:
  several physical mip chains request `mip_filter=2` (base-map), and a world BC texture requests
  `aniso=2` with levels 0..7. Fetch/instruction LOD bias remains correctly applied by translated
  shader code.
- `PA_CL_CLIP_CNTL` is now normalized and its depth-clip-disable bit participates in D3D12 PSO
  identity. Build 976 observes the first disabled state on an early transition packet and keeps
  the safe guest semantics for later retained draws.

Stencil was also classified rather than blindly enabled. The first live stencil state is
`RB_DEPTHCONTROL=0x00008777` at sequence 25, but it is textureless, unbiased transition-era
geometry and is rejected before native retention. Do not convert the native D32 graph to a
depth/stencil graph until a stencil-enabled draw is proven to survive the actual submission
guards.

Release tests pass with 519 assertions in 29 cases. Current source/staged
`rexgpu-native.dll` SHA-256 is
`6CBA50B97333B030DC68A27A65E0FDAE7460DCC9C7548E1879BB55A7C43E92A8`.
The active validation log is `Fable2_976.log`; PID 16876 is intentionally left running with
`REXGPU_NATIVE_RESOLVE_HEAP=1`.

**LATEST RESUME (2026-07-27, first native gameplay boundary classified):**
the save-selection renderer remains intact, and the native draw snapshot/upload path now supports
all referenced vertex-fetch streams rather than only the first. Streams are concatenated into a
4 MiB-bounded raw host buffer and every referenced fetch constant is rewritten to its compact
byte offset while preserving size and endian mode. Live `Fable2_949.log` proves the former first
gameplay rejection is now retained: sequence 310661 uses two fetches, 192,260 compact bytes, and
20 vertices.

The same slice adds tiled/linear `TextureFormat::k_16` upload as R16, so the observed 31x29 and
769x705 gameplay layouts no longer enter the unsupported inventory. Normal-mode
`k2DCopyRectListV2` is accepted through the rectangle-expansion path. Draw retention is now
bounded both globally (384) and per color target (256), leaving downstream composition capacity
without allowing one world target to grow without bound.

The remaining black gameplay world is now classified rather than guessed. Normalized draw events
capture `VGT_OUTPUT_PATH_CNTL` (register 0x2284), hash it, and log it on rejection. In a second
Bower Lake run, the one-vertex primitive-0x12 boundary reported `output_path=true:1`;
`VGTOutputPath::kTessellationEnable` is 1, so this is a quad tessellation patch rather than a 2D
copy rectangle. Tessellated draws are now kept validation-only before normal PSO creation, avoiding
the incorrect rectangle-list submission that made the first gameplay frame extremely slow.
Native hull/domain shader integration is the next correct world-rendering subsystem. Unsupported
cube textures and DXT3A/other gameplay layouts remain after it.

Evidence:
`ghidra_out/title_ui_re/native_956_multivfetch_gameplay.png`,
`ghidra_out/title_ui_re/native_959_tess_gate_gameplay.png`, `Fable2_949.log`, `Fable2_950.log`,
and `Fable2_951.log`. The final gate run confirms that the first 144-factor primitive-0x12 draw is
classified as tessellated before PSO creation. It reaches presentation checkpoint 4096 and resolve
event 65536 without the prior multi-minute incorrect rectangle submission; the world is still
black because tessellated geometry remains intentionally validation-only.
PM4 tests pass with 488 assertions in 29 cases; native plugin smoke passes. Source and staged
`rexgpu-native.dll` are 1,339,392 bytes with SHA-256
`518279672D5E2476276C12FEEC383955F06F5425244491E0C64E73BC43094C46`.
The final validation process is intentionally left open as PID 3852 with
`REXGPU_NATIVE_RESOLVE_HEAP=1`.

**LATEST RESUME (2026-07-27, complete native save-selection card fan):
the native D3D12 frontend now renders all six save cards and their ornate layers without the
large black panel. The mature Xenos oracle is
`ghidra_out/title_ui_re/xenos_945_savefan_oracle.png`; the final native still and continuous
evidence are `ghidra_out/title_ui_re/native_954_savefan_final.png` and
`ghidra_out/title_ui_re/native_954_savefan_60fps_30s.mp4`.

The saved Xenos PM4 capture proved that a settled save-selection frontbuffer contains 405 draw
events and 168 broadly eligible textured draws, so the old guarded 128-draw prefix truncated the
card fan. The native bound is now 256, large enough for the complete observed pass. Resolve
packets also decode as draw events; the final resolve inherited valid texture state and became a
full-screen black draw when retained. Draw events with a nonzero
`render_target_transition_mask` are now excluded from frontend proof submission.

After restoring the complete pass, candidate 128 exposed a separate black rectangle. It had no
visibility predicate and used the same full 1280x720 scissor as adjacent atlas draws. The actual
difference was guest rasterization state: `PA_SU_SC_MODE_CNTL=0x00218006` enables back-face
culling, while the native PSO had hard-coded `CullMode=NONE`. Normalized draw events now capture
that register; the native pipeline key includes its cull/front-face bits; and D3D12 PSOs apply
the matching front/back cull mode and winding. This removes the hidden back-facing atlas quad
while preserving the complete six-card fan.

The final continuous capture contains 1,260 decoded frames over 30 seconds. FFmpeg black-frame
analysis found no black interval. Per-frame signal statistics show only gradual scene animation:
the largest adjacent whole-frame Y-average change was 0.049 (on an 8-bit 0-255 scale), rather
than a one-frame flash. `Fable2_947.log` contains no native draw-cap overflow, D3D12 rejection,
device-removal, submission, or texture-coherency error. PM4 tests pass with 485 assertions in
29 cases, and native plugin smoke passes.

Source and staged `rexgpu-native.dll` are both 1,332,224 bytes with SHA-256
`BC03DA1AD532B6A92D2A1CB1596969455046C1BCB4041E6CB3611864712912A4`.
The verified native run is left open as PID 5332 with `REXGPU_NATIVE_RESOLVE_HEAP=1`. The
256-draw list is still a bounded compatibility path; replacing it with an explicit
render-target/resolve-aware host graph remains open.

**LATEST RESUME (2026-07-27, residual transition/title flashes actually removed):
the user correctly reported that the earlier sparse sampling missed real flashes. A continuous
60 Hz window recording caught repeated full-black bursts lasting roughly 50-180 ms during the
white-to-title transition and the settled title. The failing evidence is
`ghidra_out/title_ui_re/native_930_title_flash_probe_45s.mkv`; extracted frame
`probe_t_11_78.png` is a completely black frame between two complete title frames.

An opt-in per-frontbuffer trace then established both failure modes:

1. Complete 61-draw title resolves were followed by runs of 3-7 incomplete 31/34/58-draw
   resolves. The old guard retained only the first two and published the third, exposing black
   until the next 61-draw frame.
2. The bright transition used complete 35-39-draw resolves with intermittent 3-7-draw
   resolves. The old absolute 59-draw threshold never classified those relative collapses as
   underfilled.

The bounded hold now lasts eight resolves (the longest measured transient was seven resolves /
0.46 seconds) and also recognizes a new frontbuffer with fewer than half the draws of an
established frame containing at least 16 draws. A genuine sustained lower-complexity scene still
publishes on resolve nine; the trace observed that path before the diagnostic instrumentation was
removed.

Final diagnostic-free evidence is
`ghidra_out/title_ui_re/native_934_full_flash_fix_clean_50s.mkv`: all 2,374 captured frames were
scanned for near-black output. The only black interval was the initial 0.55 seconds before the
window received its first image; there were zero black intervals afterward, including the full
white transition and settled title. `Fable2_934.log` has no queue timeout, device removal,
GPU-loss, retirement, or submission error. PM4 tests pass with 482 assertions in 29 cases and
native plugin smoke passes. Source and staged `rexgpu-native.dll` are both 1,326,080 bytes with
SHA-256 `6B3AEE8639D2A115CCFAA59DC2AB45FDC91BC04D5BBAD9752785471AE7932B47`.
The clean native run is left open as PID 11192.

**PREVIOUS RESUME (2026-07-27, title-screen flashes partially suppressed):
the remaining full-screen black flashes came from genuinely underfilled native frontbuffer
resolves, not the swapchain, presenter mailbox, fallback clear, or a lost GPU copy.
Compositor-level desktop sampling caught the black frames while every resolved-target publish
returned success. Per-resolve GPU readback then showed the exact black source images. Complete
title passes contain more than 59 retained draws (normally 82-85); the flashing intermediate
passes contain 55-59. Full PM4 accounting confirmed that this is a real shorter command pass
(290-294 draw events versus 318-321), not draws lost by the proof filter.

`ResolvedTargetEntry` now records its draw count. For the active full-size frontbuffer only, a
resolve at or below 59 draws retains the preceding greater-than-59-draw image for at most two
consecutive underfilled passes. A third consecutive short pass is accepted, so a sustained
lower-complexity scene or real fade cannot freeze indefinitely. This removes isolated
intermediate flashes without content readback or a hard scene-specific image test.

The diagnostic run `Fable2_823*.log` produced 0 black compositor samples in 140 frames after the
guard, versus repeated black events within seconds before it. All temporary publish tracing,
per-frame readback/BMP output, cross-plane retry experiment, and forced end-of-copy fence wait
were removed. The final clean run is `Fable2_824.log`: no trace/readback lines, no D3D12/device
errors, and 0 black frames in 180 compositor samples (minimum sampled RGB mean 11.098). Evidence:
`ghidra_out/title_ui_re/desktop_clean_guard_samples_824/samples.csv` and
`ghidra_out/title_ui_re/native_flash_guard_clean.png`.

Release tests pass with 469 assertions in 28 cases, and native plugin smoke passes. Source and
staged `rexgpu-native.dll` are both 1,308,160 bytes with SHA-256
`EC3FD356B14461428A24CE3CC41FCAC848391BC3A8A964F734384FFB11574D94`.
Only the plugin was staged; the nightly runtime remains untouched. The verified clean native
D3D12 run is intentionally left open as PID 14904 with `REXGPU_NATIVE_RESOLVE_HEAP=1`.

**PREVIOUS RESUME (2026-07-27, complete native title composition):
the native D3D12 resolve-heap path now renders the complete Fable II title screen: scenic
background, opaque animated logo, `Press A to start`, and the standalone green A icon. The
decisive diagnostic-free capture is
`ghidra_out/title_ui_re/native_viewport_final_clean_retry.png`; the earlier
`native_viewport_fixed_composed.png` independently proves the same result with the bounded probe
still present.

The missing content and oversized/cropped composition shared one root cause: translated vertex
shaders require Xenos `PA_CL_VTE_CNTL` plus `PA_CL_VPORT_XSCALE..ZOFFSET` state. Draw events now
capture register `0x2206` and raw registers `0x210F..0x2114`, preserve them through pending-draw
coalescing, map VTE bits 8/9/10 to the translated shader's divided-by-W/reciprocal-W flags, and
convert the guest viewport scale/offset into the full-target host NDC transform. The exact A draw
uses VTE `0x0000043F` and viewport values `640, 640, -360, 360, -1, 1`; honoring bit 10 keeps its
roughly 40-valued W from being reciprocated, while the guest viewport restores normal 1280x720
placement for every title layer.

The isolated controller draw is preserved as
`Fable2Recomp/out/build/win-amd64-nightly/rexgpu_native_controller_icon_after_draw.bmp`. It shows
the centered green A on a clear target. Descriptor routing was also proven correct: all six
signed/unsigned texture bindings for fetches 13/14/15 and all three samplers were bound. The
temporary isolated submission, readback, shader dumps, and verbose controller logging have been
removed from the final source. `Fable2_812*.log` contains no controller-probe messages and no
D3D12/device-loss/submission error.

Release tests pass with 469 assertions in 28 PM4 cases, including regression coverage for VTE
and all six viewport registers; native plugin smoke also passes. Source and staged
`rexgpu-native.dll` are both 1,306,112 bytes with SHA-256
`B59BFF67DADD53DA80D96487B90EDAC24BDA7920C9D146A455286FC40ECA999F`.
Only the plugin was staged; the existing nightly `rexruntime.dll` remains untouched. The final
diagnostic-free native run is PID 4128 with `REXGPU_NATIVE_RESOLVE_HEAP=1`.

**PREVIOUS RESUME (2026-07-27, native title/UI composition and missing A isolated):
the owned D3D12 renderer reaches the title and draws the animated blue/sparkle contribution of
the Fable II logo plus the two prompt text runs. The latest user screenshot is
`C:\Users\Cornelio\AppData\Local\Temp\ai-chat-attachment-2324873055422847686.png`: black
background, blue sparkling FABLE II mask, and `Press     to start`. The opaque logo, scenic 3D
background, and controller glyph are still absent.

The exact A producer is proven. Each title frame ends with a 30-index DXT5A font draw (`Press`),
one 6-index generic image quad (A), and a 42-index font draw (`to start`). The middle draw is
`Art/GUI/Controller/icon_button_a.tex`, not the ABYX atlas: guest base `0x0D367000`, 64x64,
pitch 64, Xenos format 6 (`k_8_8_8_8`), tiled, endian 2, swizzle `0x60A` (BGRA), VS
`6D15306961102F7D`, PS `4B61E208208F3F5B`, and SrcAlpha/InvSrcAlpha blend `0x07060706`.
Live geometry is valid: full 0..1 UVs, alpha about 0.498329, color `00FFFFFF`, and a roughly
34x33-pixel transformed box near screen center. The texture detiles and decodes correctly to the
green A. Native upload order matches the verified decoder: tiled address -> endian-2 dword swap
-> RGBA8 upload -> BGRA SRV mapping. Resume downstream of upload: descriptor binding, PSO
execution, depth/composition ordering, resolve accumulation, and final presentation.

Artifacts:

- `ghidra_out/title_ui_re/live_0D367000_0000000100000000_100000000_64x64_rgba.png`
- `ghidra_out/title_ui_re/title_prompt_16_glyph_crops.png` (the earlier 16-quad batch is
  `Lionhead Studios.`, not the prompt)
- `ghidra_out/title_ui_re/title_pid17836_1A93D000_512x256_bc4.png`
- `Fable2Recomp/out/build/win-amd64-nightly/logs/Fable2_802.5.log`, around
  `captured controller-icon geometry`
- `Fable2Recomp/out/build/win-amd64-nightly/pm4_captures/pm4_press_a_2026-07-26.f2pm4`

This session also added Xenos component-swizzle composition to frontend and host-resolved SRVs
plus real per-fetch sampler filtering/addressing/border state. DXT5A/BC4 now replicates R into
RGBA, and formats such as the A icon honor BGRA. The staged build contains bounded one-shot title
diagnostics. All 28 focused cases pass (463 assertions), and native plugin smoke passes.

Current staged/running state: `Fable2.exe` PID 4068; native D3D12 backend; resolve heap enabled.
Source and staged `rexgpu-native.dll` are both 1,313,792 bytes with SHA-256
`D67F369D7845D663E299524311B81B672CD8D8040314EA6228240ED951EF97A1`.
One later source-only diagnostic retarget changes the direct controller-icon probe from the ABYX
atlas to standalone `0x0D367000`; it has not been rebuilt or staged. The running binary's
frame-order trigger already caught and logged the exact standalone A draw, so rebuilding this
probe is optional rather than a prerequisite.

**Architecture directive:** the destination is a native PC game pipeline, not permanent Xbox 360
EDRAM emulation. PM4/Xenos/EDRAM knowledge is an input-decoding and compatibility bridge only.
Convert it as early as practical into unrestricted PC heaps, persistent D3D12/Vulkan render
targets, explicit resource states and synchronization, native depth/HDR formats, host texture
caches, and direct presentation. Do not reproduce the 360's 10 MiB EDRAM or guest-RAM round trips
as the final architecture. Preserve enough guest semantics to run unchanged game code, but own
the rendering graph and memory model on PC.**

**PREVIOUS RESUME (2026-07-26, UI source isolated and native resolve heap prototyped):
the missing title prompt/menu is not an undiscovered font format. `Fable2_759.log` proves the
menu-era swap has 45 supported textured draws plus 74 textureless companions. The decisive source
trace shows the final frontend shaders sampling guest address `0x0C6E4000` (560x360 RGBA8 and
aliases), while its content fingerprint is the deterministic all-zero hash. PM4 resolve event 1
explicitly writes that same address. The current blocker is therefore guest render-target/resolve
routing: valid UI/material textures exist, but the offscreen surface consumed by the frontend was
never produced by the native backend.

An opt-in first native PC render-target heap now exists behind
`REXGPU_NATIVE_RESOLVE_HEAP=1`. It captures color/depth target identities per draw, groups draws
against the resolve source base, renders at resolve boundaries, stores D3D12 textures keyed by
guest destination address, rebinds those host textures without a guest-memory round trip, and can
present a captured frontbuffer. Runs 760-764 completed thousands of GPU-only resolves without a
D3D12 error. The prototype surface is still black: the remaining bug is in seeding/exporting the
selected EDRAM source pass, not texture discovery. It remains opt-in so normal boot video and the
upright legal frame are not regressed.

Default build `Fable2_765.log` is staged and intentionally left running. Its timed capture
`rexglue-src/out/native_765_resolve_heap_gated/frame_04.png` re-verifies readable upright legal
text. All 28 focused tests pass (27 parser cases / 447 assertions plus native plugin smoke).
Source and stage SHA-256 match: `rexgpu-native.dll`
`C0B97405FDF2DE5E20CF72E0CAE2C6BD487293F5EADD7DA9639F3E0CD7A407F8`;
`rexruntime.dll` remains
`0659BCBABDC41F1EEAE1C469637B92BA2A79327A2712CEBB8E27868594BBD73B`.
Next: add a bounded readback for the selected resolve source and preserve EDRAM target contents
across partial resolves, then enable the heap only after `0x0C6E4000` becomes non-black.**

**LATEST RESUME (2026-07-26, first upright native frontend frame):
the owned D3D12 path now advances beyond both logo videos and renders Fable II's legal/frontend
text through the normal guest-swap path. `Fable2_746.log` moved planar video presentation from the
old eager path to guest swaps while deferring 27 incomplete companion draws. `Fable2_747.log`
inventoried the first frontend texture layouts. The renderer now snapshots, endian-converts,
untiles, uploads, and samples tiled RGBA8, 10:10:10:2, RGBA16F, BC1, BC2, BC3, and BC4/DXT5A
textures. Base mip data is usable even when the guest descriptor exposes later unpacked mips.

Xenos `RB_BLENDCONTROL0` and color-write state are captured per draw, included in native PSO keys,
and translated to D3D12 blend factors/operations. This removed the opaque black UI quads.
Frontend geometry uses the guest's top-down clip convention while the bottom-up planar path keeps
its proven video transform. `rexglue-src/out/native_753_frontend_oriented/frame_00.png` is the
decisive first upright, readable Fable II frontend frame. `Fable2_755.log` is the final staged
build and additionally binds native 1D R32F/RGBA/RGBA16F lookup textures. The prior flashing black
square and decoder-transition garbage remain absent.

All 28 focused PM4/native-plugin Release tests pass. Source and nightly-stage hashes match:
`rexgpu-native.dll` `5A15EC1E226E851C6EEB58D5A3D2F1596C0AD53CF4E52F4FE2C77A292B877419`;
`rexruntime.dll` `0659BCBABDC41F1EEAE1C469637B92BA2A79327A2712CEBB8E27868594BBD73B`.
The current `Fable2.exe` process is intentionally left open for visual inspection. Next, support
the depth/HDR postprocess inputs, preserve more than the guarded 128-draw prefix,
and route guest render targets/resolves so the legal text composites over the actual menu scene.**

**LATEST RESUME (2026-07-26, native PM4 register alias fixed; boot video promoted to normal D3D12):
the terminal swap-mailbox stall was caused by the native command processor, not Fable's heap or
timing. PM4 Type-0 register writes carry 15-bit register indices, but
`NativeGraphicsSystem::WriteSideEffectRegister` multiplied those indices by four and routed them
back through the 16-bit MMIO aperture. Fable's 1,024-register constant upload beginning at
`0x4000` therefore wrapped `0x41DC/0x41DD` onto low `SCRATCH_UMSK/SCRATCH_ADDR`, zeroing the
scratch writeback configuration. The following `SCRATCH_REG1=1` could not publish mailbox word
`0x1FC83004`, leaving `WAIT_REG_MEM` blocked.

PM4 register side effects now read and write the full register index directly; only real guest
MMIO accesses use the 16-bit byte-addressed aperture. A regression sends the exact 1,024-dword
Type-0 shape and verifies `0x41DC/0x41DD` remain distinct from `0x01DC/0x01DD`. All 28 focused
PM4/plugin tests pass. The full CTest inventory still has nine unrelated migration/template tests
failing because their SDK template resources are absent in this worktree configuration, plus the
known unbuilt PPC target; none of the renderer tests failed.

Clean D3D12 live logs `Fable2_733.log` and `Fable2_738.log`, run without the forced-wait
diagnostic, are the decisive proof. They translate Fable's first VS to DXBC, create the VS/PS PSO,
present the first vertex-backed draw, and then submit changing three-plane YUV frames continuously:
frames 1/2/3/4/8/16/32/64/128/256 have distinct content hashes and rising draw sequences through
7017. Host-vblank polling reached 600 polls, 291 submitted frames, and only five incoherent
snapshots; the ring kept advancing and dispatched every observed interrupt. There was no malformed
span, rejected PM4 packet, D3D12 submission failure, or device loss.

The earlier near-white BMP was intentionally captured only from streaming frame 1 and was stale,
not evidence of a YUV-range failure. Timed live-window captures show the real animation and color:
`rexglue-src/out/native_738_boot_sequence/frame_14.png` is a clean Microsoft Game Studios frame,
and frame 20 shows the Lionhead animation. The live capture initially exposed one real presentation
defect—the planar video was vertically inverted. The streaming proof now reverses each Y/U/V
plane's rows during host upload, preserving the required D3D NDC transform while presenting both
logos upright.

The bridge is no longer opt-in. Presenting D3D12 initialization enables planar streaming by
default, while headless initialization remains unchanged. `Fable2_739.log` and final-binary
`Fable2_740.log` were launched with neither `REXGPU_NATIVE_STREAM_TEXTURE_PROOF` nor the eager
proof variable. Both automatically recognized the real 4:2:0 layout; log 740 reached frame 128 in
eight seconds, and `native_739_normal_sequence/frame_12.png` shows the upright Microsoft frame.
Normal streaming no longer enables diagnostic BMP/DXBC dumping. The old stream variable remains
only as an explicit `0`/`1` diagnostic override.

The first user-visible run exposed a flashing black rectangle during the movie and bottom-edge
garbage after it. `Fable2_741.log` showed two presentation owners: each coherent planar frame was
followed by a retained incomplete companion batch. The planar stream now takes exclusive
presentation ownership, discards the stale pre-video batch, and suppresses generic proof batches
while it owns the output. A second problem was independent host-vblank memory polling: it sampled
decoder surfaces outside the guest draw boundary as the surfaces transitioned, producing the
post-video garbage. Streaming is now paced exclusively by completed guest planar draws.
`Fable2_743.log` records both decisions, and the 50-frame
`rexglue-src/out/native_743_draw_paced_sequence/` capture stays clean and byte-stable after the
Lionhead fade (frames 37-49). The next renderer task is normal swap/pacing and later frontend
composition.

Use both `--gpu_plugin native --gpu_backend d3d12` for visual validation; `--gpu_plugin native`
alone selects the headless validation backend. No native renderer environment variable is required.
Clean source Release hashes are
`rexgpu-native.dll` `4DF669BCFA14FE821643C476068E6C8B7FB9DD3DE1EE26053320B4DA663FB264`
and `rexruntime.dll` `0659BCBABDC41F1EEAE1C469637B92BA2A79327A2712CEBB8E27868594BBD73B`.
The nightly stage contains these source test DLLs for the next visual run; the recoverable official
backup remains at `_stage_backup_host_texture_20260726`. No Fable process remains.**

**LATEST RESUME (2026-07-26, first native video frame + producer-stall isolation):
`REXGPU_NATIVE_STREAM_TEXTURE_PROOF=1` now enables a continuous, backend-owned YUV video bridge.
It recognizes the genuine three-plane linear R8 4:2:0 draw, retains sequence 106, samples all
three guest planes on each native host vblank, brackets each plane with dirty generations, verifies
the complete set again for cross-plane coherence, fingerprints content directly, skips duplicate
frames, and submits changed frames through the existing three-slot D3D12 ring. Draw preparation,
submission, presentation, and vblank polling are serialized only in this opt-in mode. The old
one-shot diagnostics are unchanged when the variable is absent.

This produces an authentic decoded Microsoft-logo frame through the native renderer
(`out/stream_client_709.png`). `Fable2_710.log` then performed 600 direct vblank polls in ten
seconds: one submitted frame, 600 identical snapshots, no dirty-generation changes, and no
D3D12/device-loss error. Thus the static image is not a missed texture invalidation or presenter
bug; the guest stops producing later YUV surfaces after sequence 106.

A controlled A/B run with the same executable/runtime and `rexgpu-xenos` reaches the real Fable II
title in about 12 seconds (`out/xenos_window_711.png`) with immediate nonzero audio. This proves
the Bink stream, title executable, and runtime are healthy and narrows the native blocker to
guest-visible GPU scheduling/synchronization. Native PM4 side effects now mirror `EVENT_WRITE`,
`EVENT_WRITE_SHD`, `EVENT_WRITE_EXT`, and `EVENT_WRITE_ZPD` into `VGT_EVENT_INITIATOR`; EXT also
writes the mature-Xenos-compatible full-screen extents. `REG_RMW` now operates through the
guest/MMIO-visible register callbacks. These changes materially restored several seconds of
nonzero native boot audio (`Fable2_712.log`, `Fable2_714.log`, `Fable2_715.log`) but did not release
the next video frame.

`FABLE2_PM4TRACE=1` now records real type-3 opcode/body data. `Fable2_713.log` shows sequence 106
finishing normally, followed by `SET_BIN_MASK_LO` and a conventional frame-boundary
`WAIT_REG_MEM` at `0x1FC83006` for reference 1. There is no malformed or rejected packet and no
later guest command buffer. Moving native host-vblank polling after guest interrupt dispatch did
not change the stall.

The follow-up `Fable2_716.log` separates PM4 interrupts from the 60 Hz vblank timer and wakes the
guest-aware `XHostThread` immediately, matching mature-command-processor behavior. All nine queued
interrupt packets were dispatched; the last queue-to-callback latency was 7 microseconds and the
pending mask was zero. The earlier wait at encoded address `0x1FC83002` resolved, but the terminal
wait remained at `0x1FC83006`, observed value 0 versus reference 1, after 6,000 iterations. At that
point the primary ring remained read 43 / write 55 while the GPU counter reached 735. Interrupt
latency is therefore eliminated as the blocker. Next, identify the missing guest/CPU-side write to
aligned semaphore word `0x1FC83004` and the native GPU completion/read-pointer behavior expected
to provoke it. Do not spend time on more texture/presenter work until that producer wait is
explained. Ghidra was not needed and remains free for the other agent.

There is now a decisive motion proof. Disabled-by-default
`REXGPU_NATIVE_FORCE_STALLED_WAIT=1` publishes the correctly endian-swapped reference only after an
individual memory wait has stalled for 1,000 polls. It is a diagnostic, not the intended fix.
`Fable2_717.log` used it to advance the swap handshake and submitted **four native YUV frames**,
including distinct contents `7F7CDC0EC93BEE73`, `0202E53F125620C4`, `5AC2A50BC03B0506`, and
`949DB859515516BA` at draw sequences 106, 133, and 187. This proves the owned renderer can display
changing boot-video frames. They advance only every few forced waits, not at real-time speed.

`Fable2_718.log` traces the forced chain through encoded waits on the `0x1FC83000` mailbox for
values 1/0/4/0x200 and known frame-end pointers/device state such as `0x82BA2DB8`,
`0x82B90160`, and `0x834A600C`. Existing decomp confirms `Function_82BA2F68` emits the
wait-for-1 / callback / wait-for-0 pair around `Function_821F6050`; this is the D3D swap-ring
callback handshake, not a renderer heap limit.

Native GPU commands now also run on an `XHostThread`, like the mature command processor, so PM4
interrupt callbacks execute synchronously on the command thread. `Fable2_719.log` proves 7/7
synchronous callbacks at 3–4 microseconds, but the unforced mailbox still remains 0 versus
reference 1. Thread identity is ruled out. Next, compare/decompile `D3D_GraphicsInterruptCallback`
at `0x82BA26B0` and the callback-packet builder `Function_821F6050`, focusing on the
`EVENT_WRITE_SHD` completion words at `0x1FC84000` and the expected publication into
`0x1FC83000`. This is now a concrete Fable-specific question for which Ghidra would be useful; if
the other agent is actively using it, they can finish their current work first.

Debug and Release pass **435 assertions in 26 test cases**, and both native-plugin smokes pass.
Current source Release SHA256 is `rexgpu-native.dll`
`C91F862C47EBA940E81632257AB7C82100904936FE6CEA60DA65335852E58294` and `rexruntime.dll`
`068496CFBEAF71B40DD081493E5ED701AD89BCF7032F0266EDF8C020CC907D50`.
The official nightly stage is restored to `rexruntime.dll`
`B04B0E823892DF0FFEF165D0E5473AC0CB152C54568320876F57812509F2963B` and
`rexgpu-native.dll`
`C9CAF88EE658E9D443EEE6BBE03DAAFB57B6E3992CFD713A5529A5A700C598FF`; no Fable process remains.
We are close to a boot video in renderer capability—the first real decoded frame and the
continuous submission path work, and the diagnostic now proves multiple changing frames—but the
swap-ring handshake must be fixed before motion is correct or real-time.**

**LATEST RESUME (2026-07-26, native texture lifetime + grouped frame submission): the one-shot
YUV upload is now a real backend-owned D3D12 cache. A backend-neutral key combines all six raw Xenos
descriptor dwords, the exact visible byte length, and a content fingerprint, so guest-address reuse
with changed data cannot return a stale texture. Cache entries own persistent default-heap
resources; upload heaps remain transient. D3D12 allocation sizes, live/peak bytes, hits, misses,
LRU evictions, skipped guest snapshots, deferred transient bytes/resources, slot waits, and peak
draw submissions in flight are exposed in backend diagnostics. The correctness-first cache has a
256 MiB host VRAM budget.

The native graphics system now registers a physical-memory invalidation callback and maintains
per-4 KiB dirty generations for watched texture ranges. Snapshot capture reads generation before
and after the guest copy and retries once if a writer races it. Once a ready host entry exists, an
unchanged generation reuses its full content key without copying or hashing guest memory. A write
invalidates the whole coalesced watched resource range on its first protection fault, avoiding one
fault per page while ensuring an address-only match can never serve stale video.

Live coverage exercised both sides. `Fable2_698.log` observed all three streamed YUV planes change
after capture (`reusable=0/3`), correctly refusing the shortcut. Final `Fable2_699.log` rejected an
incoherent sequence-104 capture while the producer was writing, then uploaded and replayed stable
sequence 106 from the persistent cache and reported `reusable=3/3`; there was no second upload and
no D3D12, device-loss, or GPU error. `Fable2_696.log` remains the deterministic cache proof:
1,507,328 actual D3D12 bytes, three cache hits, and the known 921,600-pixel readback
`EA1BD6D13ECFB239`.

Transient GPU resources no longer depend on local `ComPtr` lifetime or an upload-time whole-queue
wait. New backend-neutral `SubmissionRetirementQueue<T>` batches move-only resources by submission,
tracks live/peak bytes, and releases only through the completed fence value. Texture entries become
queue-ready immediately after submission because later direct-queue work is ordered; their upload
heaps remain owned by the retirement queue. Diagnostic readbacks use the same safety path,
including failed fence-signal handling. Final `Fable2_701.log` assigns three upload heaps
(1,474,304 bytes) to submission 103, exercises fence-completed transient retirement, replays all
three cached textures, and preserves the exact `EA1BD6D13ECFB239` result without a GPU/D3D12 error.

The remaining pre-draw whole-queue wait is gone too. Three `DrawSubmissionSlot`s independently own
their command allocator/list, shader-visible view and sampler heaps, constant buffer, compact
vertex memory, and index memory. A slot waits only for its own previous fence before reuse.
Persistent textures record their last referencing submission, so LRU eviction skips resources
still in flight. `Fable2_702.log` is the live proof: submission 4 was queued with two draw
submissions simultaneously in flight, then the run reached the YUV upload/cache replay and exact
921,600-pixel `EA1BD6D13ECFB239` frame with no GPU/D3D12/device-loss error.

Adjacent retained draws now coalesce only when their complete captured GPU payloads are identical
and their sequences are contiguous. Equality covers shader pair, primitive/index state and bytes,
vertex fetch/data, fetch/float/bool/loop constants, texture signs, descriptors, and captured
texture bytes. The backend then records every logical repetition in one command list and performs
one output copy/presenter refresh for the run. `Fable2_704.log` collapses the first 24 candidates
to one submission batch while retaining all 24 draw calls. It preserves both the three-slot
two-in-flight proof and the exact textured-frame hash `EA1BD6D13ECFB239`, with no GPU, D3D12, or
device-loss error. Sequence adjacency prevents a run from crossing a rejected or unsupported draw.

Heterogeneous retained runs now execute beneath one presenter refresh. The first run clears the
private accumulation target, intermediate runs never touch guest output, and only the final run
copies the completed target. When the run count fits the three-slot ring, all command lists also
share one fence submission; failure recovery explicitly signals already queued work. Intermediate
diagnostic waits are suppressed and the final accumulated readback remains authoritative.
`Fable2_706.log` proves this on a real 26-draw frame: the 24 identical point draws plus two
different vertex-backed draws become three command lists, one fence signal, one output copy, and
one presenter refresh. This saves two signals and two output copies while preserving the exact
518,400-pixel rectangle hash `091707070CB32225` and textured-frame hash `EA1BD6D13ECFB239`.

The focused Debug/Release suite passes 426 assertions in 26 cases; both plugin smokes pass.
Source Release SHA256 is `rexgpu-native.dll`
`CCDE2B545A39443B0D161DA6DCAE14B563337DDB9922CAC20A5252EA507C7AEF` and `rexruntime.dll`
`2E6450EBD4311EA9A1956BFCF1F895BBE92BAD0E5CC2351663502971BF820764`.
The official stage is restored to runtime
`B04B0E823892DF0FFEF165D0E5473AC0CB152C54568320876F57812509F2963B` and native plugin
`C9CAF88EE658E9D443EEE6BBE03DAAFB57B6E3992CFD713A5529A5A700C598FF`; no Fable process remains.
Next: place per-run constants/descriptors/vertex/index data in offset slices so these grouped
command lists can collapse into one command list. After that, broaden texture formats/layouts and
add title registration/rebind/destruction hooks.**

**LATEST RESUME (2026-07-26, Skate/audio/native-memory pass): Skate3Recomp was audited at
`f6e0ae87fdfecbadb5c1e36c55d66a744187a3cd`. Its useful pattern is title-specific hooks publishing
an immutable native `FrameScene`, host mesh/texture stores keyed by guest identity and content
fingerprint, a small D3D12/Vulkan RHI, and per-frame emulated fallback. It confirms our P2-to-P5
direction; its Skate-specific renderer is reference-only (no top-level license file).
`docs/NATIVE_PC_MEMORY.md` now defines the correct dual-domain migration: keep a 32-bit guest
compatibility arena only for untranslated ABI-visible pointers, while renderer/audio/video/assets
move into ordinary 64-bit host allocations behind stable IDs or generation-checked handles.

The user's choppy-audio report was traced separately from heap. Log 692 submits signal-bearing PCM
at the exact expected `48000/256 = 187.5` chunks/s. The 3,856 `BaseHeap::AllocFixed` collision
messages are a one-time ~30 ms startup probe scan with no OOM. The SDL path now includes a bounded
wait for replacement chunks, wall-clock-paced guest credits, five-second underrun/queue/callback
stats, configurable device buffering, a 16-frame queue, and an above-normal audio worker. Live
instrumented log `Fable2_694.log`: first startup window had 59 silence chunks while filling; the
next two windows were 187.4 chunks/s, zero silence, queue depth 16, and 10 ms max callback gap.
Debug/Release builds pass; PM4 tests remain 383 assertions/22 cases and plugin smokes pass.
Source Release runtime SHA256 is
`6154BE43E23D33508E797CE888ABF629328AF75857555AD76076ACE1D4AA4777`.
The official stage was restored after the diagnostic: runtime
`B04B0E823892DF0FFEF165D0E5473AC0CB152C54568320876F57812509F2963B`, native plugin
`C9CAF88EE658E9D443EEE6BBE03DAAFB57B6E3992CFD713A5529A5A700C598FF`; no Fable process remains.**

**LATEST RESUME (2026-07-26): `rexgpu-native` now snapshots and hashes live Xenos
float/bool/loop constants, packs the used float4 registers in translator order, binds the full
constant payload, snapshots DMA index buffers, and preserves the guest index endian mode through
`DrawIndexedInstanced`. Bindful pixel/vertex texture and sampler root tables are now populated
with valid dimension-correct null descriptors as the bridge to real texture upload. In
`Fable2_685.log`, the first formerly rejected frontend draw (sequence 104) is retained with 1 VS
float vector, 4 PS float vectors, a six-index DMA buffer, and three fully known pixel textures.
Those textures are a particularly useful first upload target: format 2 (`k_8`), linear,
endian-none surfaces at 1280x720 and twice 640x360. Sequences 104 and 106 now join the guarded
frame; the static frontend does not issue a later swap in the bounded run, so this new frame was
retained but not submitted. The earlier `Fable2_681.log` proof still submits 24 draws and produces
the complete 518,400-pixel rectangle. No PSO validation, D3D12 submission, or device-loss error
appears. The focused suite passes 382 assertions in 22 cases and Debug/Release plugin smokes pass.
Next: snapshot these three linear `R8` surfaces, upload them to host textures, replace their null
SRVs with real views, and trigger/capture a subsequent swap. The sequential fenced accumulator
remains correctness-first and should later be batched with per-draw resource slices. Vulkan
remains viable; `XenosRecomp` remains reference material rather than a dependency.

`Pm4ShaderCache` reduces the saved boot replay's 26,526 stage prepare requests to 19 exact owned
payloads (7 VS + 12 PS), with 26,507 hits and zero rejections. The deterministic replay hashes
remain unchanged.

`rexgpu-native` also owns live D3D12 and Vulkan host-submission paths.
It has an owned presenter, per-API device/queue/command recording and fence tracking, guest
vblank/interrupt dispatch, scratch-register writeback, endian-correct PM4 memory operations, and
blocking `WAIT_REG_MEM`. Logs `Fable2_656.log` (D3D12) and `Fable2_659.log` (Vulkan) complete
Fable's scratch/interrupt semaphore handshake, reach `XE_SWAP`, and each submit/present a 1280x720
diagnostic clear. D3D12 remains the fastest path to one known frontend draw;
Vulkan is viable now and should mirror the shared shader/pipeline/resource slice. `XenosRecomp`
remains useful reference material for offline cache/PSO design, but ReXGlue's existing translators
already accept our raw captured microcode and are now the implementation path. Ghidra is free for
the other agent until
a concrete Fable-only semantic ambiguity appears. Start with
[SESSION_SNAPSHOT_2026-07-26.md](SESSION_SNAPSHOT_2026-07-26.md), then
[OWN_RENDERER_TASKS.md](OWN_RENDERER_TASKS.md). No Fable process remains.**

**★★★ QUICK RESUME (2026-07-20 very late): the black-skin bug is now understood as COLOR/FORMAT +
TEMPORAL in the GPU composite resolve, NOT missing data. Under GPU readback the hero AND dog render
with full detail but blue-shifted/desaturated color + flicker; the composite guest data is a correct
natural-flesh face in ARGB, so on-screen blue = an R/B channel swap (or gamma) between resolve and
sample. The whole CPU pipeline (deliver→decode→tile) is PROVEN WORKING this session. Read
[SESSION_SNAPSHOT_2026-07-20.md](SESSION_SNAPSHOT_2026-07-20.md) ▶▶▶ LATEST STATE block + memory
`fable2-black-skin-renderdoc` top RESUME block. NEXT = fix R/B-swap/gamma on the composite resolve;
verify with `renderdoccmd thumb`. Tools: FABLE2_RT_DUMP + scratchpad/rtdump untile + headless
renderdoccmd (NEVER launch qrenderdoc GUI). ⚠ Claude launches the game, not the user.**

**★ (earlier) SESSION_SNAPSHOT_2026-07-20.md — artifact-hunt state, wins, modified files, env flags.
The sections below are the full narrative.**

## ▶▶ 2026-07-20 (morning) — ★★ ARTIFACT MECHANISM MEASURED TO THE INSTRUCTION + AUTONOMOUS TEST HARNESS SHIPPED
**Runs 549-557+ (user save Hero001 "Hero 2"). The handoff plan's two refusal-instrumentation items
are DONE and the answers re-aimed the fix. Memory: [[fable2-black-skin-renderdoc]] (mechanism) +
[[fable2-autonomous-test-harness]] (the new harness).**
- **Instrumentation shipped (`src/MorphFillTrace.cpp`, COMPLETIONDIAG):** QueryMulti-refusal
  classifier (thread-local watch on nested `rbtree_find 0x82B6E148`), per-texture FINAL-STATE
  table (promoted vs stuck + IsReady-refusal attribution via `tl_finalizingTex`), MoveFB0toFA4
  counter. ⚠ Summary cadence = FINAL%2048 (the PROC%512 trigger stalls at ~70-120 post-load).
- **MEASURED (repeated): QueryMulti NEVER refuses** (enq 70-149, refused=0 — the "~590 ret=0
  retries" are StreamMips early-outs). All reads insert 5FA4 first; most later promote to READY;
  a residue of 1-4 providers NEVER completes → the eternally-stuck textures (champion: DXT1 1024²
  mips=1 `4216Dxxx`, ~8k Finalize/IsReady refusals). **Parking spot VARIES: run 555 = RETRY 5FB0
  (1 entry, in-flight empty → v2's stall gate can never fire = v2 pump formally dead), run 556 =
  4 left in 5FA4, run 557 = healed to stuck=2 with no mechanism visibly firing (variance!).**
- **★ RETRY PROTOCOL DECODED (undisasm broken):** `FindBranchTo 0x82B6D020` → sole caller
  `0x821F7B5C` inside un-disassembled pdata fn → ClearAndDisasm → **`0x821F7A10` = the per-slot
  retry state machine** (0x1C-byte slots @ *(FUN_82ca9404()+0x18); state 0 idle→QueryMulti→1
  requested→IsReady?→2 done; **a state-1 slot BLOCKS all other slots from issuing** — one wedged
  read stalls the whole refill queue). On IsReady miss → **`0x82B6CE10`** = find 5FB0 entry, gate
  on reader `ready()` via provider+0x64 vtbl+0xC (THE delivery-RE stub site), only-if-1 consume
  (`0x82B81B28`) + `MoveFB0toFA4 0x82B6D020` (pure 5FB0→5FA4 move). `0x82B6CD08` = provider dtor.
  The driver DOES tick in gameplay (it makes the QM calls) → fix = make ready() turn 1 = poll the
  INNER reader (fill runs in the poll).
- **v3 RECOVERY POLL (`src/MorphFillPump.cpp`, env `FABLE2_RETRY_POLL` 1=observe/2=act, default
  OFF):** every 2s walk BOTH parking maps (5FB0 + 5FA4; MSVC-rbtree manual walk, SEH-guarded),
  log provider/reader state; act-mode polls the inner reader's ready() for stub-wrapper-class
  (0x82002A04) readers ONLY (⚠ other classes: +0xC is NOT an inner ptr — run-557 junk-deref
  lesson). **VERDICT PENDING: 4-run alternating baseline/combined series launched end of session.**
- **★★ AUTONOMOUS TEST HARNESS (user directive "automated testing doesn't work if I have to load
  the game"):** (1) **runtime scripted-input channel** in `rexglue-src` MnK driver —
  `FABLE2_INPUT_TAPS=<file>`, appended `TAP A|DPAD_DOWN|...` lines become controller taps,
  **focus-INDEPENDENT** (bypasses `has_focus_`; SendInput=denied foreground lock, PostMessage=
  dropped by the gate — runtime injection is the only reliable route); feeds the VK_PAD keystroke
  synth. (2) `tools/artifact_repro.ps1` — one-command A/B: stages/restores mystartup, launches
  nightly (⚠ `--gpu_plugin=xenos` REQUIRED — run 549 omitted it = headless boot, empty diag),
  **auto-loads the save: A → DPAD_DOWN → DPAD_RIGHT → A** (validated: `save loaded = True`,
  zero input), then kills/analyzes (compdiag summaries + offline per-tex stuck table).
- **★ LORE CORRECTIONS:** `SetSkipFrontEnd` DOES NOT EXIST (not in the 3592-native catalog nor
  startup.lua) — the "skip-boot autonomous repro" always sat at the TITLE SCREEN (the T+50s
  "heals" = the title scene). `SetInitialSaveGameName @0x8235AA08` exists but "Hero 2" didn't
  auto-load (consumer gate unknown; tap channel made it moot). Saves:
  `Documents\Fable2\B13EBABEBABEBABE\4D5307F1\00000001\Hero00N` + `Headers\...\Hero00N.header`
  (BE-UTF16 names "Hero 1".."Hero 6"; user's = Hero001="Hero 2", texturemorphs 48976B).
- **SERIES + v4 VERDICT (end of session): v3 = no effect (wrong reader class — the parked entry's
  wrapper is the FORWARDING class vtbl 0x820F9DA8, inner @ +0x10, ready/advance forward correctly).
  v4 (FORCE the wrapper's blocking advance() on 3-poll-stale 5FB0 entries) = forced delivery
  WORKS (adv=1, ready()→1) but the driver didn't consume: by then the game's own HEAL WAVE had
  invalidated the slot (state=3).** ★ THE FULL LIFECYCLE (run 579): wedge (champion 18k refusals,
  BOTH maps EMPTY — provider erased without READY) → after ~4min a SPONTANEOUS MASS RE-QUERY
  heals 159/159 → new wedge forms (same range key 7F83D652/0x568ED5/0x663D every run). Baselines
  identical (8k refusals). The artifacts = the minutes-long wedge windows.
- **★★★ AFTERNOON UPDATE — 3 BACKGROUND AGENTS LANDED THE MISSING PIECES; v5 BUILT:**
  (1) **WEDGE ROOT CAUSE COMPLETE (`ghidra_out/healwave_requery_re.txt`):** `0x82B6C960` is
  **CANCEL** (not WaitReady!) — destroys the provider (dtor 0x82B6CD08), erases the key, NO READY
  insert (callers 0x82A7AF68/0x82A91ACC eviction region). Canceled in-flight request + the
  driver's "no new query while a sibling slot is state-1" gate = permanent per-texture wedge.
  Heal wave = completing slots invalidate siblings (state 3) → per-frame reclaim `0x821F7830`
  (top of LOD pass `0x821F8F48`, r3=manager, cached at *(owner+0x18), objects vector @mgr+0x7C)
  resets state 3→0 → driver re-queries with its own args. ⚠ prior "FUN_82ca9404 ctx" was a
  misread (that's __savegprlr_27); slot arrays live per streamed-texture object @obj+0x18/+0x1C.
  (2) **v5 SLOT HEAL IMPLEMENTED (`src/MorphFillPump.cpp`, env `FABLE2_SLOT_HEAL=1`, default
  OFF):** hook `sub_821F8F48`; ~1 scan/s walks mgr objects→slots; a slot in state 1 across ~10
  consecutive scans is written state=3 → the same call's reclaim re-queries it (the game's own
  heal, seconds instead of minutes). Metric: champion peak refusals <~500 vs baseline 8k-18k.
  (3) **FRONT-END INPUT MODEL SOLVED (`ghidra_out/frontend_menu_re.txt`):** menus consume ANALOG
  left-stick state (GUI:IsPushingUp etc.) — NEVER DPAD buttons/VK_PAD keystrokes (zero VK_PAD
  imm16 hits in the exe; GUI dispatcher jump table has no dpad row). Tap channel extended with
  `LSTICK_UP/DOWN/LEFT/RIGHT` verbs (synthetic thumb axes, 24-poll hold; runtime rebuilt).
  Save fan: LOAD_CARD_ACTIVE = card focus (triggers preview reads), LOAD_CARD_SELECTED →
  load kick `0x82447160`. Autoloader now: retry-A until 'FrontEndMainMenu_New Game' button-spawn
  event (⚠ NOT 'ALLSB_FrontEndMainMenu' — the '..._GUI_SCREEN_OPTIONS' preload matches that
  substring pre-open), then LSTICK_DOWN → per-step preview-read/morph-size verification →
  LSTICK_RIGHT… → A; real-load confirm = region_specific_* bank mounts.
  (4) **FRAME-END/RENDERER MAPPED (`ghidra_out/frame_end_layer_re.txt`, own-renderer track):**
  "3D Engine" thread (RenderThread_3DEngine 0x823881C0) → DeviceState_FrameDispatch 0x82388380 =
  frame-end root → RenderQueue_ExecuteAll 0x82B668D0 → Frame_Present 0x82B6F1D0. ★ Game code
  submits render-command objects via **RenderQueue_SubmitToFrame 0x82B66CF0 (76 call sites = the
  renderer-class worklist for the P5 climb)**.
- **★★★ EVENING — THE VISIBLE ARTIFACT IS A *CONTENT* BUG, AND THE PRIME SUSPECT IS A MISSING
  DECOMPRESS (all residency/readback leads eliminated by measurement):**
  - v5 slot-heal verified firing correctly (run 583: HEAL#1/2 incl. THE champion key) and
    correctly found NOTHING during the user's artifact session (run 584: 0 wedges, all 343
    composites promoted) — **the wedge mechanism is real but is NOT the visible artifact.**
  - **CONTENT SAMPLER added (MorphFillTrace.cpp: SampleContent, 3×1KB windows, zero%+FNV in
    TOUCH/FINAL lines + CONTENT-SUSPECT list in the summary): the artifact = 6 streamed sources
    `4216ACB0..4216B1F0` (DXT1 512², the classic cohort) promoted with 66% ZERO content —
    first window has data, windows at +50%/+end are ZERO — byte-identical hashes across ALL
    runs (deterministic partial fill).**
  - **GPU-readback lead DEAD for this artifact:** the GummiFableII port already EXISTS in our
    runtime (command_processor.cpp: d3d12_readback_resolve + readback_resolve_only_dest_bases +
    log_resolve_readback; nightly Fable2.toml had a mid-experiment exclusion of 0x1E+). Tested
    scene-buffer readback denied AND fully allowed (674 readbacks/run of dest 1ECFF000): the 6
    sources' content bytes are IDENTICAL either way, artifacts unchanged (user-confirmed twice).
    (The femtofork fixed the FULLY-BLACK skin = our old pool-boost win; different layer.)
  - **▶▶ PRIME LEAD (next session): missing DECOMPRESS in the gameplay refill consume.** The
    champion range-read is len 0x663D (26KB) feeding a 128KB texture — matches "only first
    window filled"; greenish-black noise = compressed bytes rendered as DXT1. The load-time path
    decompresses (bank_block_load 82C6D288, compression types 1..3; type 3 = compressed-slice)
    but the delivery-RE showed the gameplay consume is a RAW memcpy (fill 0x82B56B60). TODO:
    (a) decomp how load-time turns a compressed slice into texture bits vs what
    TexStream_ConsumeReadIntoResource 0x82B6C340 does; (b) find the decompress call missing from
    the runtime-refill path; (c) fix = route the consume through the same decompress (host hook
    or guest-path correction). Success metric IS NOW MACHINE-VISIBLE: the 6 suspects' zero% →
    ~30s and hashes change (CONTENT-SUSPECT list empties) + user visual.
  - **★★ LATE NIGHT — DECOMPRESS MECHANISM FULLY RE'D (`ghidra_out/refill_decompress_re.txt` —
    READ THIS FIRST NEXT SESSION):** consume 0x82B6C340 provably never decompresses; compression
    is a per-bank-entry CLASS (raw 0x8200E02C ctor 82C72F60 vs compressed 0x8200E058 ctor
    82C73428, fixed-0x8000 chunk table); load-time works via the eager whole-entry decompressed
    CACHE (mgr 0x8333597C, entries ≤1MB → zero-copy views); gameplay refill creates a DEFERRED
    decompress job (ctor 82C763E8, run 0x82C76150, set-dest 82C760B0) with dest=NULL on async
    queue 0x8349709C (CS 0x834C22BC) that NEVER COMPLETES in the recomp → raw zlib bytes surface
    = the greenish-black patches. Forwarding wrapper 0x820F9DA8: inner JOB at +0x10, data field
    at +0xC. Clean sync API alternative: compressed_entry_sync_read 0x82B5ACC8(entry,&destPtr,
    decompOff,len,pri). ~50-row labels TSV in the file (unapplied).
  - **FIX B TESTED = REGRESSION (leave FABLE2_REFILL_WHOLEREAD OFF):** zeroing readdesc+0x10
    converted 45 reads BUT the classic cohort's content stayed byte-identical (their consume
    ignores it) AND a new 42653xxx-42654xxx batch promoted 100%-ZERO (whole-read delivered
    nothing). +0x10 is not a free lever.
  - **▶▶ NEXT = FIX A (primary, recipe with exact offsets in refill_decompress_re.txt §Q4):**
    step 0 = observation run: extend the existing consume hook (MorphFillPump GuardDeliver) to
    log wrapper vtbl + job state/dest fields for the suspect cohort; then implement: in the
    0x82B6C340 override, for forwarding-wrapper consumes with job-not-complete: hk_malloc dest →
    set-dest 82C760B0 → blocking-advance the raw child → run job 0x82C76150 (this=job+4) inline
    UNDER CS 0x834C22BC (the 0x8000 skip scratch 0x8333599C is a shared global — race risk) →
    write dest into wrapper+0xC → original consume. Success = CONTENT-SUSPECT list empties
    (machine-visible) + user visual.
  - **RENDERQUEUE CATALOG DONE (`ghidra_out/renderqueue_catalog_re.txt`) — ⚠ CORRECTION: the
    render-command queue is NOT the scene renderer.** All 76 SubmitToFrame sites = exactly TWO
    2D overlay command classes (FillRect2D vtbl 0x820FAEDC exec 0x82B67400; DrawText2D vtbl
    0x820FAEE8 build 0x82B67B10 exec 0x82B67CE8) used by the dev menu/profiler/subtitles/debug
    HUD. World/characters/terrain never touch it → **the own-renderer seam remains the PM4 ring**
    (per the original OWN_RENDERER.md plan). The queue IS a proven host-injectable 2D
    text/rect overlay channel (useful for Fable II Studio HUD). 82 labels applied (frame_end 26
    + healwave 16 + renderqueue 40); Ghidra repairs: bogus noreturn __savefpr stubs removed
    (0x823ABD38/0x82B67CE8 un-truncated), noReturn cleared on 0x822B88A8/0x821FABD0/0x82193E38/
    0x82173E38 — RE-RUN any old decomp that showed halts at those.
  - **FIX A IMPLEMENTED + TESTED = NEVER ENGAGES (run 590/591, artifacts unchanged):**
    `src/MorphFillPump.cpp` env `FABLE2_DECOMP_FIX=1` (kept as scaffold + its CONSUME CENSUS
    logging). Census (run 591): 83 consumes w/ forwarding wrapper 0x820F9DA8 + 40 w/ stub
    0x82002A04; **every fwd consume arrives with the data field (+0xC) ALREADY POPULATED and the
    inner (+0x10) NOT a decompress job (vtbl ≠ 0x8200E134)** → no pending job ever reaches the
    consume; delivery/decompress is NOT the failing step.
  - **★★ FINAL MEASUREMENT OF THE NIGHT (run 591, head-hex dump): ALL suspect textures have
    ALL-ZERO HEADS (first 16 bytes 00…) with their data sitting mid/late in the surface** (66%
    zero = head window empty, data elsewhere; zero DXT1 = black patches). Data IS delivered and
    copied — **INTO THE WRONG OFFSET inside the texture (the fill's window-placement math), NOT
    missing/undecompressed.** ▶▶ NEXT SESSION PRIME TARGET: the window math — decomp the fill
    window computation (fill 0x82B56B60's dest-window derivation + consume 0x82B6C340's variant
    dispatch 0x82B8D028→0x82B8CAC0/0x82B8CDF8 offset args, and mip-offset math in the provider
    texture-header fields +0x10/+0x1C); instrument consume dest ranges vs the texture's mip-0
    extent; likely recomp-side cause: an offset computed from a field the recomp leaves
    stale/zero. Head-hex + census instrumentation is in place to verify any fix instantly.
  - **★ DETECTOR RUN 594 (post-midnight) NARROWED IT FURTHER: (a) ZERO cancels fire in gameplay
    (missing-slice/cancel theory DEAD for the live artifact — cancels are eviction/destructor
    paths: 0x82A7AF68 = per-texture whole-read cancel key {hash,0,-1}; the 82A91ACC fragment =
    object dtor canceling all state-1 slots); (b) EVERY consume is a WHOLE-resource read (census:
    off=0 len=FFFFFFFF, data ptr already populated) — one whole copy still yields zero-head +
    data-mid/late.** → PRIME HYPOTHESIS NOW = 360 TILING/MIP-PLACEMENT mismatch in the CPU upload
    path (block-scramble = the classic signature; suspects are created mips=1 vs healthy mips=6 —
    a mips-dependent placement divergence is the top candidate). Upload path found:
    Function_82B95CE0 (mip-walking upload w/ format table DAT_8331dadc stride 0x70; partial
    decomp ghidra_out/decomp_upload_untile.txt); consume variants 0x82B8D028→82B8CAC0/CDF8 +
    0x82B8D170/82B8BA38 header parse. **Background agent dispatched at session end →
    ghidra_out/morph_upload_tiling_re.txt (full placement-math RE + fix design).**
  - Also NEXT: apply the ~50-row refill labels TSV; autoloader end-to-end still unproven.
    Fable2.toml: readback filter currently "" — revert to the 0x1E-exclusion after the content
    fix; SLOT_HEAL worth default-on regardless (verified, zero false positives).

## ▶▶ 2026-07-19 (night, decomp session) — ★ OWN-GRAPHICS-PIPELINE FOUNDATION: 360 D3D layer mapped, plan written
**User directive: build our own graphics pipeline (Vulkan/etc.), stop relying on rexglue's Xenos
emulation. Plan of record = [docs/OWN_RENDERER.md](OWN_RENDERER.md).** This session (pure Ghidra,
no build/run):
- **The statically-linked 360 D3D runtime found + carved:** it was the over-merged blob
  `0x82B9F038–0x82BAF08C` → 119 functions via new `tools/ghidra_label/CarvePdataRange.java`.
  65 labels applied (`ghidra_out/labels_d3d_layer.tsv`): `D3DDevice_Swap@0x82BA34D8`,
  `D3D_DeviceInit@0x82BA6990`, `D3D_InitRingBuffer@0x82BA2830`, ★`D3D_CmdBufReserveSpace@0x821E8EC0`
  (the kick every PM4 writer calls), `D3D_Resolve_EmitCopyDraw@0x82206F30`, swap thread, blits,
  fences, bin-mask emitters. Device struct: +0x30 PM4 write cursor / +0x38 limit.
- **★ KEY FINDING — the D3D API boundary is DISSOLVED (LTCG-inlined):** the draw-writer functions
  have ZERO references (no bl/b/pointers; `FindBranchTo.java` is new) while the kick has 144 call
  sites across game code → PM4 writing is fused into Fable's renderer. So "hook the D3D calls" is
  NOT viable. The clean seam = the PM4 ring itself (already the GPU-plugin interface).
- **Strategy set (OWN_RENDERER.md):** build `rexgpu-native` behind the same `--gpu_plugin` seam.
  P0 = shadow PM4 tracer validating our parser against Fable's real stream (~18 opcodes enumerated,
  all writers decompiled in `ghidra_out/d3d_writers_decomp.txt`); P1 CP parity → P2 own backend
  (D3D12 first, RHI kept thin for Vulkan later) → P3 own shader translation (finite shader set,
  offline-translatable) → P4 retire xenos → P5 climb to the game-renderer boundary via decomp.
- **Next:** P0 tracer implementation; decomp the Swap callers `0x82B6EB9C/0x82B6F408/0x82B6FB00`
  (Fable's frame-end layer = top of the game renderer); enumerate the register subset from
  SET_CONSTANT payloads.

## ▶▶ 2026-07-19 (late night) — ARTIFACT MECHANISM CORNERED BY A/B RUNS: scenario-specific FINALIZE refusal, NOT a global stall; autonomous repro loop established
**Runs 544-548 (user save-scene + NEW autonomous skip-boot repro). The stall premise is DEAD; the
real shape of the artifact bug is now measured. All instrumentation env-gated off by default.**
- **★ AUTONOMOUS REPRO LOOP (no user needed): activate `data/scripts/Startup/mystartup_skip.lua`
  (backup/restore `mystartup.lua`!), launch 60-150s with FABLE2_FILLTRACE=1 FABLE2_COMPLETIONDIAG=1
  [FABLE2_TEXSTREAM_PUMP=2], kill, read the log.** The artifact cohort's signature is fully
  log-visible: `[filltrace] HERO STREAM#/FINAL#` fmt=23 512x512 mips=1, `residState
  7FFFFFFF->7FFFFFFF` (stuck) vs `->00000000` (healed). ⚠ RESTORED after use (verified 26 bytes).
- **MEASURED FACTS:** (1) pump telemetry (152-line/5s cadence — the 0x821C3858 hook IS per-frame in
  gameplay): all 4 stream maps drain continuously in gameplay; no permanent 5FA4 parking → the
  parked-forever premise from log-532 era is gone in the current build. (2) FILLTRACE cohort = 27
  distinct HERO DXT1 512² mips=1 SOURCE textures: StreamMips CREATES their pool resource fine
  (+0x4c=4C12xxxx, and its ret = TexStream_QueryMulti 0x82B6C618's return); FIRST request enqueues
  (ret=1), all ~590 retries ret=0; Finalize 0x82A7B148 (flags=0) refuses ~24x/tex (not-in-READY
  gate 0x82B6CD58). (3) ★ THE A/B: skip-boot runs 547/548 both promote 24 textures at ~T+50s
  (guard on OR off — the game heals ITSELF in this scenario); the USER's save-scene run 546 = **0
  promotions over minutes** (and user-visible artifacts). → **scenario-specific: the customized
  hero's morph-source reads never complete vs default-adult's do.** Remaining lead: 5FB0 RETRY map
  showed a lingering entry (telemetry retry=1; PROC +6c=0 dispatches there; who re-drives 5FB0 →
  TexStream_MoveFB0toFA4 0x82B6D020?) + per-hero source-hash resolution.
- **Delivery guard + stall pump (v2, MorphFillPump.cpp) = SAFE, VERIFIED, default-off.** Guard
  classified 250+ wrapper-class consumes correctly across 4 runs (zerocopy path live, SKIPPED=0, no
  crash, no white screen). The stall pump never fired (correctly — no dormant stall exists). Keep
  the guard (crash-proofing for any future pump/advance work); the pump alone won't fix the
  artifacts.
- **NEXT (the plan): (1) instrument the two REFUSAL reasons distinctly — 0x82B6C618 ret=0 cause
  (key-found-in-WHICH-map vs res[1]==0 gate) + Finalize distinct-texture end-state summary; (2) run
  the A/B again (skip-boot autonomous vs user save-scene) with that; the diff pinpoints the broken
  step for customized heroes; (3) likely fix shapes: drive 5FB0→5FA4 re-dispatch on stall (extend
  the existing stall pump — infra already shipped) OR fix the per-hero source hash/read failure.**

## ▶▶ 2026-07-19 (night) — DELIVERY-AWARE TEXTURE PUMP SHIPPED (v2); worker-concurrency landmine mapped; ⚠ NOT yet verified in the artifact scene
**Implements the texstream_delivery_re.txt safe fix. `src/MorphFillPump.cpp` fully rewritten; env
`FABLE2_TEXSTREAM_PUMP=2` (any non-zero) arms BOTH hooks; default OFF.**
- **Hook 1 — DELIVERY GUARD on `TexStream_ConsumeReadIntoResource 0x82B6C340`:** before every
  consume, if the reader at provider+0x64 is the stub WRAPPER (vtbl 0x82002A04), drive the INNER
  reader (wrapper+0xC) through its OWN vtbl: ready(+0xC) poll (=the fill pump); accept zero-copy
  views (pendingChild==0, destBuf==0, dataPtr!=0 — ready() returns 0 for these BY DESIGN, do not
  block them!); blocking advance(+0x10) force-complete when a real destBuf exists; SKIP the consume
  (park = black, never AV) on error/undeliverable. **VERIFIED IN-GAME (runs 542/543): 65 zero-copy
  + 147 passthru consumes classified correctly, SKIPPED=0, no crash, load-time path unharmed.**
- **Hook 2 — STALL-GATED worker pump (in the 0x821C3858 per-frame hook):** invokes guest worker
  0x82B6D348 ONLY when pend(0x834A5FA0)==0 && inflight(0x834A5FAC)>0 && unchanged for 30 hook
  calls. TWO CRASH/BREAK MODES FOUND EN ROUTE (both understood, both fixed by the gate):
  (1) run 540 WHITE SCREEN: calling the worker unconditionally per frame runs PASS B
  (CustomAtlas_SyncFillStep) at times the game never intended → broke the title/UI atlas;
  (2) run 542 AV (third thread, guest null write): the worker is SINGLE-INSTANCE by design —
  its currently-processing latch globals 0x834A5FC8../0x834A5FD8.. are clobbered by concurrent
  worker instances (the real worker IS active at boot/load; 0x82B6C960 waiters then proceed
  early). NEVER run two workers concurrently; pump only on provable dormant-stall.
- **rbtree map layout (for host-side walks): map {+0 ?, +4 head node, +8 size}; node {+0xC..+0x14
  u96 key, +0x18 provider}. 5F98 PENDING size@5FA0; 5FA4 in-flight size@5FAC; 5FB0 retry
  size@5FB8; 5FBC READY size@5FC4.** In-flight throttle: rbtree_begin(5F98) refuses hand-out while
  inflight>=4 (uRam834a5fac<4 check).
- **⚠ BUILD GOTCHA (cost an hour): the ACTIVE build tree is `out/build/win-amd64-nightly`**
  (nightly SDK, plugin GPU via LoadGpuPlugin + rexgpu-xenos.dll). `win-amd64-release` is the STALE
  v0.8.0 tree — rebuilding it produces an exe whose D3D12GraphicsSystem import the patched
  (rexglue-src) runtime can't satisfy → STATUS_ENTRYPOINT_NOT_FOUND at launch. Always build/run
  nightly. (TOOLCHAIN.md §Run still says release — outdated.)
- **NEXT / VERIFY:** user plays to the adult-hero artifact scene with FABLE2_TEXSTREAM_PUMP=2 and
  WAITS ~10-30s: expect log `[texpump] stall detected` + guard `fast`/`advanced` counters moving +
  artifacts/pop-in clearing. If the stall never triggers in-game, instrument the 0x821C3858 hook
  call cadence (sparse at boot — 5 calls/17s — assumed per-frame in gameplay per DrawDistanceFix).
  If artifacts persist WITH the pump firing, the residual mips=1-DXT1 Finalize-skip hypothesis
  (FILLTRACE round 3) is a second, separate mechanism to chase.
- **USER DIRECTIVE (new, this session): plan + start our OWN graphics pipeline (Vulkan etc.) —
  plan of record written: [GRAPHICS_OWNERSHIP.md](GRAPHICS_OWNERSHIP.md) (G1 claim the plugin fork /
  G2 catalog the game's statically-linked D3D-360 API in Ghidra / G3 native renderer behind that API,
  D3D12-first with an RHI seam for Vulkan; G2 can start now as background decomp).**

## ▶▶ 2026-07-19 (evening, runtime/C++ session) — ★★ NPC POP-IN FIXED (user-confirmed) + pump-fix crash → DELIVERY root + GPU-reroute P1
**The implementation session pairing the decomp results below. Two user-confirmed wins shipped; the
last deep bug (skin artifacts + texture latency) is cornered to the async DELIVERY layer.**

- **★★ NPC POP-IN — FIXED & USER-CONFIRMED ("didn't notice the pop in this time").**
  `src/DrawDistanceFix.cpp` + env **`FABLE2_DRAWDIST=2.0`** (float, clamp <1→off, >4→4): writes the
  4 draw-distance multiplier globals `0x83319464/68/6C/70` (static/animated/villager/creature).
  ⚠ HARDENING GOTCHA: first hook site `ModelStreaming_PerFramePump 0x82A54500` is EVENT-driven
  (fired ONCE/session) — the guaranteed per-frame re-assert lives in **MorphFillPump.cpp's
  `sub_821C3858` hook** (TextureStreaming_UpdateAndPump, unconditional; cross-TU call to
  `drawdist::ReassertIfEnabled`). Applies from boot, survives level-load re-inits. Memory:
  [[fable2-npc-popin-drawdistance-fix]]. TODO: promote to a Fable2.toml cvar / default-on.
- **⚠ BLACK-SKIN PUMP FIX CRASHED → root is one layer deeper = DATA DELIVERY.** Built the
  recommended per-frame worker pump (`src/MorphFillPump.cpp`, `FABLE2_TEXSTREAM_PUMP=1`, verified the
  worker self-terminates) — **AV @ guest 0x9d62a698, HOST State=RESERVE (~18s)**: PASS C consumed a
  reader whose async data NEVER ARRIVED (ready-check `0x82EA63A8` is an always-1 ICF stub — it LIES).
  So: async reads OPEN but never DELIVER bytes into a committed buffer in gameplay (XMA-class).
  Pump kept DEFAULT-OFF as scaffold — re-enable ONLY after delivery is fixed. Escalation RE prompt:
  `ghidra_out/TEXSTREAM_DELIVERY_RE_PROMPT.md`; partial result `texstream_delivery_re.txt` (the
  reader at provider+0x64 is a DELEGATING WRAPPER, vtbl 0x82002A04: its ready(+0xC) AND advance/
  read(+0x10) are no-op `return 1` stubs — real data must come from an INNER reader @this+0xC,
  created by `TexStream_ReaderFactory 0x82B53D68` = the delivery frontier).
- **GPU texture reroute (break the 512MB physical wall): Phase 1 SHIPPED + tested** —
  `rexglue-src/include/rex/graphics/host_texture_heap.h` (HostTextureHeap: Alloc→{token,host_ptr},
  Resolve, IsToken; self-test ALL PASS; zero behavior change). **Phase 2 PAUSED on a real design
  issue:** the game CPU-WRITES texture data to the pool's guest addresses, so plain host tokens break
  CPU-filled textures — needs write-watch shadowing (EnableAccessCallbacks) or the deeper physical-
  addressing rework. Plan + P2 notes: `ghidra_out/GPU_TEXTURE_REROUTE_PLAN.md`. Don't build P2 as
  originally specced.
- **RAM measured (src/MemPressureDiag.cpp, FABLE2_MEMDIAG=1, per-frame sampler):** town square =
  PHYSICAL 330/512MB (182MB free), virtual v40 609MB/~1GB, v00 ~unused → **512MB is NOT the active
  bottleneck in normal play** ([[fable2-ram-memory-model-map]]). Aggressive pool-boost pop-in attempt
  REVERTED (pop-in was never capacity). Pool boost stays at the visible-skin floors (1024²=64, 512²=80).
- **Standard launch env now:** `FABLE2_MODAPI=1 FABLE2_MODTEXT=1 FABLE2_DRAWDIST=2.0` + the usual
  flags. All diags (MEMDIAG/ATLASDIAG/FILLTRACE/COMPLETIONDIAG) and experiments (TEXSTREAM_PUMP,
  MORPHMIPS, MORPHKEEP) default-OFF.
- **NEXT:** (1) finish the delivery RE (inner reader 0x82B53D68 → who reads/commits the bytes → why
  not in gameplay) → then re-enable the pump/implement the delivery fix = clears skin artifacts +
  texture latency in one shot. (2) Optionally tune FABLE2_DRAWDIST higher / make it a cvar.
  (3) GPU-reroute P2 only after the write-watch design is settled.

## ▶▶ 2026-07-19 (background decomp) — NPC POP-IN: draw-distance knob found (+19 `labels_npc.tsv`)
**Deliverable `ghidra_out/npc_streaming_re.txt`.** Two symptoms/owners: NPC GEOMETRY pop-in =
mechanism #1 (LOD draw/activation radius — a tunable, not a bug); NPC TEXTURE pop-in = mechanism #3
(the texture-completion/DELIVERY stall — shared with black skin, see below). **★ The knob: 4 global
draw-distance multiplier floats, all currently 1.0f, each with a Lua setter that also re-runs
`RenderMgr_RefreshEntityLOD @0x82368840`:** `DAT_8331946C` Villager (`SetVillagerDrawDistanceMultiplier
@0x823B3FF0`), `DAT_83319470` Creature, `DAT_83319468` AnimatedEntity, `DAT_83319464` StaticEntity.
Radius test = `IsEntityWithinDistanceOfLODCentre @0x8229BFD0` (VMX128 `Entity_SquaredDistToLODCentre
@0x822904E8`). **APPLYABLE VIA LUA NOW** (existing mod system): set villager→2.0, others→1.5 at level
init, zero native code (or host weak-override the 4 setters). Keep ≤2.0 (over-widening ↑ resident count
→ worsens texture pop-in until the delivery stall is fixed). Model-stream: `mesh_stream_poll_slots
@0x82AB4768` = hardcoded 3-slot cap; `ModelStreaming_PerFramePump @0x82A54500` per-frame time budget
@ctx+0xE78; `mesh_stream_finalize_slot @0x82AB49D0` ~36MB resident budget.

## ▶▶ 2026-07-19 (background decomp) — ★★★ BLACK-SKIN COMPLETION MECHANISM FOUND + SAFE FIX (VMX128-enabled)
**Deliverable `ghidra_out/blackskin_completion_re.txt`; memory [[fable2-black-skin-renderdoc]]; +7 fns
`labels_texstream_completion.tsv`.** VMX128 (now enabled) decompiled the worker's previously-truncated
tail → RESOLVED the contradiction: the skin-source-texture completion path is **PASS C of
`TexStreamCompletion_Worker @0x82B6D348`** (the 5FA4→READY drain), NOT a missing separate fn. Promotion
code is CORRECT + unconditional (ready-check `0x82EA63A8` = ICF `return 1`; copies data via
`TexStream_ConsumeReadIntoResource @0x82B6C340` before inserting READY `0x834A5FBC`). **WHY it fails:
the worker isn't re-pumped over runtime composite providers in gameplay** (0 disassembled callers, only
.pdata → undisassembled IO-thread runs at load, not for refills) — same class as the XMA async bug.
**★ SAFE FIX (C++, host-side weak-override in `Fable2Recomp/src/` — FOR THE BLACK-SKIN/RUNTIME OWNER,
not the decomp agents):** per-frame pump that invokes guest worker `0x82B6D348` when `0x834A5FA4`
non-empty, alongside `TextureStreaming_UpdateAndPump 0x821C3858`. Drives the game's OWN path (idempotent,
copies before promote — no forged state, no blind mip-write). Env-gate `FABLE2_TEXSTREAM_PUMP=1`.
Confirm gaps (decomp agent hardening now): RepairRegion the worker's caller-thread; trace
`resource_async_open_by_hash 0x82C68E50` byte delivery if pump promotes but data stays zero.
**★ UPDATE (round 2): the pump fix was BUILT+TESTED by the runtime team → CRASHED** (AV @ guest
0x9d62a698, HOST State=RESERVE/uncommitted): PASS C consumed a reader whose async data NEVER ARRIVED.
So the real root is the DATA-DELIVERY layer, not the worker (prompt `TEXSTREAM_DELIVERY_RE_PROMPT.md`).
**DELIVERY RE — STEP 1 done (partial, `ghidra_out/texstream_delivery_re.txt`; rate-limit cut it off):**
the reader-resource (vtable `0x82002A04`) is a DELEGATING WRAPPER — its own ready(+0xC) + advance/read
(+0x10) are ICF `return 1` stubs that do NO read; get-data(+0x14)/get-size(+0x18) delegate to an INNER
reader at this+0xC (created by `TexStream_ReaderFactory 0x82B53D68`). So delivery must be in that inner
reader; if it's lazy/unpopulated → consume reads uncommitted buffer = crash + black. **NEXT (STEP 2,
resume post-limit): RE the inner reader (this+0xC) — its data-read/populate + what commits its buffer.**
⚠ Do NOT re-recommend the worker pump (it crashes until delivery is fixed).

## ▶▶ 2026-07-19 (~19:50) — 3 DEEP RE RESULTS LANDED (through heavy server throttling; incremental writes saved them)
Server-side throttling ("temporarily limiting requests / not your usage limit" — NOT the usage limit;
status.claude.com all-green) repeatedly killed agents mid-run, but incremental-write + small-batch-labels
preserved everything. Three flagship deliverables essentially SOLVED:

### ★★★ BLACK-SKIN / TEXTURE POP-IN — DELIVERY BUG PINPOINTED (`ghidra_out/texstream_delivery_re.txt`, STEPS 1-3a)
The exact broken step is found. The texture async-read protocol: the reader-resource (vtable
`0x82002A04`, provider+0x64) is a DELEGATING WRAPPER over an INNER archive sub-reader (at wrapper+0xC,
from `TexStream_ReaderFactory 0x82B53D68`). **The INNER reader's `ready()` @0x82B56860 IS the delivery
pump** — polling it performs the fill (`Function_82B56B60`) the moment the async segments complete;
`advance()` @0x82B56968 is the blocking wait-until-delivered. **★ THE DISCONNECT: the completion worker
+ Finalize call READY/ADVANCE through the WRAPPER's vtbl+0xC/+0x10, which are ICF-folded `return 1`
NO-OP stubs that DON'T forward to the inner reader.** So if a sub-reader is created while its bank
segments are still in flight (pendingChild!=0), nothing ever polls the inner `ready()` → the fill never
runs → get-data (0x82174038, plain getter, no fill trigger) returns an unbacked/stale dest ptr →
consume copies uncommitted memory (= the pump-test AV) OR the provider parks in 5FA4 forever (= BLACK
SKIN). Works at LOAD only because segments are already delivered (eager). **★★★ COMPLETE (STEPS 2-4, agent finished
HIGH conf, +23 → labels_texstream_completion.tsv=42; `ghidra_out/texstream_delivery_re.txt` 311 lines).**
Full 3-layer reader stack, all decompiled: wrapper (0x82002A04, provider+0x64) → range sub-reader
(ctor 0x82B56650, vtbl 0x82007728, at wrapper+0xC) → bank_segmented_reader (0x82B4A7C8) + segment-list
child (0x82B55610) → **synchronous `file_read_bytes_sync 0x82B44888` (ReadFile — NO overlapped I/O
anywhere).** Delivery = POLLING: inner `ready() 0x82B56860` IS the pump (each poll checks segment-list
ready-all, runs fill 0x82B56B60 = memcpy parent→dest window, state=1); `get-data 0x82174038` never
fills; wrapper ready/advance are ICF `return 1` stubs that DON'T forward → nothing in the texture path
ever calls the real ready/advance. **MECHANISM CLASS = lazy-read-never-pumped** (NOT IO-thread: bottom
is sync ReadFile; NOT phys/virt: fault 0x9d62a698 is v90000000, no alias, nothing wrote — RESERVE =
commit-on-delivery that never ran). **Why mesh works but texture doesn't: the mesh path polls the RAW
reader vtbl+0xC per frame + force-completes; the texture path is the ONLY one with the stub wrapper in
between.** ★★ SAFE FIX (spelled out in the deliverable, for the runtime owner): a delivery-aware host
pump in `Fable2Recomp/src/` — per frame, for each 5FA4 provider, vtable-dispatch the INNER reader's
ready (`*(wrapper+0xC)`→vtbl+0xC, OUTSIDE CS 0x834A5F7C); ONLY when it returns 1, invoke guest worker
`0x82B6D348` (stub gate then harmless — data genuinely delivered, promoted via the game's own consume).
Watchdog fallback = inner blocking advance `0x82B56968` (mesh force-complete pattern; can't deadlock —
sync bottom). Env-gate `FABLE2_TEXSTREAM_PUMP=2` + 5FA4/5FBC logging. ⚠ NOT the naive worker pump
(proven crash), no state forging. Fixes black skin AND texture pop-in. Project repairs: cleared wrong
noReturn 0x82214DD8, re-laid 82B4E800–82B50800 (35 fns, recovered `file_load_whole_into_buffer
0x82B4F5D8`). Confidence HIGH on protocol/disconnect, MED-HIGH on the RESERVE explanation
(runtime-verifiable via VirtualQuery around a forced fill).

### ★★ TERRAIN aux-float SOLVED (`ghidra_out/terrain_loader_re.txt`, +16 `labels_terrain.tsv`)
**In-memory GHF cell (0xC stride) = { float0 = TERRAIN height, float1 = WATER height, float2 = TBD }.**
Aux float1 = per-cell water height, decomp-PROVEN: `heightfield_sample_water_height @0x821EEC60` (water
twin of the height sampler) + `heightfield_cell_bilerp_WATER @0x8223CC90` (reads cell+4) →
`lua_native_IsOverWaterAtPosition @0x821A3608` returns `sentinel(DAT_82099484) < waterH`. So water is
NOT a separate grid — it's packed into the height grid's aux float. **★ COMPLETE (agent finished, HIGH conf, +16 `labels_terrain.tsv`=19):**
aux "float2" (cell+8) is NOT a float — a PACKED WORD: bits[31:2] = index into a per-chunk interned table
of GROUND-MATERIAL GDB GUIDs (footstep-SFX/dust-FX surface system: `heightfield_get_cell_material_entry
@0x823563E0` → `terrain_get_ground_material_at_entity @0x82712788` reads GDB field "Material" @0x820D1D60,
beside "FX_Dust_Foot"/"SE_FOOTSTEP_WALK"); bits[1:0] = 2 per-cell flags (likely Havok collision — next
hop collision-shape vtbl @0x820CA520). **14→12 FULLY DECODED:** on-disk 14B cell = {+0 f32 terrainH, +4
f32 waterH, +8 u32 materialGUID, +0xC u8 flagA, +0xD u8 flagB} → in-mem 12B = {h, water, (internIdx<<2)|
A<<1|B}. Loader chain: `ghf_cellgrid_load_convert @0x8265CD18` ← `heightfield_open_ghf_and_load
@0x8250ECE0` (name+".ghf", 64KB decompressing reader, ONE .ghf = ONE chunk) → `heightfield_load_chunk_
record @0x82354780` (origin f32[3] + wCells/hCells, 2 cells per tile) → `heightfield_add_chunk 0x82355B58`
→ cellgrid ctor 0x8265CC00 + intern 0x8265CE20; zlib: inflate @0x82B59220 / uncompress `0x82B4F0B8` /
range-read `0x82C76150`. Consts: sentinel DAT_82099484=0.0, bilerp split DAT_82099490=1.0, default
material @0x83497DC8, 2-cells-per-tile hardcoded. **TERRAIN height+water+material round-trip is
format-complete for a WRITER.** Also FIXED 3 mis-flagged noReturn save-helpers (82cadcd4/82cadcd8/
830069d4) that poisoned the VMX terrain/collision cluster. GAPS: flagA/B semantics (collision), render
mesh-builder/LOD path (no LOD-distance const found — render path undisassembled).

### ★★ GDB INSTANTIATION (Q1) SOLVED (`ghidra_out/gdb_instantiation_re.txt`) — the ADD-NEW-COMPONENT path
Full record→live-entity chain decomp-verified. `item_instantiate_worker @0x824F2870` → `entity_create_
from_record @0x8238B860` (hk_malloc 0x94, entity_ctor 0x8238B320 vtbl 0x820A9B38, entity_manager_add
0x82363BA0) → **★ `entity_build_components_from_record @0x8238DCC8`** (the Q1 core): (1) collect
component hashes recursively up the kHashParent chain via `gdb_record_collect_component_hashes
0x8238DB28` (component identity = subrecord field-name hash; FNV("RemoveComponent") subtracts inherited
ones); (2) per hash → **`entity_create_component_by_hash @0x8238CDC8`** = THE COMPONENT FACTORY: looks up
a **component registry** (`component_registry_lookup 0x82630218`, binary search, 0x18-byte entries
{nameHash, createFn, …, typeId}), calls `desc->createFn(entity)`, sorted-inserts {typeId,component} into
entity+0x48/+0x4C; (3) per component, virtual **`InitFromGdbRecord` (vtbl+0x20)** reads its GDB subrecord
fields into the live object (`gdb_record_get_component_subrecord 0x82A00520`). Component vtable layout
recovered (+0x20 InitFromGdbRecord, +0x24 LoadFromStream, +0x2C OnPostCreate, +0x3C OnReplaced).
**★★ Q1 FULLY CLOSED (agent finished, +36 `labels_gdb.tsv`, spec `gdb_instantiation_re.txt` 217 lines
+ `gdb_component_registry.txt` 261-class table).** The factory registry (`component_registry_lookup
0x82630218`, sorted 0x18-stride {nameHash,createFn,destroyFn,?,?,typeId} @registry+8;
registry=`*(*(*(*(DAT_83496AB8+0xC)+0x58)+4)+4)`, created in `entity_system_ctor 0x8234EF58`) is
POPULATED by **`component_registry_register_all @0x826399A8` → 261 per-class `Register_CEC*` fns**.
**★ Registered name = class name MINUS the "CEC" prefix** (CECPhysicsSimple ↔ GDB field "PhysicsSimple").
**MODDING ANSWER: add a new component type = insert one 0x18 registry entry ({FNV1(name), host createFn
thunk, destroyFn, typeId}) after entity_system_ctor, OR weak-override the register-all hub — the build
worker needs NO patch** (enumerates field hashes generically → a new type-6 GDB field instantiates
automatically). New ARCHETYPES from existing components = pure data (GdbEdit), no native hook.
⚠ typeId HARD CAP 0xFD (entity+0x24 bitset, ids used to 253 — reuse gaps or extend the bitset).
Root unlock: 4 mis-flagged no-return leaf helpers (lh_string_assign_cstr 0x8222CED0,
lua_get_userdata_ptr 0x822280F8, intrusive_ptr_assign 0x822651A0, intrusive_handle_assign_counted
0x821F0240) were truncating the chain — FixNoReturn on them also un-truncated many unrelated decompiles
project-wide. **ALL 3 BACKGROUND TASKS NOW COMPLETE (texture-delivery, terrain, GDB).**

**Earlier this round (pre-throttle):** save-editor Task 2 COMPLETE (40 labels, `herosave_editor_spec.txt`,
field-tag catalog, string-editable); NPC draw-distance knob → SHIPPED as a user-confirmed in-game fix
(src/DrawDistanceFix.cpp, [[fable2-npc-popin-drawdistance-fix]]).

## RESUME NEXT SESSION (all checkpointed; 3 agents may still be finishing at context-clear):
1. **Texture-delivery STEP 3b** — the bottom archive segment-read I/O completion (finish the black-skin/
   pop-in root); then the runtime owner implements the inner-reader-poll fix.
2. **Terrain** — float2 semantics + the .ghf loader (14→12 convert) — repair the VMX/noReturn cluster.
3. **GDB** — flush labels_gdb.tsv + STEP 5 (component-registry populate / RegisterComponentType) → the
   add-new-component modding path. Also standing: FX emitter Update slot + quest gate dispatcher (deferred).

## ▶▶ 2026-07-19 (background) — BLENDER INTEGRATION ARCHITECTURE SET (source-level) + 4 DECISIONS PENDING
Plan of record `docs/BLENDER_INTEGRATION.md`; memory [[fable2-blender-integration]]. Existing addon
`FableLevelImporter.py` = import-only/level-only/FBX-intermediate/no-anim/no-round-trip. **★ KEY
FINDING: Blender's own glTF addon ships a native C++ lib (`intern/draco_bridge`, built SHARED, flat
`extern "C"` API) loaded from Python via `ctypes` — the exact pattern to reuse the AssetBrowser C++
decoders (`libf2`) in Blender with NO fork, NO upstream tracking.** RECOMMENDATION: one addon, two
backends (libf2 .dll + ctypes, pure-Python fallback → same codebase). Blender source shallow-cloned at
`third_party/blender` (406MB). SHIPPED (new files, ⚠ un-run, need Blender testing): `docs/BLENDER_
INTEGRATION.md`, `.../addons/fable_mdl_format.py` (pure-Python MDL parser, verified vs synthetic MDL),
`fable_libf2_bridge.py` (ctypes A↔B seam), `FableMdlImporter.py` (bpy mesh+armature+skin importer).
**★ 4 USER DECISIONS PENDING:** (1) libf2 .dll+ctypes (rec) vs pure-Python vs fork; (2) who builds
libf2 + platforms; (3) Phase-3 anim via existing FBX export vs native f2_anim_*; (4) Phase-4 export =
level edit-sets only? See the memory. **★ ANIM importer now SHIPPED too** (`fable_anim_format.py` + `FableAnimImporter.py`):
full AnimBank codec ported pure-Python + verified (selftest + 2 synthetic decodes); container =
`.animation_toc` ("AnimBank") + `.animation_data` (0xCEA5EBED), 4-mode per-8-frame-block codec. Retarget
= by NORMALISED BONE NAME (per AnimRigMap.h), not FNV-1 hash. Both MDL+Anim importers ⚠ need real-data +
in-Blender testing. **PACKAGED (done): one installable add-on `Fable2AssetBrowser/source/addons/fable2_io/`**
(umbrella `__init__.py` reuses all 3 importers; `blender_manifest.toml` schema 1.0.0 for Blender-5.x
Extensions + `bl_info` for classic install on 3.6–4.1) + `run_selftests.py` (**passes 3/3 green**, no bpy)
+ `README.md` (install routes + manual test plan). User validation cmd: `cd
Fable2AssetBrowser/source/addons && python run_selftests.py` (expect ALL GREEN), then Blender ▸ Prefs ▸
Add-ons ▸ Install from Disk ▸ `fable2_io`. **★ DECISION 1 MADE: libf2 .dll + ctypes native path (no
fork).** Blender track now PARKED at its decision gate — next step = author the libf2 shared-lib + C API
(a C++/AssetBrowser-build task; pending decision #2 who-builds+platforms; the pure-Python importers
already work so the DLL is an optimization, not a blocker). Real-data + in-Blender testing still needs
the user.

## ▶▶ 2026-07-19 (background decomp) — TERRAIN mapped + ★ the "UNDISASSEMBLED .text" WALL is the #1 blocker
**TERRAIN (+3, `labels_terrain.tsv`; spec `ghidra_out/terrain_spec.txt`):** two per-level files —
`.ehf` "HeightFieldGraphicsFile" (63B header; body = tex blobs → patch grid "850A0" → weight/paint
masks → LODs{6 strings+flags+params} → chunks: chunk_w×chunk_h grid, each chunk **32×32 cells**, ≤16
layers) + `.ghf` gzip height grid (14B/cell = f32 height + **10 UNDECODED aux bytes = main gap**).
Collision = Havok `hkpSampledHeightFieldShape` sampling GHF directly. `GetLandscapeHeightAt @0x8245EBE8`
→ `heightfield_sample_height @0x821EEE48`; `cinit_set_ghf_extension @0x832548F0`. Chunk res 32 baked in
(higher-res terrain needs re-mesher + re-authored grids). Cross-checked vs `.fable` manifest +
AssetBrowser. Feeds the Blender terrain round-trip.
**★★ CROSS-CUTTING: the recurring RE wall = big regions of `.text` were left UNDISASSEMBLED by Ghidra's
auto-analysis** (`halt_baddata`; format magic strings show 0 refs because the compare lives in un-swept
code). This SAME wall blocks: the terrain `.ehf/.ghf` parser, the spell AoE spawn worker
(0x8253Fxxx–0x82540xxx), and the engine-level parser. A targeted force-disassembly sweep of those
regions (PPC = fixed 4-byte-aligned, so low-risk) would unblock MULTIPLE subsystems at once — high
leverage. (Dispatched next.)
**★★ SWEEP RESULT — ROOT CAUSE = Xenon VMX128, NOT un-swept .text (`ghidra_out/forcedisasm_session.txt`,
+1 label `spell_command_enqueue @0x82545940`).** The stuck loaders hit **VMX128 vector instructions
Ghidra's stock PPC-AltiVec disassembler can't decode** (opcode-4/63 VMX128 forms, e.g. `100049C3` /
`FD60001E`); Ghidra stops → decompiler `halt_baddata`. Force-disassembly CANNOT fix it (real code,
undecodable by the current processor spec). Confirmed: spell cast `CreateScriptedSpellShotLongFireball
@0x825409C0` stops at 0x82540A64; terrain `heightfield_sample_height @0x821EEE48` stops at 0x821EEE60;
the "engine-level" region 0x82AB9700–0x82ABB400 was DATA (0 prologues), never blocked. Also: spell AoE
gap (a) is behind a vtable `bctrl` (`spell_command_enqueue @0x82545940` → virtual spawn) — needs a
vtable trace, not just VMX128. **★ HIGHEST-LEVERAGE NEXT STEP = add VMX128 to Ghidra's PowerPC SLEIGH
spec** (the extension XenonRecomp/XEXLoaderWV use) → unblocks terrain sampler + spell math + FX parsers
+ every vector-heavy loader AT ONCE. New reusable tools in `tools/ghidra_label/`: ForceDisasm,
ForceDisasm2, ClearAndDisasm, RepairRegion. (VMX128 enablement dispatched next.)
**★★★ VMX128 FIX STAGED & VERIFIED — READY TO APPLY (`ghidra_out/vmx128_session.txt` + staging).**
Solution = the [0dinD/ghidra `vmx128` branch](https://github.com/0dinD/ghidra/tree/vmx128) `vmx128.sinc`
(~89 forms: lvx128/stvx128/vperm128/vor128/vaddfp128/vmaddfp128/vmsum*fp128/vsldoi128…) wired into the
project's EXISTING language `PowerPC:BE:64:A2ALT-32addr` (slafile `ppc_64_isa_altivec_be.sla`).
**ADDITIVE: language ID+version stay 1.7 → NO re-import, existing disasm + labels preserved.** Compiled
offline with the install's sleigh.bat = 0 errors, +7.7KB SLA. Verified it covers the exact halt opcodes
(terrain 0x102059C3, spell 0x100049C3/0x11A0D8C3/…, 0xFD60001E). Backups in `ghidra_out/vmx128_backup/`,
staged apply artifacts + exact 4-file copy steps + rollback in `ghidra_out/vmx128_staging/`.
**★★★ APPLIED + VERIFIED WORKING 2026-07-19 (user-authorized).** Copied the 4 staged files into the
install (ppc.ldefs untouched, version 1.7 → no re-import, all 188 labels intact; extra pre-apply backup
in `ghidra_out/vmx128_preapply_backup/`). VERIFY PASSED: ClearAndDisasm re-created both targets at FULL
length (`heightfield_sample_height` 0x1e4, `CreateScriptedSpellShotLongFireball` 0x17c); the old halt
words now decode — 0x821EEE60/6C `stvx128`, 0x82540A64/68 `stvx128` — and BOTH decompile cleanly (no
halt_baddata). **The VMX128 wall is broken project-wide.** Harvest plan = `ghidra_out/post_vmx128_worklist.txt`
(22 blocked targets: 13 round-1 pure re-decompile, 9 needs-more; top-5 = terrain heightfield sampler,
spell cast cluster, `CreateParticleAtPosWithDirection @0x823BBE68`, FX sec1/2/4/5 parsers, quest tick
`@0x82482EB0`). ⚠ Existing VMX-hit functions still show old halt disasm until re-laid — harvest does
`[RD @addr]` = ClearAndDisasm+DecompFuncs per target. Rollback = file-swap from `vmx128_backup/`.
**★ HARVEST ROUND 1a (terrain+spell, +5 `labels_harvest1.tsv`; `ghidra_out/harvest_terrain_spells.txt`)
— VMX128 confirmed on all 6 fns.** TERRAIN: `heightfield_sample_height @0x821EEE48` (now CONFIRMED) +
`heightfield_cell_bilerp @0x82232880`. **GHF in-memory cell = 12B = 3 floats** `(y*width+x)*0xC`:
float0=height, float1+float2=aux (on-disk 14B→in-mem 12B; the 10 aux bytes → 2 floats) → **terrain
height round-trip unblocked**; terrainObj layout (chunk-ptr @+0x08/0x0C, extent @+0x98/0xA0/0x9C/0xA4),
chunk struct (cellGrid@+0, ranges @+8/0x10/0xc/0x14). SPELL cast chain mapped: `spell_shot_spawn
@0x825408C8`, `spell_spawn_dispatch @0x8253F2C8`, `spell_find_active_shot @0x82174328`,
`ForcePushOnDeath @0x82540B40`; shot struct = 0x28B per-shot records @+0x40 (FORCE_PUSH radius/mag floats
+0x64/0x6C/0x70/0x7C); ESpellType 1=fireball/2=SLOW_TIME/3=SWORDS/6=FORCE_PUSH. **★ Spell AoE is NOT at
cast time** — it's in the spell-shot entity's per-frame UPDATE (vtable, world-tick-dispatched); next
hop = shot vtable → Update slot → the entity-in-radius distance loop = the composer AoE verb.
Remaining: terrain .ghf loader/mesh-builder (aux-float semantics: normal vs blend) via terrainObj vtable.
**★★ HARVEST 1c (spell AoE, +5 `labels_harvest1.tsv`=10): AoE apply chain TRACED.** Each ESpellType =
own shot class (`spell_shot_create @0x824B1828`; FORCE_PUSH vtbl @0x820E2BA8); AoE in shot UPDATE
`forcepush_shot_update @0x828B3EE8` → target list (@shot+0x19C) → ★`forcepush_apply_to_target @0x828B4650`
(THE composer primitive: magnitude→1 entity, WILL-scaled via `@0x828B4D28`, Combat comp 0x1F). **gap (a):
apply-to-SPECIFIC-entity = exposable NOW; apply-in-RADIUS = 1 hop (proximity scan FUN_828E53F0 / add-target
`@0x828B3918` filling shot+0x19C).** Terrain aux-float semantics still need the .ghf mesh-builder trace
(sampler doesn't reach it; via terrainObj vtable / EngineLevel_ReadType5).
**★ HARVEST ROUND 1b (FX+quest, +11 `labels_harvest2.tsv`; `ghidra_out/harvest_fx_quest.txt`).**
★ PARTICLE-BANK WRITER UNBLOCKED: sec1 `@0x83233928` = a 0xBAADF00D version header (NOT a texture
table — tex paths are in sec2 visual nodes); sec4 `@0x8322BFD8` emitter record = 3 BE u32s
{type_id,entity_id,sub_type} (`ParticleBank_ReadEmitterHeader @0x83225BA0`), no special padding
(0x1C917E is just its offset). All sections = `[ScopeMarker][BE count][per-item: type-id→factory→vtbl-
deserialize]` → writer = invert AssetBrowser Reader. `CreateParticleAtPosWithDirection @0x823BBE68`
sig = (ctx, pos vec3, dir vec3, fxName), pos/dir from ARGS not entity-deref → viable as
`Debug.SpawnFXAt(pos,dir,name)` w/ null ctx (1 vtable hop to confirm ctx==0). QUEST: `script_update_dual_lists
@0x82482EB0` (sub-tick, resumes 2 Lua mgr coroutine lists via the invoker bridge — the CK-tool hook
choke point; the top +0x182/183/184 gate dispatcher still unpinned). QUEST-FLAG store CORRECTED: not a
static array — `quest_progress_enumerate @0x82443048` (XContentCreateEnumerator, cap 0x96=150 over the
save's quest section) + `quest_name_to_index @0x82440798` (corrects the strncmp mis-ID) → a
quest-progress tool reads/writes that 150-record save section.
**★ HARVEST 1b-cont (FX+quest vtable traces, +8 `labels_harvest2.tsv`=16): gap (c) CLOSED +
composer-verb trio mapped.** `CreateParticleAtPosWithDirection @0x823BBE68` = null-ctx SAFE (cmpwi
r31,0;beq skips all entity derefs) → `Debug.SpawnFXAt(pos,dir,name)` call it with ctx=0 (gap (c) DONE).
So ALL 3 freeform-composer verbs mapped: (a) AoE `forcepush_apply_to_target @0x828B4650` (+radius-gather
hop), (b) `Health.Modify @0x824CBC68` wrap, (c) SpawnFXAt @0x823BBE68. FX emitter obj 0x100B (vtbl
@0x82004B48, `Emitter_Deserialize @0x82A11AC8` reads timelines+max_active; factory `@0x83227100`); Update
tick = 1 vtable-slot hop. QuestManager pair pinned (`QuestManager_InvokeOverList @0x8245D3C0`); top
+0x182/183/184 gate dispatcher = 1 RD hop (near 0x8245D048). Quest-flag r/w fully spec'd. Remaining FX/
quest hops (emitter Update slot, gate dispatcher) deferred — agent redirected to BLACK-SKIN completion RE.

## ▶▶ 2026-07-19 (background decomp) — QUEST / AI-BRAIN-SCRIPT runtime mapped (+14, `labels_quest.tsv`)
Spec `ghidra_out/quest_ai_spec.txt`; ties to [[fable2-quest-interactable-system]]. Native runtime is
THIN: 3 Lua script managers (General/AI/Quest), each a Lua obj + bound Update on the game-script-systems
object (+0x84 General/+0x98 update; +0x178 AI/+0xA4 update; gates +0x182 AI/+0x183 quest/+0x184 general
= the 3 SetUpdate* natives). Quests/AI-brains/interactables = Lua coroutine threads the managers resume.
Core bridge `script_invoke_lua_method @0x8219A758` (rawgeti {methodRef,self} → lua_cpcall) +
`script_resolve_lua_method @0x824633A0` + `script_general_AddScript @0x82460CF8`. Quest progress = the
150-flag (0x96) bitfield (`Save_BuildProgressMeta @0x82444DA8`); quest-script-file registry
`AddQuestScriptFile @0x8245EC68`. **AI brains bind via GDB property "BehaviorScripts"**
(`cinit_hash_BehaviorScripts @0x83294FA8` → DAT_834BE618) — re-confirms AI=scripted, not Havok.
Interactables = `OnActionUse.*` via ActionUseScript (`OnActionUse_SetCanDisplayWorldIcons @0x82823090`).
Gaps (blocked on VMX128/undisasm sweep): per-frame tick dispatcher (cand 0x82482EB0), SetQuestManager +
QuestManager Update slot, the quest-flag get/set accessor (needed for a quest-progress tool).

## ▶▶ 2026-07-19 (background decomp) — FX / PARTICLES runtime mapped (+15, `labels_fx.tsv`)
Spec `ghidra_out/fx_particle_spec.txt`; detail in memory [[fable2-modding-systems-analysis]] (EFFECTS
section). Loader `ParticleBank_Load @0x832361B8` (FNV-1-keyed, 5-section layout, FX global @0x8349E640);
spawn/attach/lifetime via `ParticleAttacher.AttachParticles @0x82765FF0` → GDB type-4 attachment
subrecords. `max_active` data-driven per-emitter (no global cap). **Particle-bank WRITER is invertible
from the AssetBrowser Reader** — 2 unknowns (sec1 texture-table layout, sec4 padding). Sec1/2/4/5
parsers + `CreateParticleAtPosWithDirection @0x823BBE68` UNDISASSEMBLED → pending the force-disasm sweep.

## ▶▶ 2026-07-19 (background decomp) — SAVE/LOAD mapped + ★ "MORE SAVES" = a UI change, NOT a format limit
**(+23, `labels_save.tsv`; spec `ghidra_out/save_format_spec.txt`.)** A save is an Xbox 360
**XCONTENT/STFS package** (`XContentCreate`); `herosave.bin`/`texturemorphs.bin` live INSIDE it.
Content-type **0x5841091D** = a Fable 2 save on disk; **0x4D5307F1** = metadata magic. Card-fan
metadata (no need to open herosave.bin): hero name @`XCONTENT_DATA+0x108`, 128-char chapter desc,
timestamp, 150-quest-flag progress bitfield. Serialize+load = one bidirectional `CSaveArchive`
(`Save_Orchestrate @0x82445398` → `SaveArchive_WriteContent @0x82449930`; `LoadHeroSave @0x82448018`
mode 3). **★ SLOT-CAP ANSWER (user goal "more save files / scrollable list"): NO fixed max-saves
constant anywhere in storage/format/enumeration** — `SaveList_FilterByType @0x82EF0150` builds the list
from a DYNAMIC vector (filter content-type 0x5841091D). **The cap is purely the card-fan front-end
widget** (consumes the filtered array @obj+0x16C) → more saves + a scrollable list = a GUI change, not
a format limit. Gaps: herosave.bin internal record layout = pure-virtual `Serialize()` overrides
(vtable @0x820B4AFC) not yet enumerated (needed for a save EDITOR); compression not confirmed (no zlib
in the archive path — likely raw in STFS).
**★ HEROSAVE.BIN INTERNAL FORMAT mapped (+9 `labels_save.tsv`=32; `ghidra_out/herosave_layout_spec.txt`)
— it's a NAMED-FIELD PROPERTY-TREE, not a flat struct.** Fields = {name,value} pairs in a
`SerializeBuffer` key/value tree (`SerializeBuffer_ctor @0x82C8DD08`, vtbl @0x8200E9C8): `_OpenNode
@0x82C8D1B0` (named group), `_WriteU32 @0x82C8D7B0` (int: health/will/gold/renown/morality/skillXP),
`_WriteFloat @0x82C8D678`, **`_WriteHexU32 @0x82C8D548` (u32→16-char hex TEXT — inventory/equip GDB
record IDs)**, `_WriteValue @0x82C8D868`. Flow: `HeroSave_WriteFile @0x824464B8` → SerializeBuffer_ctor
→ `HeroSave_WriteRootTag @0x82440F78` ("HeroSave") → serialize hero component graph →
`SerializeBuffer_FlushToFile @0x82A1CD30` (size-prefix + raw tree). **COMPRESSION = NONE (raw in STFS,
confirmed).** Save blocks = hero's entity COMPONENTS 1:1 (Health/Inventory/Stats/Morality/Money/Gameflow).
Editor = find-by-name → set value. Remaining (mechanical): (1) per-component field TAGS (decomp each
component's Serialize override for the key strings); (2) on-disk tree encoding (binary TLV vs text
key=value — decomp the inner node writer SerializeBuffer+8 vtbl +0x44/0x54/0x58 + blob builder).
**★ ENCODING VERDICT = TEXT KEY/VALUE TREE (salvaged +6 `labels_save.tsv`=36; the finish run's writeup
was lost to a rate-limit cutoff but the labels landed):** `SerializeNode_ctor @0x82C8F528` = node 0x2C
bytes {+2 name lh_string, +3 value lh_string} → herosave.bin is a TEXT key/value tree.
`Serialize_format_float @0x82C8D0C0` writes floats as TEXT "%f %X" (decimal + lossless hex bit-pattern);
`SerializeBuffer_WriteHexU32` = IDs/GDB record IDs as hex text; `SerializeBuffer_alloc_chunk @0x82C8EEA0`
(4KB growable text buffer) + `SerializeBuffer_bump_alloc @0x82C88B20` (appends name/value text). **→ a
save editor can likely STRING-EDIT the save.** ★ TASK 2 DONE (spec `ghidra_out/herosave_editor_spec.txt`,
labels_save.tsv=40): field-tag catalog = Morality/Renown/Experience/Health/MaxHealth/Toughness/
Inventory+InventoryItem (items=hex GDB record IDs)/Appearance/Gameflow. **SAVE EDITOR = string-edit:
extract herosave.bin from STFS → find field by name → rewrite value token (floats: both %f + %X) →
repack.** Gaps: Gold/Money tag not found (differently-named or a currency item in Inventory — needs
Money-component Serialize decomp); exact sub-field types + strict node delimiters (mechanical hops). ⚠ 3 background agents were cut off mid-run by a session rate limit (reset 19:20):
NPC-streaming produced NOTHING (`npc_streaming_re.txt` + labels_npc.tsv empty — full re-run needed);
black-skin HARDENING pass incomplete (fix stands, confirmation gaps still open).

## ▶▶ 2026-07-19 (round 3, user-present then away) — BLACK SKIN: BLACK → VISIBLE-WITH-ARTIFACTS; RT-BRIDGE DEBUNKED; RAM MODEL COMPLETED
**Full detail: memory `fable2-black-skin-renderdoc` (round 3) + `fable2-ram-memory-model-map`.**

**★ BLACK SKIN — major progress, now VISIBLE with residual artifacts (was fully black).**
- **RT→texture-bridge premise DEBUNKED from the user's own logs:** 31k resolves + 4k readbacks/run,
  NONE touch the composite → it is a CPU async-fill pool texture, NOT a render target. All RT-bridge
  work retired. (The command_processor.cpp:3016 hardcoded 0x1B1xx morph-skip is now a dead
  band-aid — the composite page isn't even stable across runs; retire it.)
- **Stage-1 fix SHIPPED (user-confirmed black→visible): pool boost widened to the DXT1 buckets.**
  `src/AtlasFillDiag.cpp` `sub_82B60910`: the composite pool buckets by format (f0=35 DXT1, f0=36 DXN);
  the SKIN is DXT1 but the original POOLBOOST only widened the DXN 1024² bucket. Now boosts DXT1+DXN
  512²/1024² (row4 28→64, rows2/3→80). `bucket-alloc FAIL=0`.
- **Full fill path LIVE-mapped** (`src/MorphFillTrace.cpp`, FABLE2_FILLTRACE=1): ProcessOnePending
  0x82181828 → StreamLayerSources 0x82A5C690 → (sources ready?) → TouchTarget 0x82A5C250 →
  TouchAndGetD3DTexture 0x8221EAF0 → StreamMips 0x821D7A98 (create into +0x48) + FinalizeStreamedResource
  0x82A7B148 (promote +0x48→+0x44, set residState +0x54; NO-OPs if +0x4c==0). TouchTarget composites
  become resident fine; the ARTIFACTS = a batch of mips=1 DXT1 textures (`4216Axxx`) that never
  become resident.
- **⚠ MIP-FIX CRASHED THE GAME (log 529, AV at boot) — DISABLED.** Writing an inflated mip count to
  +0xA0 corrupts the texture pipeline; also the create-math re-trace shows mips=1 makes a VALID
  create, so it was the wrong hypothesis anyway. `FABLE2_MORPHMIPS` now DEFAULT-OFF (opt-in). Game
  re-verified stable past 35s. LESSON: don't write game texture-state fields blind.
- **NEXT (pure diagnostic, needs a user PLAY run — no fix): FABLE2_FILLTRACE=1** (mip-fix off), reach
  the artifact scene, read the improved STREAM lines' new `pendRes(+0x48)`/`+0x4c` fields: +0x48
  populates but residState stays 7FFFFFFF ⇒ Finalize SKIPPED (hypothesis #2: sources streamed with
  flags&3==0 → TouchAndGetD3DTexture skips Finalize — the STRONGER lead); +0x48 stays 0 ⇒ StreamMips
  create genuinely failed (#1). Then design a SAFE fix. A game instance is already running with this
  build (mip-fix off, FILLTRACE on) ready to play.

**★ RAM/MEMORY MODEL COMPLETED (autonomous, safe RE — advances the ownership north star):**
- xmemory.cpp: ~2GB VIRTUAL (v00 1GB 4KB + v40 ~1GB 64KB) + 512MB PHYSICAL (aliased A0/C0/E0). The
  512MB wall is on PHYSICAL memory only (hard-capped by the 360 4GB address map).
- `HeapPageAlloc 0x83237618` decompiled: **NO software growth cap** — heaps grow via VirtualAlloc
  until region exhaustion. The `0x20000000` flag (set for 64KB-page heaps) routes to PHYSICAL; 4KB
  heaps go VIRTUAL. Facade resolved (0x82BF11AC malloc, 0x82BF11E8 realloc; 0x8305A720 = thunk).
  +4 labels in `ghidra_out/labels_mem_budget.tsv` (applied).
- **★ BLACK-SKIN ↔ RAM: the texture pool lives in PHYSICAL memory (bucket memoryBase 0xEFA06000/…
  = the 0xE0 alias).** So texture starvation IS the physical 512MB wall; the real texture RAM lift =
  decomp TexturePool_Init's allocator and reroute it off the 0xE0 physical alias to a PC-native
  virtual heap. General-data lift (virtual, ~2GB) is much easier than texture/GPU (physical, hard).

## ▶▶ 2026-07-19 (background decomp) — CORE ALLOCATOR + BUDGET CONSTANTS DECOMPILED (RAM-lift knobs)
Autonomous Track-B decomp (ran alongside GPU/menu work, Ghidra-only, no code/build/run touched).
**The game's core general allocator is now mapped** — a Lionhead segregated size-class allocator on
Xbox `VirtualAlloc` behind `hk_malloc @0x8221F258` (~700 callers) = the primary guest-RAM owner.
25 fns named in `ghidra_out/labels_mem_budget.tsv` (applied; own file, NOT labels_in.tsv):
`game_malloc_core @0x83238598`, `game_free_dispatch @0x83238460`, `game_alloc_huge @0x83237178`, etc.
**★ Baked-in 360 budget constants (the RAM-lift knobs), from `.rdata` table @0x8209B3E0:** 10MB
huge-alloc threshold @0x8209B400 (≥ → raw VirtualAlloc; cleanest reroute point), 10KB/64B/16MB size
classes, 32MB..128MB per-heap reserves (initial segments, grow via HeapPageAlloc — NOT hard caps).
Real limits = the routing thresholds + the 512MB physical wall in xmemory.cpp. Full writeup:
`ghidra_out/decomp_membudget_session.txt`; memory [[fable2-ram-memory-model-map]] updated. Open
leads: malloc facade 0x82BF11AC/0x82BF11E8, OOM handler @0x8305A720, HeapPageAlloc growth ceiling.

**★ Same round — Track-C BNK/resource-reader stack decompiled (+16 labels, labels_in.tsv 1066→1083).**
`bank_directory_ctor @82C6CA08` (TOC parser, vtable 0x8200DEA0), `bank_block_load @82C6D288`
(shared sub-block loader, compression-type 1..3 dispatch), `bank_open_from_config @82C73768`,
resource-reader class (vtable 0x8200DF08: `resource_reader_ctor @82C6F810` etc.), stream prims
(`reader_read_u32 @82C8AAE0` BE-swap, `reader_read_lenprefixed_blob @82C6B430`), `ModelStreaming_
PerFramePump @82A54500`. ★ KEY: `bank_block_load` is shared with the babel text system
(`text_GetText_toWString`); `type==3` = a distinct compressed-slice layout → next lever for the
AssetBrowser BNK *writer* (can't repack compressed entries today). Deliberately left the CustomAtlas
cluster (82447EC8/82A5C778/82A5D510) unlabeled for the texture track. Writeup:
`ghidra_out/decomp_trackc_session_bnk_reader_streaming_20260719.txt`. Open: pin 823DCB10/8250EDD0
via globals 0x834978BC/0x83496B08; decode full BNK entry format; mesh loaders 82B8F1B8/82AB0A30.
**★ Same round — guest AUDIO ENGINE decompiled (+18, `labels_audio.tsv`; writeup
`decomp_audio_session.txt`; memory [[fable2-ingame-audio-silent]]).** Multi-threaded software mixer:
init `~0x82CE1F00` spawns 6 threads; `audio_render_driver_callback @0x82CEAD70` →
`audio_mix_and_submit @0x82CE1418` → bus mix → `AudioSubmitRenderFrame @0x82CDEB18` (sole
XAudioSubmitRenderDriverFrame caller); XMA via direct MMIO 0x7FEA1818 (`xma_context_poll_state
@0x82CDA9C0`, 0x60 stride). Knobs: thread/pending-frame count **6** (audioObj+0x130 @0x82CE20CC),
48kHz, 2 vol categories, no max-voice cap. NEXT audio target = XMA ctx-write/kick cluster
0x82CDA800–0x82CDA9C0 (the old silent-gameplay path). ✅ DONE next round (below).

**★ WILL/SPELLS runtime decomp + 3-GAP VERDICT (+4, `labels_spells.tsv`; spec
`ghidra_out/spells_spec.txt`; memory [[fable2-modding-systems-analysis]]).** Cast off the per-entity
component table; `entity_get_spell_manager_component @0x82173EB0` (CECSpellManager typeId 0x38). No
data-driven spell-def; params in spell grid `spellMgr+0x9C` + hero ability array. **Freeform-composer
verdict: (b) Health.Modify arbitrary-target FEASIBLE NOW** (`Health.Modify @0x824CBC68`, health comp
curHP+0x14/maxHP+0x1c → a `Debug.DamageEntity` wrapper); **(c) free-PFX-at-point LARGELY PRESENT**
(`CreateParticleAtPosWithDirection @0x823BBE68`); **(a) AoE-in-radius = the REAL gap** (entity-in-sphere
query buried in undisassembled spawn worker 0x8253Fxxx–0x82540xxx — force-disassemble = next RE
target). ⚠ old `CreateScriptedSpellShot @0x82540E38` anchor imprecise → real variants 0x825409C0/0x82540B40.

**★ NETWORKING / SESSION / co-op subsystem MAPPED (+30, `labels_net.tsv`; spec
`ghidra_out/net_session_spec.txt`; memory [[fable2-networking-session-map]]).** CXboxLiveManager
(XSession lifecycle, singleton `@0x822D3868`) + NLivePresence (friends/presence/co-op invites) over one
~0x200B session obj; MP-mode = `(*(DAT_83496AB8+0xC))+0x11C` (0 single/1 MP/2 local-coop). **XSession
API all dispatches via `XMsgStartIORequest(class 0xFB, MSGCODE)`** — every wrapper named
(`XSessionCreate_wrapper @0x82D00980`=0xB0010 … ArbitrationRegister=0xB001A). Co-op = drop-in
2-player (orb-menu → `Coop_FindHenchmanHeroInRange` → `NLivePresence_TeardownSession`); 2-slot cap is
game-side config, not kernel. Native-netcode gaps: XSessionJoinRemote/Search wrappers, XNet transport
(XNetConnect/QoS + packet framing), XNotifyGetNext pump.

**★ HAVOK structurally mapped + a KEY CORRECTION (+6, `labels_havok.tsv`; spec
`ghidra_out/havok_spec.txt`).** hkaSkeleton struct recovered (0x1C: name/+0x04 parentIndices
hkArray<int16>/+0x0C bones/+0x14 referencePose hkArray<hkQsTransform>; bone hash = FNV-1; byte-exact
vs AssetBrowser MDL skeleton) + 11 anim-class struct sizes. Anim codec = Fable's OWN 4-mode Bézier
(not Havok). Labels = the hkClass reflection registrars (`hkClass_register @0x82D55C40` + 5). **★★
CORRECTION to [[fable2-modding-systems-analysis]]: NO hkbBehaviorGraph in this build — Fable AI =
scripted "brains" (Lua/quest), NOT a Havok behavior toolkit. So "Havok behaviors = biggest gap" is
WRONG; behavior belongs to the AI/brain-script track.** Havok = ragdoll + anim playback. Real gaps:
(1) guest anim-sampler runtime (struct-ptr code, no string anchors → data-flow trace from
.animation_data load); (2) AnimBank ENCODER (new anims). Meta: all Havok class strings are
reflection-registration-only → future Havok RE must be struct/data-flow-driven.

**★★ GDB / ENTITY / ITEM-APPEARANCE system FULLY MAPPED (+21, labels_in.tsv → 1111; spec
`ghidra_out/gdb_entity_spec.txt`; memory [[fable2-modding-systems-analysis]]).** Global DB
`DAT_83496DA0` → DB obj (0x1C): record arena +0x10 (2MiB, cap 0x40000), strtab +0x0C, open-addr hash
`gdb_db_hash_lookup @0x821E4250`. Record = `{schemaPtr, value[field_count]}`; type = desc>>24
(0 bool/1 s32/2 u32/3 float/4 string/6 **ref**); **components = type-6 refs → sub-records**;
inheritance = kHashParent (0x5F6317D5) ref chain (`gdb_record_find_field_walk @0x821E5A08`). Loader:
`gdb_db_load_file @0x829FFD68` → `gdb_parse_stream` → `gdb_parse_bytes` → `gdb_db_build_hash_index
@0x82A00280`. Accessors (827E6xxx) → `gdb_record_get_field @0x82A006C8` (typed +
`gdb_field_type_compatible`); strings via `gdb_strtab_lookup @0x82A01110`. `InstantiateItemOfRecordID
@0x824F2730`/`AddItemOfType @0x824F0180` = thin arg-marshallers; record→entity walks type-6 comp refs.
Byte-exact agreement w/ AssetBrowser GdbReaderInternal.h/GdbEdit.cpp. **★ A DATA-level item/entity
editor is buildable NOW** (accessors + GdbEdit round-trip). Only gap = the true record→live-entity
INSTANTIATION worker (dispatched via reflection registrar member-fn-ptr, not static) — needed only to
ADD new component types, not to edit existing data.

**★★ XMA decode-trigger path FULLY REVERSED (+11, `labels_audio.tsv`=29; writeup
`decomp_xma_ctx_session.txt`; memory [[fable2-ingame-audio-silent]]).** `xma_context_pool_configure
@0x82CD9BD8` writes ctx+0x1C/+0x20 = MmGetPhysicalAddress(input_buf) (the NULL field from the old
bug) + valid bit. Kick: write 0x03000000 → MMIO 0x7FEA1804; status poll @0x7FEA1818; ctx index =
(phys-DAT_83337248)>>6. Buffer-consumed = POLL not interrupt (`xma_kick_and_wait @0x82CDA8F8`
spin-polls ~125ms). **Native-XMA reimpl spec: only need the emulated 0x7FEA1818 status register
semantically correct (running bit clears when host decoder consumes input); phys-addr already fixed.**

**★★ Same round — BNK ARCHIVE FORMAT FULLY DECODED (+5, labels_in.tsv → 1089; spec
`ghidra_out/bnk_format_spec.txt`).** TOC = zlib-compressed blob → `u32 file_count` + per-entry
records (BE): raw = name_len/name/rel_off/size; compressed adds decomp_size/comp_size/chunk_count/
chunk_decomp_size[]. Compressed entry = concatenated **zlib substreams, one per 0x8000-byte chunk**.
**Codec = zlib DEFLATE, definitive** (game inflate() @0x82B59310, err str @0x820FA858) — NO LZX/
Lionhead. The `type 1/2/3` field is a LOWER typed-block container (babel-shared), NOT the archive
codec. New labels: `bank_segmented_reader_ctor @0x82B4A7C8`, `reader_segment_list_pushback
@0x82B557C0`, `reader_read_bytes @0x82C6AA00`, `bnk_toc_vec_reserve_20b @0x82C6C468`,
`bnk_toc_vec_pushback_20b @0x82C6C940`. **★ ACTIONABLE (C++/AssetBrowser track, NOT done): the
compressed-entry WRITER is ~1 guard away** — `AddEntriesToBnkBytes` already deflate-compresses new
entries + self-verifies; only a self-imposed guard `"target entry is chunk-compressed; rewrite not
supported"` in the Rebuild path blocks compressed REPLACEMENT. Lift it + route through the existing
`deflate_slot_chunks` → compressed repacking works. Gaps: game's own 0x8000 chunk loop not yet
decompiled (past bank_directory_ctor); v2 archive writing unsupported (likely unneeded — moddable
banks are v1).

## ▶▶ 2026-07-19 (late night, round 2) — COMPOSITE NEVER RESOLVES; PRAGMATIC SKIP DEPLOYED
**Diagnostic finding (DEFINITIVE):** Ran with `FABLE2_RESOLVE_DIAG=1` to trace composite. Result:
**the morph composite (0x1B169000) NEVER appears in ANY resolve logs.** This proves:
1. The GPU render-to-texture IS happening (pixels exist GPU-side)
2. But the resolve never writes to the guest memory address the texture samples
3. Readback DOES pull the composite from raw EDRAM (that's why it shows when readback is on)
4. But the readback result is corrupt/glowing (wrong format or needs conversion)

**Pragmatic fix deployed:** Skip the morph composite (0x1B1xx000 range) from ALL readback in
command_processor.cpp. This isolates the problem: other objects readback cleanly, skin goes black
(better than corrupt). Unblocks the render-target→texture bridge as the ONLY viable long-term fix
(custom DXT1 shader to convert EDRAM RT format → compressed, or RT direct→guest copy).

**GPU plugin staged with morph-skip** (rexgpu-xenos.dll). Next = either (1) implement the bridge
(GPU work, ~dedicated session) or (2) move to savings list / graphics-GUI work while accepting
black skin as a known gap.

## ▶▶ 2026-07-19 (late night, autonomous task-loop) — MENU FIX CONFIRMED, DLC REMOVED, GRAPHICS-OPTIONS FOUNDATION, RUNTIME UNBLOCKED
**North star (user, verbatim intent): a fully-decompiled C++23 codebase — own EVERY system, retire
rexglue/emulators, remove 360 limitations (no Downloadable-Content menu item, more save files,
graphics settings IN the game GUI, etc.).** Working an active TaskCreate backlog. State:

- **★ MENU GRANTS/WARPS — FIXED & LOG-CONFIRMED (run 517):** the pause bridge ran on the WRONG VM
  (L=4F640220, not live gameplay) → grants no-op'd, warps crashed. NEW native channel:
  `Debug.Mod.NextCommand()` + host queue (ModApi.cpp); ConsoleInjector routes `ModBridge<code>` →
  `EnqueueCommand`; modmenu.lua `drive()` polls it → `__mm_dispatch(cmd)` in the CORRECT VM. Log:
  `queued code=2/6/7` → `[modlua] dispatch gold/halo_set/gear_set` (gameplay-VM channel). See
  [[fable2-pausemenu-folder-bridge]].
- **★ DLC TITLE-MENU ITEM REMOVED** (applied, verify next launch): captured `String1='Downloadable
  Content'` via new MMDBG_ALLSB SpawnButton logging; intercept SpawnButton to skip it (MM_HIDE_TITLE).
- **★ GRAPHICS-OPTIONS host channel BUILT** (in-GUI PC options foundation): GUI fires `MMGFX_<key>` →
  host `rex::cvar::SetFlagByName` (owns our cvars). NEXT = GUI 'Graphics' folder (build with testing);
  bloom/AA need GPU passes.
- **★ RUNTIME-BUILD UNBLOCKED:** rexruntime failed a full relink (`undefined __std_search_1` from
  xboxkrnl_io.cpp std::string::find) — fixed via `#define _USE_STD_VECTOR_ALGORITHMS 0`. All future
  audio/memory/kernel ownership work can build now.
- **RAM:** [[fable2-ram-memory-model-map]] — 512MB HARDWIRED (360 alias layout), real lift = decomp
  budgets (found 1st: 36MB texture-evict cap). FABLE2_MEMDIAG=1 headroom log added.
- **SKIN:** confirmed = morph COMPOSITE RTT (GPU render-to-texture), not resolution (identical at
  1x/2x). Composite renders GPU-side but never lands in the resident memory the texture samples
  (0x1B169 stays zero). Fix = own the composite (scoped readback of just that target OR bridge the
  render-target into the texture upload in rexgpu-xenos) — the hard flagship, needs build+test.
- **SAVES:** the save/load screen is a fanned HAND OF CARDS (graphically caps saves) → user wants a
  scrollable LIST with names (task 10). IsAbleToSaveGame is state-based, not the cap.
- **Test env:** `FABLE2_MODAPI=1 FABLE2_MODTEXT=1` (+FABLE2_MEMDIAG=1), `--gpu_plugin xenos
  --mnk_mode true`. All dlls re-staged. Tasks: menu-native-channel/DLC/heap-diag DONE; graphics-GUI-
  folder + GPU-composite + save-list + budget-RE open.

## ▶▶ 2026-07-19 (night) — ★ STRATEGIC PIVOT: OWN THE PIPELINES (PC game, not 360-on-PC) + big fixes
**User directive: this is a PC game, not a 360 game on PC — own audio/video/memory pipelines, break
360 limits (RAM/heaps), real graphics settings in the options menu.** Plan of record =
docs/RUNTIME_OWNERSHIP.md. Memories: [[fable2-ram-memory-model-map]], [[fable2-pausemenu-folder-bridge]],
updated [[fable2-black-skin-renderdoc]]. **Active TASK LIST (TaskCreate) drives autonomous work.**

**BLACK SKIN — root cause SETTLED (user was right it wasn't 2x):** it renders (no longer black) via
readback_resolve, but the face is CORRUPT. 1x test = identical corruption to 2x → NOT the downscale.
It's the MORPH COMPOSITE RTT: a GPU render-to-texture (per-source 2-tex layer {base, morph@entry+0x1C}
via MakeLayerFromSource 0x82A5C4A0 / StreamLayerSources 0x82A5C690, drawn into the 768x704 atlas
0x1B169; sources 0x10A07-0x10D27 load FINE). Fix per directive = OWN the composite host-side in
rexgpu-xenos (task 8), not readback band-aids. readback_resolve_only_dest_bases + log_resolve_readback
cvars PORTED to the runtime (diag only). Config back to draw_resolution_scale=2.

**MENU — native-channel routing BUILT (fixes grants + warps):** the pause-hub folder renders +
selection fires, but the old host bridge ran commands on L=4F640220 (NOT the live gameplay VM) →
grants no-op'd, warps CRASHED. NOW: ModApi.cpp adds `Debug.Mod.NextCommand()` + a host command queue;
ConsoleInjector routes ModBridge<code> → modapi::EnqueueCommand; modmenu.lua drive() polls it and runs
`__mm_dispatch(cmd)` in ITS correct-context VM. Built + bnk applied — NEEDS USER RUN to verify (if
warps still crash, wrap disruptive dispatch in a coroutine). Crash from auto-chest force-regive REMOVED;
fable2.com popup KILLED (GUI_WEBSITE swallow).

**★ RUNTIME-BUILD UNBLOCKED:** rexruntime failed a full relink (`undefined __std_search_1` from
xboxkrnl_io.cpp std::string::find substring searches) — fixed with `#define _USE_STD_VECTOR_ALGORITHMS 0`
in that TU. Runtime relinks cleanly again = all future RAM/kernel/audio ownership work can build.

**RAM:** 512MB is HARDWIRED to the 360 physical layout (xmemory.cpp aliases at 0x20000000 intervals) —
not a size bump; real lift = decomp game budgets → PC-native heaps ([[fable2-ram-memory-model-map]]).
Added FABLE2_MEMDIAG=1 heap-headroom log (MmQueryStatistics) to see how close to the ceiling we run.

**Test env unchanged:** `FABLE2_MODAPI=1 FABLE2_MODTEXT=1` (+FABLE2_MEMDIAG=1 for RAM),
`--gpu_plugin xenos --mnk_mode true`. rexruntime.dll + rexgpu-xenos.dll re-staged this session.

## ▶▶ 2026-07-19 (later) — ⚠ BLACK SKIN **NOT** FIXED (POOLBOOST verdict was premature); NEW FRONTIER = mips=1 TARGETS; MENU v11 STAGED
**User report: skin still black in-game.** The runs-506/507 "clean" verdict below measured the
wrong counters: `ALLZERO=0 / poolfail=0` only proved ALLOCATION was fixed. Re-reading 506/507/508:
**every run still has SAF `ret=0` failures (11/18/…), all on atlas targets with `mips=1`** — the
same permanently-black cohort as ever. POOLBOOST is real and stays (bucket-alloc FAIL count is now
0), but it fixed only one link.

**New RE this session (decomp `ghidra_out/decomp_atlascreate_20260719.txt`):**
- `CustomAtlas_EnsureTargetTexture 0x82A5BCE8` (full decomp): **target mip count = max over the
  SOURCE textures' mip counts** (`src+0xA0`, saturating-max loop), minus one per halving when
  clamped to g_max @0x8331AB00/04; fmt = 0x28 if any source DXN, else 0x25 if alpha else 0x23;
  target created via `FableTexture_CreateTarget 0x82A79E68` with `mips = max(mips,1)`.
- `CustomAtlas_CanDownsampleCheck 0x82B61180` (fill gate, full decomp): requires the resource's
  D3D fetch-header mip field (`hdr+0x2C >> 6 & 0xF`) nonzero AND half-res layout to fit a
  STRICTLY SMALLER pool bucket than the current one (`< wrapper+0x48`). A mips=1 target encodes
  header mips 0 → **gate always 0 → StartAsyncFill always false → StreamTick gives up at 30 →
  permanently black.** So the question is now: **why do the skin composites get created with
  mips=1, i.e. why do their SOURCE textures report mips<=1 (streamed-out?) at (re)create time?**
- `TextureResource_InitHeaders 0x82B5FBA8` has exactly 2 call sites: 821E0E7C + 82AB0060
  (undisassembled region). The failing cohort's res headers are never-written pool memory
  (garbage ASCII in the diag's texW/texH fields).
- Run 508 facts: `dispatch=57 enqueue=0` → ALL fill dispatches go **SYNC** in gameplay
  (asyncMgr DAT_834A4364 = 0); the old "async completion stall" theory is dead. `saf=123`,
  `safEarly=1`. Fail cohort = mostly 512x512 fmt 0x27 mips=1, plus **1024x1024 fmt 0x23 (DXT1)
  mips=1 (likely the hero skin) and 1024x512 fmt 0x27** — SAF#25/26 in the log.
- **NEW INSTRUMENTATION (built, in the current exe): `[atlasdiag] create#` lines** (MorphDiag.cpp's
  0x82A5BCE8 hook, now fires under `FABLE2_ATLASDIAG=1` too): dumps the OLD target state (why the
  recreate fired: ready/w/h/mips/fmt) + each SOURCE entry's texture (`entry+0x1C`: ready, w/h,
  mips, fmt) + the resulting target. **NEXT RUN (user): `FABLE2_ATLASDIAG=1` (+
  `FABLE2_MORPHBIND_DIAG=1` for the upload/bind side), reach black skin, then read `create#`
  lines: sources with mips=1 ⇒ chase why source mips never stream in (FableTexture_StreamMips
  path); sources with real mips but target mips=1 ⇒ the entry+0x1C read is wrong (adjust dump).**

**Menu extender v11 STAGED (built + bnk applied, untested):** the handoff's route 2 — the
takeover flag now lives HOST-SIDE in `ConsoleInjector.cpp`: GUI `MM_Open` select signals
`MMCTL_opt_on` (swallowed, TTL 20s), real-Options select / hub repopulation signal
`MMCTL_opt_off`, and the guisetup population wrap queries `MMQRY_options` — the fireEvent-native
hook (82BBCE80 is a lua_CFunction, decomp-verified `return 1`) ANSWERS the query by pushing a
number onto the GUI Lua stack (`PushGuestNumber`, headroom-guarded). Lua side:
`registerForEvent('MMQRY_options', ...) == 1` → swap OptionsMenu rows for the 8 mod rows.
First check in the next run's log: `MMCTL opt_on` → `MMQRY options -> 1` →
`MMDBG_SPT_OptionsMenu_1` = flag crossed; then mod rows should render. Both hooks luacheck-OK;
`apply_gui_mod.py` verified 270 entries.

## ▶▶ 2026-07-19 (00:30, user present, live-iterating) — BLACK SKIN = POOL EXHAUSTION (⚠ SUPERSEDED: partial fix only, see above); MENU EXTENDER SHIPPED
**Two tracks ran in parallel all night (runs 494-501, one per iteration). State:**

### 2026-07-19 continuation — runs 502-507
Black-skin update:
- Fixed a diag bug in `src/AtlasFillDiag.cpp`: `bucketchk inited=` now checks the texture
  resource pointer (`res`), not the wrapper pointer.
- Added live hooks for `TexturePoolBucket_Init 0x82B60348`, `TexturePool_PurgeAndPopCandidate
  0x821EE570`, and `TextureResource_ReturnToBucket 0x82A23818`, plus alloc/free counters.
- Run 502 bucket config proved the failing pool is bucket index 5:
  `entrySize=1,441,792`, `entryCount=16` (`bucket=4C12C298/4C12C038`, relocation varies).
- Normal black-skin allocation only attempts purge levels 0 and 1 (`purgeMode=0`). Level 2 is
  reachable only when `TexturePool_AllocFromBucket` is called with nonzero purge flag.
- Added env-gated runtime patch `FABLE2_POOLRETRY=1`: when `TexturePool_AllocFromBucket` returns
  0 with `purgeMode=0`, retry the same bucket/pool with purge flag 1 (adds level-2 attempt).
- Run 503 verdict with `FABLE2_POOLRETRY=1`: patch partially works. `poolretry` recovered real
  resources (`ret=4C1357B0`, `4C1357FC`, `4C135848`, etc.) and reduced `pool-create FAIL` to 1,
  but did **not** eliminate all `bucketchk ret=0` failures.
- Runs 504-507 verdict: `POOLBOOST` is the real fix. `TexturePool_Init 0x82B60910` is hooked
  before it computes total memory and the targeted config row `(f0=36, 1024x1024, mips=0,
  count=16)` is rewritten to `count=64`. Clean confirmation:
  - run 506 (`FABLE2_POOLBOOST=1`, no retry): `ALLZERO=0`, `poolfail=0`, `bucketfail=0`,
    `poolretry=0`.
  - run 507 (no `FABLE2_POOLBOOST` env, default path): row 5 logged `count=16 -> 64`,
    `bucket-init#6 entryCount=64`, `ALLZERO=0`, `poolfail=0`, `bucketfail=0`, `poolretry=0`.
  - Remaining `bucketchk ret=0` events are candidate fills with mip count 1 / header mipbits 0;
    they no longer produce zero-content skin uploads once the large BC bucket has room.
- `src/AtlasFillDiag.cpp`: pool boost is now **default-on** and independent of
  `FABLE2_ATLASDIAG`; set `FABLE2_POOLBOOST=0` only for A/B comparison. `FABLE2_POOLRETRY=1`
  remains diagnostic only and is not required by the current fix.
- Important field note: `82A23818` is a generic owner-list return routine; logs may show
  `bucket=8331AA88` when it is used by the compositor, not only the texture pool. Filter by the
  texture-pool bucket address range / entry-size bucket in later analysis.

Menu update:
- Runs 502-507 still showed `MMDBG_SPT_OptionsMenu_0`; v9 `g_Menu.__mm_options_mode` did not cross
  from input hook to the guisetup population wrap. The real Options page still opens. Menu work is
  parked per user directive to focus black skin.
- Run 508 tested v10 using parent scene attribute `MMOptionsMode=1` before `GUI:PushButtonEvent`
  to Options. It still logged `MMDBG_SPT_OptionsMenu_0`: the native Options transition gets a fresh
  page/menu context and neither Lua globals, g_Menu fields, nor parent element attributes are a
  reliable cross-transition channel. Next menu route should be one of:
  1. build an explicit in-place page manager in the pause menu using `RemoveAllChildren` /
     `PopulationTable` / `RepopulateFromPopulationTable`, with a custom B-back restore path;
  2. or add a small host/native flag keyed by `MMDBG_SEL*_MM_Open` and expose it to the GUI
     population hook, rather than trying to smuggle state through Lua/page objects.

### Black skin — ROOT CAUSE FOUND: texture-pool bucket exhaustion
Full causal chain, each link live-verified (`docs/BLACKSKIN_RE.md` has all data):
`TexturePool_AllocFromBucket 0x821CE0B8` fails (bucket 0x4C12C038 free-list EMPTY; "find-bucket"
never fails — it's exhaustion not sizing) → `TexturePool_CreateTextureWithFallback 0x821D79B8`
fails even at min size → recreated CustomAtlas target left header-less (`inited=1` but mipbits=0 —
headers were valid before recycle) → `CustomAtlas_CanDownsampleCheck 0x82B61180` ret 0 →
`FableTexture_StartAsyncFill` fails → `FableTexture_StreamTick 0x8225F9D0` (stream ptr NULL ⇒
elapsed=0xFFFFFFFF ⇒ same-tick give-up >29) → `ReleaseResourceWrappers` → reported done → black.
**The atlas manager RETRIES every ~10-15s** (bucket-alloc FAIL pairs at :05/:15/:30) — so if the
pool frees up, skin would heal; it never does. **NEXT: why is the pool exhausted in the recomp?**
`TexturePool_AllocFromBucket` retries with purge levels 0..2 via `Function_821EE570 0x821EE570`
(iVar2+0x40 = "pool full" latch!) — decompile 821EE570 (the purge/alloc core) + find the FREE/
return-to-bucket path + the pool ctor (bucket counts; pool global DAT_8349FA0C writer candidates:
832B37D8 Function_832B37D8 = cinit, 82A50724 EngineResourceList_StaticInit). Leak vs sizing vs
purge-callback-broken. Tool: `src/AtlasFillDiag.cpp` (FABLE2_ATLASDIAG=1) — v5 logs alloc-fail
mode + bucket dumps; add alloc/free balance counters next.

### Menu extender ("act like the Options menu") — ARCHITECTURE SHIPPED, v8 awaiting user verdict
User directive: **a reusable extension/hook framework for custom native menus.** Built tonight:
- **Named-command bridge (C++, ConsoleInjector.cpp, BUILT):** GUI fires event `ModBridge_<cmd>` →
  host FIFO queue (16 deep, ALL picks run, replacing the old single-slot code 1-5) → gameplay VM
  `__mm_dispatch('<cmd>')` at the injection-safe point. Numeric codes 1-9 kept for back-compat
  (6=halo_set 7=gear_set 8=warp BWSMarket 9=warp FairfaxCastleGardens added).
- **Gameplay dispatcher (modmenu.lua, in gamescripts_r.bnk):** `MM_COMMANDS` table (gold, halo_set,
  gear_set, tp_chest, unlock_chest, warp_guild/bower/bws/ffx, skip_adult) +
  `_G.__mm_register_command(name, fn)` — **mods extend menus in pure Lua, no C++**.
- **GUI side (guiscripts.bnk hooks):** pause-hub 'Mod Menu' button clone-spawned from Logbook
  (real native button — SpawnButton recipe: CreateMenuItem(S1,S2,S3,F3,F1,F2,F4) + AddChild +
  SpawnedItems insert; emphasis mirrored from Logbook via SetItemEmphasis wrap — the "greyed text"
  was the EMPHASIS state, not ghosting). Select → real Options PushButtonEvent (native page turn)
  + takeover flag → SetPopulationTable wrap swaps the OptionsMenu rows for 8 mod rows
  ({Name,TextTag,IconTexture,EventType} schema; input hook maps MM_* → ModBridge_<cmd>).
- **★ MENU-SYSTEM FACTS (bytecode+live-proven, runs 500/501):** the pause book is ONE MORPHING
  MENU (same g_Menu element repopulates per screen; engine population calls DO pass through the
  g_Menu.SetPopulationTable table slot — wrap fires); the 'PauseMenu' population branch has ZERO
  rows (hub buttons are scene-driven via SpawnButton); **GUI scripts are SANDBOXED: global READS
  fall through to shared globals, WRITES stay per-script** — `_G.x` (run 500) AND event-callback
  `self.x` (run 501) both failed to cross to the guisetup wrap. Cross-script state must be a
  FIELD WRITE ON THE g_Menu TABLE via the global read (method installs prove that channel is
  shared). **v9 (g_Menu.__mm_options_mode) applied to guiscripts.bnk, UNTESTED — first check of
  next run: `MMDBG_SPT_OptionsMenu_1` in the log = flag crossed = mod rows should render.**
- Remaining after v8 verdict: page-manager module (declarative page defs + page stack + B-pops),
  restore-hub handling, per-page population script names.

### Session gotchas (learned the hard way)
- **⚠ `apply_mod.py` DEFAULT HOOK is `myconsolehook0.lua` (chest-only), NOT modmenu.lua** — a bare
  `apply_mod.py` call WIPES the mod menu/childhood-skip/dispatcher. Always:
  `py -3.12 tools/lua_mod/apply_mod.py <bnk> tools/lua_mod/modmenu.lua`. (Happened + fixed this
  session; verify with script_index search `__mm_dispatch`.)
- MorphDiag.cpp already hooks 0x82A5BCE8/0x82A79E68 etc. — re-hooking = duplicate symbol at link.
- The `__imp__sub_X` weak-alias forwarding recipe (override `sub_X`, call `__imp__sub_X`) is THE
  generic wrap pattern — AtlasFillDiag.cpp is the reference implementation.

## ▶▶ 2026-07-18 (later night, user present) — BLACK-SKIN ROOT CAUSE CORNERED LIVE
**The black-skin bug is now cornered to a specific failing check + a give-up path** (full RE:
[BLACKSKIN_RE.md](BLACKSKIN_RE.md) §"THE ASYNC-FILL CHAIN IS MAPPED"; memory
`fable2-black-skin-renderdoc`):
- Chain fully RE'd + 17 Ghidra labels (`ghidra_out/labels_atlasfill.tsv`; labels_in.tsv = 1066).
- **Killed by live runs (logs 492/494/495):** codegen-missed-target hypothesis (zero [DISCOVER]
  across three full repro runs), host-cache staleness (0 STALE), "pump not called" (pump+batch
  verified running per-frame live).
- **★ LIVE VERDICT (log 495): 18 consecutive `FableTexture_StartAsyncFill 0x82A7A680` calls FAIL
  (ret=0) at the exact moment the composites go black, and are NEVER retried** (fail cohort has
  fill-total 0x7FFFFFFF = uninitialized vs 4 for the succeeding cohort). Fail branch = pool-bucket
  check `0x82B61180` returning 0 → fill silently dropped. The caller (in `sub_8225F9D0`, site
  0x8225FADC) attempts fills only after elapsed≥3 and **gives up at elapsed≥30 via 0x82A7A388,
  returning "done" → permanently black**.
- **★ NEW TOOL: `src/AtlasFillDiag.cpp` (env `FABLE2_ATLASDIAG=1`, in CMakeLists)** — live counters
  + decision logs across the whole chain via the **`__imp__sub_X` weak-alias forwarding recipe**
  (override `sub_X` strong, forward to `__imp__sub_X`; first use — THE generic wrap pattern for
  instrumenting/patching any generated fn). v3 adds `bucketchk#` logs (0x82B61180 inputs: wrapper,
  bucket limit +0x48, w/h, mip bits, ret) — the run that answers "why does the check fail" .
  ⚠ 0x82A5BCE8 is already hooked in MorphDiag.cpp (duplicate symbol if re-hooked).
- Decompiler gotcha (recurring): `IntrusivePtrRelease 0x821B7B18` + CS-guard `0x82200770` are
  mis-bounded and noreturn-poison every decompile that calls them (hid the `DAT_834A4364` store in
  `FableTexture_StreamMips 0x821D7A98`). Fix their bounds in Ghidra someday.
- Childhood-skip fixes (camera-flail + dog) still UNTESTED. Editset restore test still pending
  (user will test AssetBrowser later).

## ▶▶ 2026-07-18 (night, autonomous — user away) — LEVEL-EDITOR EDIT SETS + MOD EXPORT SHIPPED
**User directive captured this session: build OUR OWN C++ modding environment on top of all our
data (decomp, tools). Architecture + phased plan: [MODDING_ENVIRONMENT.md](MODDING_ENVIRONMENT.md)
("Fable II Studio"). P0 shipped tonight:**
- **★ Durable edit sets** (`Fable2AssetBrowser .../Level/LevelEdit.cpp`): the level editor's FULL
  edit state (moves/rotates/deletes + placements incl. name/scale/chest-items + chest-content
  edits) now journals to `edited_levels/<level>.editset.txt` (debounced per-frame `LevelEdit::Tick`,
  atomic tmp+rename). Rehydrates on level load → edits survive app restarts AND crashes (before:
  only placements survived, names/scale silently dropped, object moves lost entirely). Object-edit
  records carry a bnk size+mtime stamp and are dropped (with a warning) if the bnk changed — they
  address raw file offsets. On successful save, records move to `BAKED <utc>` history lines
  (provenance). Restore Defaults deletes the journal.
- **★ "Export Edits as Mod"** (Level menu; `LevelEdit::ExportMod`): copies every level file that
  differs from its `.bak` backup into `<assets>/mods/enabled/<name>/<game-relative path>` + a
  `f2ab_mod.json` manifest + the editset journals under `_f2ab/` — i.e. level edits become
  **packageable, non-destructive overlay mods** for the recomp's existing `ModSupport.cpp` staging
  (name-sorted load order). Workflow: edit → Save Level → Export Edits as Mod → (optionally)
  Restore Defaults so the base game stays pristine while the mod carries the edits.
- Build: AssetBrowser compiles clean (ninja, `source/build`). NOT committed (repo had prior
  uncommitted session work; user commits). NOT run-tested (GUI app — user rule: don't auto-open
  GUI apps); code compile-verified + reviewed. First user test: open a level, move a prop, close
  app, reopen → expect "edit set restored" in the output log + Save Level* enabled.
- Known parity-limitation kept from the old additions.txt flow: if the LEV bake succeeds but the
  GDB contents rewrite fails, pending placements stay journaled and could double-bake on a later
  save (pre-existing hazard, unchanged).
- **★ `f2tool` CLI shipped (Studio P1 beachhead):** headless CLI over the AssetBrowser format code
  (`source/tools/F2Tool.cpp`, cmake target `f2tool`, builds to `source/build/f2tool.exe`):
  `list`/`extract`/`verify <bnk>` (verify round-trips EVERY entry — streams the 2.5 GB levels.bnk;
  gamescripts_r 555/555 + guiscripts 270/270 verified clean), **`inject <bnk> <entry> <file>`**
  (BnkWriter rebuild, auto one-time `.bak`; END-TO-END TESTED on a scratch bnk incl. a grown
  entry, rebuilt bank verifies clean; RAW entries only — chunk-compressed entries still need
  `tools/lua_mod/bnk_repack.py`), `hash [--lower] <name>` for the FNV-1 resource/text-name hash,
  `editset <file>` journal summary, `mod <dir>` inspect.
  **BnkWriter was freed of its UI deps** via a new `src/Level/F2Host.h` seam (app impl:
  `src/UI/F2HostApp.cpp` → OutputLog/Progress/State; CLI impl in F2Tool.cpp) — the pattern for
  extracting the rest of libf2. App + CLI both build clean.
- **★ Menu-system extender RE updated, see [MENU_SYSTEM_RE.md](MENU_SYSTEM_RE.md):** the latest
  user run proves the pause-menu bridge fires end-to-end (`Fable2_491.log`: `MMDBG_SEL1_MM_Open`
  → `ModBridge5` → gameplay `modbridge_cmd.lua` executed). The remaining problem is not bridge
  viability; it is first-class menu/list integration. Cleaned the GUI hooks so population injection
  stays in `gui_modmenu_hook.lua`, selection dispatch stays in `gui_expandablemenuinput_hook.lua`,
  and duplicate `ModBridge5` events are gone. `apply_gui_mod.py` now compares full hook bytes so
  changed hooks reapply instead of being skipped because markers are present. `guiscripts.bnk`
  patched + verified current.
- **The rebuilt `rexgpu-xenos.dll` IS STAGED** (user closed the game 15:24; exit-stager copied it,
  hashes verified identical) — and the diagnostic RAN (user test 15:34, log Fable2_491.log):
  **★★ LIVE VERDICT — GUEST WRITER CONFIRMED, host cache exonerated** (STALE=0; hero skin atlas =
  768x704 DXT1 @ guest page 0x1B169 uploaded ALL-ZERO, bound zero 123×/frame; loading-screen SYNC
  composite path produced real content at boot, async gameplay path never fills — recycle-then-
  never-fill proven). Follow-up RE agent dispatched on the async-fill chain (0x82181828 →
  StartAsyncFill 0x82A7A680 → 0x82B61180/0x82B60E20, callbacks 0x8331AB78); results append to
  [BLACKSKIN_RE.md](BLACKSKIN_RE.md).
- **★★ Black-skin decomp/RE (user asked tonight) — BREAKTHROUGH, see [BLACKSKIN_RE.md](BLACKSKIN_RE.md):**
  the composite is **CustomAtlasTexture** — a pool-allocated texture the game **CPU-fills in place**
  via texture streaming, NOT RTT/resolve (why resolve diags never fired); "fmt 0x23/0x28" are game
  enums = DXT1/DXN (`g_TextureFormatTable @ 0x8331DAD8`); all-zero DXT1 decodes to opaque black.
  Path: manager 0x82A5CAC8 → create 0x82A5BCE8 → compositor 0x82181828 → bind 0x82A9F778 (ordinary
  texture-slot swap). Top hypotheses: (1) guest writer never completes; (2) host write-watch/
  invalidation miss (possibly our stale-page-recovery fix). +42 Ghidra labels. **NEXT RUN:**
  `FABLE2_MORPHBIND_DIAG=1` (new, in `texture_cache.cpp`) then grep `ALL-ZERO`/`ZERO-CONTENT`
  (⇒ guest) vs `STALE` (⇒ host). ⚠ needs the rebuilt `rexgpu-xenos.dll` (separate cmake target,
  NOT built by build_runtime2.cmd) — an exit-stager (`stage_rexgpu_after_exit.ps1`) copies it into
  the nightly build dir once the currently-running Fable2.exe closes; verify hashes in
  `stage_rexgpu_after_exit.log` first.

## ▶▶ 2026-07-18 (evening) — read memories `fable2-modapi-register-native`,
## `fable2-pausemenu-integration`, `fable2-black-skin-renderdoc` for full RE detail + dead ends.

**Launch env for all mod features: `FABLE2_MODAPI=1 FABLE2_MODTEXT=1`** (+ `FABLE2_CONSOLE=1` now
SAFE — see console gate; + `FABLE2_RESOLVE_DIAG=1` for the black-skin hunt). Standard flags:
`--allow_game_relative_writes true --game_data_root <assets\game> --gpu_plugin xenos --mnk_mode true`.
⚠ TOOLING RULE (user-set): **DO NOT auto-open GUI apps** — `qrenderdoc.exe --python` opens the full
Qt GUI (rejected). `renderdoccmd thumb` is headless and fine.

**★ CHILDHOOD-SKIP FEATURE — working end-to-end (user-confirmed core), polish iterating.** New game
→ "Sparrow's Path" choice → cutscene-free childhood (hold-until-choice wraps neutralise scenes /
script-rules / control-grabs / SetHeroLookTarget / SetLookAtCamera / DisplayTutorial /
DisplayInfoBoxParams, skip-mode-gated only) → skip SIGN (custom text) → **"Old Town's Fate" warrants
chooser** (Derek/Arfur/Cancel → `Gameflow.ChildhoodResolutionEvil`, the RE'd persistent flag adult
Old Town reads; the vanilla dev-skip can ONLY give the good outcome, so this ADDS a capability) →
Fairfax. STATE of the tail (all in `tools/lua_mod/modmenu.lua`, applied to gamescripts_r.bnk):
  - **Extended-loading REMOVED** — the fade-mask (LockScreenFade) was holding the loading screen up
    until the dialogue-gated unfade released it (user-diagnosed). Deleted; camera fixed at source.
  - **Camera-flail at Fairfax — FIX STAGED (untested):** user nailed it = skipping BEFORE the crowd
    cheer leaves a QUEUED look-camera that fires post-jump at stale childhood coords (breadcrumb
    `skip-flush lookat=false` confirmed the look isn't engaged at sign time). Fix: onSkipFired flushes
    unconditionally (game's own ClearLookAtCamera recipe, questmanager proto[69]) + sets
    `__mm_block_lookat` so the SetLookAtCamera wrap keeps swallowing looks across the transition
    (cleared ~150-600 ticks post-skip).
  - **Dog missing at adulthood — FIX STAGED (untested):** QC060 resurrects the childhood dog from
    limbo by `ChildhoodVars.DogName`; our earlier limbo got an EMPTY name (skip before the qc040
    bully quest = no real dog, GetDog() is a nameless placeholder). Fix: ensure a dog via
    `ScriptFunction.CreateDog()`, then limbo by name; if still nameless, `DogName=nil` → QC060's
    reposition-existing else-branch. Breadcrumb `skip-dog alive=.. name='..'`.
  - **Good-deed morality toast** not rendering (the +5 VFX applies — Stat works — but no box);
    deferred + `IsFullCutscenePlaying`-gated so it fires once the study dialogue clears. LOW priority.

**★★★ register_native + Debug.Mod NAMESPACE — DONE & PROVEN.** `src/ModApi.cpp` (in CMakeLists,
gated FABLE2_MODAPI=1). Natives callable from game Lua: **`Debug.Mod.Ping()` / `Debug.Mod.TextSet(tag,
str)` / `Debug.Mod.Log(msg)`** (+ `Debug.ModPing/ModTextSet/ModLog` aliases). `Mod.Log` → `[modlua]
...` host log = the mod debug channel (already used for skip breadcrumbs). Mechanism: AllocateThunk
from the exe + pushcclosure/setfield into the Debug class table at a luaD_call depth-0 safe point,
gated by `ReadyForInjection` (raw class-table probe + ≥64 Lua-stack slots). `Debug.Mod` table is
published script-side by the gamescripts hook (plain table raw-stored into Debug; top-level `Mod`
global can't work — env is metamethod-proxied). Full RE: memory `fable2-modapi-register-native`.

**★ CONSOLE gate — crash class FIXED.** `FABLE2_CONSOLE=1` was corrupting the heap at boot (Lua
stack realloc under live StkIds). All injection paths now gated by `ReadyForInjection`; verified
5+ min survival, script-side `Debug.ModPing` ran from the console. (Heartbeat diag: state-init ticks
have only ~38 headroom → gate correctly defers; ready ticks come at gameplay-state init.)

**SUPERSEDED by the night update above — PAUSE-MENU Mod Menu was visible, but NOT openable (screenshots 2026-07-18).** The GUI-bank append is
real: "Mod Menu" renders in the pause hub. But the four user screenshots prove it is still outside
the native selectable-list lifecycle: on first/transition opens it visually collides with the Save
row, and after refresh the A-selection bubble/hitbox can sit on an empty slot ABOVE the rendered
"Mod Menu" text. Pressing A does not open the R3 mod menu. Treat this as a GUI layout/index bug, not
a bridge-success bug: PopulationTable/NumberOfItems/SpawnedItems/highlight rows are out of sync with
the scene-fetched Save/Quest/etc. items. Existing diagnostics now use `registerForEvent` breadcrumbs
(`MMDBG_*`) because arbitrary GUI `fireEvent("MMDBG...")` did not reach the host. NEXT: stop trying
host raw `OnPressA` alone; fix the GUI-side list construction point so our entry is a real spawned
button with a matching selection index, or patch the native scene/menu population. Memory
`fable2-pausemenu-integration`.

**★ BLACK HERO/DOG SKIN — BNK load basically ruled out; morph-link/render path now targeted.**
RenderDoc thumbnail proves the hero appearance-MORPH composite is black (skin/fur RTT, shared
hero+dog): hero skin black + clothing fine + NPC skin correct in the SAME frame. Affects child hero
too. User confirmed it predates resolution scale. Later `FABLE2_MORPH_DIAG=1` logs prove
`skeletalmorphs.bnk` loads nonzero data, save `herosave.bin` + `texturemorphs.bin` load, 12 texture
morph entries import, and composites are created (mostly fmt 0x23/0x28). Resolve skip/drop warnings
do NOT fire in the repro logs, and the backend does not see fmt 0x23 morph outputs as ordinary bound
texture fetches. NEXT: RE/log the object-to-material/fetch binding path from the morph composite
objects, not the BNK loader. Memory `fable2-black-skin-renderdoc`.

**Decomp:** +20 Ghidra labels today (Lua lapi/core — `lua_pushlstring @0x82A244D8` corrects the old
census guess — + the boot bank-mount chain: `bank_open_by_name @0x82C6D7C8` verified). labels_in.tsv
= 975. Open Track C: the LS_* level-load state machine (names @0x820F8B44 = LS_PROCESSING_ENGINE_LEVEL
etc.) — disasm its dispatcher next.

## ▶▶ 2026-07-18 — register_native historical note (SUPERSEDED by current section above)
The section below was written before the later console gate and Debug.Mod namespace work. Treat it as
mechanism/history only; do not follow its old warning that `FABLE2_CONSOLE=1` is unsafe. The current
state is: console injection is gated by `ReadyForInjection` and has survived armed runs.

## ▶▶ 2026-07-18 — ★★★ register_native WORKS: host natives callable from game Lua
Full detail in memory `fable2-modapi-register-native`. `src/ModApi.cpp` (in CMakeLists, gated
`FABLE2_MODAPI=1`) exposes host C++ natives to the gameplay VM — **`Debug.ModPing()` /
`Debug.ModTextSet(tag, text)`**, self-test log-proven (bind → VM luaD_call → host fn ran) and
survived real gameplay. Mechanism: `AllocateThunk` from the exe (runtime exports all symbols) +
`lua_pushcclosure` sub_8219AA80 + `rex_lua_setfield` 0x82A246C8 into the **Debug class table** at a
luaD_call depth-0 safe point, gated on (a) a RAW host-side table-walk probe (Debug populated ~20-30s
into boot; layout offsets in the memory) and (b) ≥64 slots Lua-stack headroom. **KEY RE: the game's
_G is metamethod-proxied** — naive global setfield goes to a reflection backing store, missed reads
auto-vivify self-referential proxy tables, and CALLING a pre-init proxy AVs natively (pcall can't
catch). **⚠ FABLE2_CONSOLE=1 is currently UNSAFE**: its fingerprint RunGuestFileOnL now fires during
boot data-load and corrupts the heap (dlmalloc try_realloc_chunk AV — Lua stack realloc moves the
buffer under the engine's live StkIds; proven with a NO-OP command file). That caused all 2026-07-18
crashes incl. the user's continue-from-save. Leave console OFF until it gets the same gates.
**Mod-feature launch env: `FABLE2_MODAPI=1 FABLE2_MODTEXT=1`** (console unset). Also fixed: the bnk
carried a STALE modmenu.lua (last night's `stopInteractiveCutscene` sister-scene abort was edited
23:22 but bnk written 23:13 — never applied); re-applied 2026-07-18, awaiting user childhood retest.
NEXT: gate+re-enable the console; more natives (asset/input hooks); a dedicated plain Mod table
(needs lua_createtable pin).

## ▶▶ 2026-07-17 (night) — ★ THE LUA SCRIPTING API IS MAPPED: 3348 NATIVES LABELED
Final state of the day's RE arc (supersedes the two sections below; full detail in memory
`fable2-decomp-labeling-progress`). `tools/ghidra_label/FindLuaNatives5.java` is now self-contained
and finds EVERYTHING: raw .text bl-scans for both pushcclosure SITES and registrar CALLERS (Ghidra
refs only cover disassembled code — ~530 sites incl. the whole Debug class were invisible),
stack-Lh-string name resolution, and 64-bit `ld` member-fn-ptr method extraction. **3348 unique
native methods labeled (0 failures); Class.Method catalog with 3592 rows in
`ghidra_out/lua_natives5_catalog.tsv`** — `Debug.CreateEntityAt`@8245E038, `Player.SetSafetyMode`,
`Inventory.AddItemOfType`, the full entity-component API. **RESIDUE CLOSED (same night): the 96
dark callers were 7× `luaL_register` (= the Lua 5.1 STDLIB — new tool `FindLuaLibs.java` parsed
the luaL_Reg arrays → +97 labels `lua_<lib>_<name>`) plus internal Lua-API wrapper calls (not
registrations). And `ShowMenuBox`/`AddAllScriptItems`/`AddAllCarbonatedPrizes` were NEVER natives:
they're script-side Lua (miscfunctions.lua wraps native `DisplayMenuBox`@82303EE0;
weaponinventory.lua defines all 30 AddAll* item granters) — their strings don't exist in the exe
(verified via new `FindStrRaw.java`). THE SCRIPTING-API ENUMERATION IS COMPLETE: 3348 natives +
97 stdlib; anything absent from the catalog is script-side Lua.** Names like
TeleportPlayerTo/GetEntityWithName/SetGameflowPosition are script-side Lua — correctly absent.
**SUPERSEDED by the night update above — PAUSE-MENU stage 1 = PARKED** (memory `fable2-pausemenu-integration`): item not visible yet, BUT
plain-text `guisetup.lua` replacement RUNS THE WHOLE MENU SYSTEM (GUI-state injection beachhead
proven; currently deployed, benign; revert = `python tools/lua_mod/apply_gui_mod.py --revert`).
art\gui\gameface\*.lua = dev leftovers (break-test proven). GUI dofile is BNK-only (no loose
files). Hypothesis: Options book pages are SCENE-populated (optionsscreen.bgf embeds pages) —
next: RE expandablemenuspawning + who calls SetPopulationTable; try a wheel-style screen.
`draw_resolution_scale = 2` remains active (verified applied; judge visually in person).

## ▶ 2026-07-17 (later) — REFLECTION REGISTRARS CRACKED: +1358 MORE NATIVES (API now ~1500 named)
Continuation of the batch below. `tools/ghidra_label/FindLuaNatives5.java` solved the ~1023
"runtime-named" registrar sites: Ghidra had no refs to them because their callers are in
UNDISASSEMBLED code → raw-scanned .text decoding every `bl` word (1975 callers / 1021 entries),
then parsed each caller with symbolic r1+offset tracking — the name is a stack Lh-string built by
`lh_string_assign_cstr(&slot, "Name", -1)` right before `registrar(r3=env, r4=&classStr,
r5=&nameStr, r6=methodPtr)`. **1503 callers parsed → 1358 methods labeled** (600 fns created;
0 failed): the whole entity-component scripting API (Creature.Kill, Dog, Building, Shopkeeper,
Firearm, GuildMessages.Post, Inventory.AddItemOfType, PlayerWebsiteUnlocks.IsItemUnlocked, …).
Class.Method catalog: `ghidra_out/lua_natives5_catalog.tsv`. Multi-name addresses = linker
identical-code folding (trivial getters share one body) — first name wins, rest in the comment.
**Residual (next lever): 472 dark callers — the Debug/GUI/QuestManager/Gameflow singleton classes
never appear, so they use yet another variant; instrument FindLuaNatives5's dark branch + disasm
a few dark sites.**

**Resolution scaling FIXED FOR REAL:** the cvar is **`draw_resolution_scale`** (earlier note said
`resolution_scale` — wrong) and it must sit at the TOML TOP LEVEL (above `[log.levels]`). Now
active (`= 2`) in `Fable2Recomp/out/build/win-amd64-nightly/Fable2.toml`, verified applied via the
cvar plumbing (no `unknown cvar` warning; see memory `fable2-console-limits-removal`). Note:
invisible over remote view — judge in person.

**SUPERSEDED by the night update above — PAUSE-MENU MOD MENU — STAGE 1 DEPLOYED (2026-07-17, awaiting user menu-test):** GUI-side hook
live in guiscripts.bnk (pristine backup `guiscripts.bnk.orig_backup`; revert =
`python tools/lua_mod/apply_gui_mod.py --revert`). Injection = the retail PLAIN-TEXT element
script `art\gui\gameface\expandablemenu.lua`; the hook wraps global `SetPopulationTable` (the ONE
population fn — 'PauseMenu' branch exists, front-end + pause share 'OptionsMenu') to append a
'Mod Menu' item to the Options screen, and swallows its PushButtonEvent. TEST: title screen →
Options → 5th entry (may be BLANK if the gameface text path doesn't render raw tags — that
outcome decides whether we need a tag fallback). Stage 2 = the native ModBridge (memory
`fable2-pausemenu-integration`).

## ▶ 2026-07-17 (autonomous) — LUA NATIVE API MAPPED (closure-push pattern cracked)
Track-B decomp session, no game testing. **The `823CE420` registration pattern is cracked and
tooled**: new `tools/ghidra_label/FindLuaNatives2.java` walks all 1173 `lua_pushcclosure`
(@8219AA80) call sites, raw-PPC-parses each site (96-word window, volatile-reg clobber modeling,
per-bl snapshots) and pairs the fn ptr stored in the userdata (4-byte plain fn at +0; 8-byte
bound-method at +4) with the `lua_setfield` name. **+150 Ghidra labels applied**
(`ghidra_out/lua_natives2.tsv`): 146 named natives — incl. `lua_native_LoadLevel`@82350D20,
`RunScript`@82459928, `SaveGame`/`AutoSaveGame`, `FNVHash`@8245B290, `PlayMusic`,
`UnlockAchievement`/`WriteStat`, the GDB accessor API @827E6xxx — plus `lua_newuserdata`@82227880,
`lua_pushcclosure`@8219AA80, `lua_rawgeti`@82229108, `lua_native_class_lookup`@823CE2E0.
Also new: `NameAt.java` (symbol-at-address probe). **Known gap (next lever for the full API):**
the other ~984 sites are template-instantiated generic registrars whose method NAMES arrive at
runtime from reflection/property structs (`r31 = *(*(arg))`) — enumerate them via their callers'
constant struct ptrs or by dumping the binding-descriptor arrays in .data. Details: memory
`fable2-decomp-labeling-progress`. The user-present TO-DO list below is unchanged.

Also started **Track C (asset loaders, +7 labels)**: `mount_tu1_data_bnk`@822F2F30,
`bank_open_by_name`@82C6D7C8 (verify), Lh string ctor/dtor/assign helpers (incl.
`lh_string_assign_cstr`@8222CED0 — closes the old "script-NAME setter" lead),
`cinit_set_engine_level_extension`@83254A70. Open leads: the levels.bnk/streaming.bnk/
gamescripts_r.bnk mounts live in the undefined region ~822F2C34 inside the big boot fn; the LS_*
level-load state machine (names @0x820f8b44+). New tool `RemoveFuncAt.java`; ⚠ DecompFuncs
creates a function at the given address if none exists — only pass real entry points.

## ★★ NEXT SESSION — START HERE (as of 2026-07-16 night)
**State:** clean + normal boot. In-game **MMB cheat menu** deployed & working (`tools/lua_mod/modmenu.lua`
in gamescripts_r.bnk): MMB/R3 in-world → Ancestor Chest / Give Items+Gold / Warp / Skip Story. Dev
mode on (`episodic_exec.txt`). Runtime dll unchanged (backup: `rexruntime.dll.working_backup`).

**TO DO WITH THE USER (need live testing — do NOT do these blind):**
1. **★ VERIFY EVERY MOD-MENU OPTION** (user request 2026-07-16 night) — MMB in-world, walk through ALL
   entries and confirm each fires: Ancestor Chest (teleport / unlock-contents), Give Items+Gold (1M gold /
   Halo-Spartan set / all-script-items), Warp (Bower Lake / BWS Market / Fairfax Gardens / Guild Cave),
   Skip Story (Childhood→Fairfax / Adult). Note which work vs not.
   - **KNOWN:** "Give ALL Script Items" (`Debug.AddAllScriptItems`) user-reported "didn't seem to work" —
     RE'd: it adds QUEST/SCRIPT items (QC120_CrucibleRuleBook, QC180_Seal, QC230_MusicBox, ...), i.e.
     invisible quest junk, NOT visible gear — so it likely ran but added nothing noticeable. FIX next
     session: relabel it "Give Quest Items", or replace with a useful visible-gear grant (weapons/
     clothing via Inventory.AddItemOfType with real ObjectInventory* IDs). The Halo/Spartan set
     (`AddAllCarbonatedPrizes`) is the good visible-gear one.
2. **Resolution scaling** — uncomment `resolution_scale = 2` in `out/build/win-amd64-nightly/Fable2.toml`,
   relaunch, confirm sharper + stable (memory `fable2-console-limits-removal`). Try 3 if smooth.
3. **SUPERSEDED by the night update above — Pause-menu "Mod Menu" entry** (user's active ask) — BUILD together (fragile GUI-bank edit + native
   bridge). Full plan in memory `fable2-pausemenu-integration`.
4. **Black adult dog/hero skin** — needs a RenderDoc/PIX capture of the adult scene (memory
   `fable2-adult-hero-black-skin`, updated: dog too → shared skin/fur texture-morph composite).

**AUTONOMOUS-OK (no testing) next:** continue decomp labeling (memory `fable2-decomp-labeling-progress`;
tool `FindLuaNatives.java` — extend to the 823CE420 register pattern for the full native API); map
resource/asset loaders (Track C). New memories this session: fable2-quest-interactable-system,
fable2-console-limits-removal, fable2-decomp-labeling-progress, fable2-lua-native-menu,
fable2-babel-text-system, fable2-episodic-exec-devmode, fable2-pausemenu-integration.

## ▶▶ 2026-07-16 (late) — NATIVE MOD MENU + TEXT-HASH CRACKED + QUEST SYSTEM RE'd
**Working now (deployed to gamescripts_r.bnk):** a native in-game **Mod Menu** — press **R3/MMB**
in-world to open a real Fable-II book-style menu (`tools/lua_mod/modmenu.lua`). 2-level structure:
**Ancestor Chest** (teleport / unlock-contents), **Give Items+Gold** (1M gold /
Debug.AddAllCarbonatedPrizes Halo-Spartan set / Debug.AddAllScriptItems), **Warp** (Bower Lake /
Bowerstone Market / Fairfax Gardens / Guild Cave via Debug.LoadLevel), **Skip Story** (Childhood→
Fairfax via `Gameflow.ChildhoodVars.SkipToLuciensStudy()` / Adult via SetGameflowPosition DebugQC060),
Close. Trigger = `MESSAGE_EVENT_GENERIC_RIGHT_STICK_PRESSED` poll. User-confirmed the base menu opens
on MMB; the expanded submenus/actions were added 2026-07-16 (untested by user yet, all pcall-guarded
so safe). ShowMenuBox gotchas (memory `fable2-lua-native-menu`): ≤5 opts, 1-based, no-pcall-around-yield.
**Key unlock:** the text system renders RAW STRINGS verbatim on a tag-lookup miss, so custom UI text
needs ZERO babel work — just pass the words. (Along the way the text-tag hash was cracked = **FNV-1
32-bit**, basis 0x811c9dc5 — see memory `fable2-babel-text-system`; and `fable2-lua-native-menu` for
the ShowMenuBox gotchas: ≤5 options, 1-based index, no-pcall-around-yield.)
**Quest system RE'd** (memory `fable2-quest-interactable-system`): quests = coroutine-thread script
classes via QuestManager.NewQuestThread/NewEntityThread; interactables = OnActionUse* classes bound
by an entity's ActionUseScript property; childhood-skip-sign = SkipToLuciensStudy. Foundation for the
community node-based quest tool + our CreationKit direction.
**SUPERSEDED by the night update above — IN PROGRESS — pause-menu entry (user wants "Mod Menu" IN the pause menu):** fully designed, not yet
built — needs a native GUI->gameplay bridge (states are hard-separated). Complete build plan in memory
`fable2-pausemenu-integration`. The GUI-bank edit is fragile (untested = could break the pause menu),
so BUILD IT WITH THE USER PRESENT to test/recover. episodic_exec.txt dev-mode marker also created
(memory `fable2-episodic-exec-devmode`).

**AUTONOMOUS SESSION additions (2026-07-16, no user testing — safe/zero-risk work):**
- **★ Resolution scaling found (memory `fable2-console-limits-removal`):** internal supersampling via
  the `resolution_scale` cvar (1-8, GPU-clamped, requires restart). Added a COMMENTED toggle to
  `out/build/win-amd64-nightly/Fable2.toml` — uncomment `resolution_scale = 2` for crisp 1440p. FPS
  already uncapped (~194). RAM (512MB) is the hard one (engine work, deferred).
- **★ Decomp labeling (memory `fable2-decomp-labeling-progress`):** applied ~49 Ghidra labels — the
  whole localized-text/GUI/menu pipeline (fnv1 hash, text_db_*, GetText, DisplayMessageBox, etc.) +
  27 Lua-native cfuncs. New reusable tool `tools/ghidra_label/FindLuaNatives.java` (maps the scripting
  API). Full native-API enumeration continuable (multiple register helpers).
- **★ Black adult dog/hero skin (memory `fable2-adult-hero-black-skin` updated):** DOG also black =>
  it's the shared skin/fur texture-morph COMPOSITE, not hero-specific. Morph data loads fine; no GPU
  errors => the RTT->resolve->texture handoff yields black. Needs a RenderDoc/PIX frame capture to
  pinpoint (can't diagnose blind). Code area: rexglue-src/src/graphics/d3d12 render_target_cache /
  texture_cache resolve path.
- **Cheat menu expanded (memory `fable2-lua-native-menu`):** MMB menu now Ancestor-Chest / Give-Items+
  Gold (Halo-Spartan set, all items) / Warp (4 dests) / Skip-Story (childhood->Fairfax via
  SkipToLuciensStudy, adult) — all pcall-guarded, deployed.
- **Quest/interactable system RE'd (memory `fable2-quest-interactable-system`)** — the modding/node-tool
  foundation.



**Updated:** 2026-07-16 (later) — **★★★ DEV MODE UNLOCKED: empty `data\episodic_exec.txt` makes the
retail exe load the FULL dev debug menu at boot (log-verified).** See memory
`fable2-episodic-exec-devmode`. Read [../CLAUDE.md](../CLAUDE.md), then the block below.

## ▶▶ START HERE — 2026-07-16 (later): EPISODIC_EXEC DEV MODE + CHEST TEST STATE

**★★★ THE FIND:** boot path `sub_822F2868` probes `data\episodic_exec.txt` (UTF-16 string
0x820A17D8; the probe shows as `NtCreateFile FAILED: episodic_exec.txt` in every old boot log);
if present (`r26=1`) it loads **`scripts\startup\DebugMenu.txt`** = the whole dev cheat menu
(gold/items/spawns/freecam/`Gameflow:SetGameflowPosition` quest-skip incl. "New Beginning" =
post-childhood/Level Warping). **File CREATED (empty) → debugmenu.txt load VERIFIED in log 357.**
Menu opens on **MMB** (R3) with `--mnk_mode true`. DebugMenuLevelList.txt got a custom top entry
"* Teleport To Ancestor Chest" (`ScriptFunction.TeleportPlayerTo(ScriptFunction.GetEntityWithName(
'SpecialChest'):GetPosition())` — chest entity is literally named `SpecialChest` in ThagsCave).
May also un-gate the mystartup.lua dev hooks (SetSkipFrontEnd/SetInitialLevelName) — RETEST them
WITH the marker (without it they're confirmed dead; that's why 'skip-boot WORKS' was wrong).

**Chest-test state:** chest-unlock mod applied to gamescripts_r.bnk as the **v5 chesttest hook**
(`tools/lua_mod/myconsolehook0_chesttest.lua` — entitlement flips + auto: website-popup beacon ~150
ticks, auto-LoadLevel ThagsCave ~900 ticks, auto-teleport to SpecialChest when present). ⚠ The
per-frame hook is UNPROVEN on fresh new games (v2-v5 all silent on new game; only proven on
save-load) — the debug-menu route bypasses that whole problem. **After the chest test, re-apply the
plain hook: `python tools/lua_mod/apply_mod.py` (defaults to myconsolehook0.lua).**

**Debug lessons (don't relearn):** guest print → nowhere; `RunScript(missing)` → no NtCreateFile
log; `GUI.DisplayMessageBox(<unknown tag>)` → renders NOTHING (only real tags like
GUI_WEBSITE_GOLD). Host-visible signals = level-file opens in the log, or in-game teleports.
Lua teleport primitives: miscfunctions.lua `ScriptFunction.TeleportPlayerTo(pos)`,
`GetEntityWithName(name)`, SearchTools.StartNewSearch/FilterWithName/GetSearchResults.

## (older) ▶▶ START HERE — 2026-07-16: CHEST TEST FULLY STAGED — RUN IT

**State right now (verified this session):** the chest mod IS applied (`python tools/lua_mod/apply_mod.py
--check` from repo root → hook entry present) and **`mystartup_skip.lua` IS ACTIVE** (copied over
`mystartup.lua`; revert = `copy /Y mystartup.lua.orig_backup mystartup.lua` in
`Fable2Recomp/assets/game/data/scripts/Startup/`).

**★ RE ITEM SOLVED — post-childhood gameflow position = `ScriptEnum.DebugQC060` ("New Beginning").**
Bytecode-verified in gameflow.lua (via `script_index.py disasm gameflow`, full detail in memory
`fable2-lua-dev-startup-warp`): the main chain is QC005 Slumming It → QC010 Childhood → **QC060
NewBeginning (first ADULT position; level BowerLake, debug marker `QC060_Debug_PStart`)** → QC070 Thag →
QC075 Ravenscar → … → QC270 Final Battle. And it's ALREADY a debug-menu entry — **DebugMenu.txt
"Gameflow" section: `Gameflow:SetGameflowPosition(ScriptEnum.DebugQC060)` labeled "New Beginning"** —
so the PROPER-save childhood skip is: new game → open **MMB debug menu** → Gameflow → New Beginning.
No console needed. (`SetGameflowPosition(pos)` semantics, proto[26]: pos>current → fast-forward via
`SkipToPositionInGameflow`+`SkippingThroughGameflowFlag`; `CheckForGamePosition` then sets
`Stats.SetChapterProgress` on arrival. `Gameflow.Init` ends with native
`QuestTracker.SetStartupGameflowPosition(HeroEntity)` = the boot-warp position applier.)

**THE TEST (user plays; two routes):**
- **Fast route (default adult, active now):** launch → skip-boot lands in adult BowerLake (~30s) →
  MMB debug menu → Level Warping → **"Load Guild Cave (BL)"** (`Debug.LoadLevel('Albion',
  'Caves\\BowerLake\\ThagsCave', '')`) → walk to the Chamber of Fate chest → EXPECT: chest grants
  Mascot suit, Spartan set + sword + Master Chief title, Hero Doll, Lionhead tattoos, pink dye,
  expression book, gold — and NO "visit fable2.com" popup.
- **Proper-save route (real hero):** revert `mystartup.lua` first → new game → during childhood open
  MMB debug menu → Gameflow → **"New Beginning"** → gameflow fast-forwards to adult Bower Lake with a
  real save.
- Launch: `Fable2.exe --allow_game_relative_writes true --game_data_root <assets\game> --gpu_plugin
  xenos --mnk_mode true` (build `out/build/win-amd64-nightly`). MMB = debug menu; Space=A.
- If the chest misbehaves: `apply_mod.py --revert` restores the pristine bnk; RE notes in
  `docs/ANCESTORS_CHEST_RE.md` + memory `fable2-bnk-lua-mod-working`.

## (older) ▶▶ START HERE — 2026-07-15: CHEST UNLOCK DONE, NOW SKIP CHILDHOOD TO TEST IT

**What's done (deployed + verified):** the Chamber-of-Fate **ancestor's chest** (`SpecialChest` in
`scripts\quests\generictriggers.lua`, `CustomUpdate`) now unlocks with its authentic "Otherworldly"
contents — Mascot (chicken) suit, Spartan set + sword + Master Chief title, Hero Doll, Lionhead
face/torso tattoos, rare pink dye, expression book, gold sack — and the **"visit Fable II website"
popup (`GUI.DisplayMessageBox('GUI_WEBSITE_GOLD')`) no longer fires**. Mod = a plain-text Lua hook in
`data/gamescripts_r.bnk` (`tools/lua_mod/myconsolehook0.lua`, applied via `apply_mod.py`). Full RE +
gate polarity in memory **`fable2-bnk-lua-mod-working`** and **`docs/ANCESTORS_CHEST_RE.md`**.
- ★ Gate logic (bytecode-verified): an item is added when `Gameflow.Online<X> == 'NotGiven'` (unclaimed)
  AND its entitlement passes (`Stats.IsCollectorsEdition` for Spartan; `PlayerWebsiteUnlocks.IsItemUnlocked`
  for Mascot/book/doll/dye/tattoos; `XboxLive.GetYetToCollectUnlockedGold()>0` for gold). The hook flips
  the ENTITLEMENT checks to true and LEAVES the flags NotGiven. (An earlier attempt forcing the flags to
  'Given' was BACKWARDS — 'Given' means "already claimed" → skipped every item; only gold came through.)

**★ THE BLOCKER:** the chest can only be tested on a save where it hasn't been opened yet. The user has
NO such save (existing saves already claimed it → flags saved as 'Given'). So we need a **NEW GAME** — and
childhood is a ~24-min tutorial. **Next task = get a fresh adult hero to the Chamber of Fate fast.**

**Childhood-skip levers (pick per goal):**
1. **Fast boot-warp (improper hero, fine for chest test):** activate `data/scripts/Startup/mystartup_skip.lua`
   (`copy /Y mystartup_skip.lua mystartup.lua`; revert with `mystartup.lua.orig_backup`). Uses the game's
   own dev hooks (`SetSkipFrontEnd`/`SetInitialWorldName("Albion")`/`SetInitialLevelName("BowerLake")`) to
   boot straight to adult Bower Lake in ~30s. Caveat: spawns a DEFAULT adult (skips the hero-building
   transition) — OK for testing the chest, not a "real" save.
2. **In-game warp to the chest:** once adult, `Debug.LoadLevel('Albion', 'Caves\\BowerLake\\ThagsCave', '')`
   = the **Guild Cave (BL)** entry in `DebugMenuLevelList.txt` — the route to the Chamber of Fate. Or open
   the **debug menu with MMB** (`--mnk_mode true`, R3=MMB) and pick "Load Guild Cave (BL)".
3. **Proper-save childhood skip (OPEN research):** on a new game, `Gameflow:SetGameflowPosition(<post-
   childhood>)` gives a real save with a built hero. gameflow.lua (104KB) defines `SetGameflowPosition`;
   the exact post-childhood position value is NOT yet pinned down — that's the concrete next RE item.
   (Use `tools/lua_mod/script_index.py disasm gameflow` — Codex's chunk-aware helper handles the 104KB file;
   my luadis.py truncates it.)

**Recommended path to test the chest:** activate `mystartup_skip.lua` → boot to adult BowerLake →
MMB debug menu → "Load Guild Cave (BL)" → walk to the Chamber of Fate chest → confirm it fills + no popup.

**New tooling this session (parallel Codex agent):** `tools/lua_mod/script_index.py` (BNK list/search/
extract/disasm, handles 32KiB multi-block scripts), `apply_mod.py --check/--revert`,
`docs/ANCESTORS_CHEST_RE.md`, `docs/RE_NEXT.md`, `tools/lua_mod/catalog/items.csv` (authentic chest IDs
vs Pub-Games/Carbonated IDs). Launch: `Fable2.exe --allow_game_relative_writes true --game_data_root
<assets\game> --gpu_plugin xenos --mnk_mode true`.

---

## (older) ▶▶ START HERE — 2026-07-14 NIGHT: ADULT STAGE REACHED + AUDIO FIXED
★★★★★★★ The game now runs childhood → Fall cutscene → **childhood→adult transition → Bower Lake
(adult Sparrow with Hammer), rendering + playable, with working in-game audio.** Verified live (user
screenshot in BowerLake). Working build = `out/build/win-amd64-nightly`. Launch: `Fable2.exe
--allow_game_relative_writes true --game_data_root <assets\game> --gpu_plugin xenos` (★ `--gpu_plugin
xenos` REQUIRED or GPU doesn't load).

**Fixes landed THIS session (all in the working build):**
1. **In-game audio — SOLVED.** Bank/XMA audio (music/ambient/voice) was silent (only PCM intro + Bink/
   XMP cutscene audio played). Cause: Fable II allocates XMA INPUT buffers in the guest VIRTUAL heap
   (0x40000000-0x7FFFFFFF); `MmGetPhysicalAddress` returned 0 for that heap → NULL XMA input → silence.
   Fix (2 parts): `xboxkrnl_memory.cpp MmGetPhysicalAddress_entry` passes virtual-heap addrs through
   unchanged; `xma_context.cpp TranslateXmaInput()` reads input addrs ≥0x40000000 via TranslateVirtual.
   Memory: **`fable2-ingame-audio-silent`**. User confirmed audio works.
2. **Adult-transition crash — SOLVED.** After the Fall cutscene the Bowerlake load called unregistered
   `0x82DB2D98` (an adapter over-merged into `sub_82DB2D78`). Split the blob (3 fns) + registered its
   tail-call target `0x82DC74B0` in `Fable2_config.toml`, regen codegen + fix_dangling_gotos + FULL exe
   rebuild + re-staged runtime dll. Gone.
3. **Save-slots-full popup — SOLVED.** Was accumulated empty stub Hero saves in user_data_root
   (`C:\Users\Cornelio\Documents\Fable2`), not a storage bug. Cleared. Memory:
   **`fable2-save-slots-full-popup`** (also documents the `<exe>.toml [log.levels]` krnl/apu debug trick).
4. **Czech-text regression — SOLVED.** Culture-selection race picked cs-cz text even at US/English
   locale. Redirect in `xboxkrnl_io.cpp NtCreateFile` rewrites `language\<culture>\text\...` → `en-uk`.

**★ Childhood-skip for testing (WORKS):** `data/scripts/Startup/mystartup_skip.lua` uses Fable II's own
dev hooks (`SetSkipFrontEnd`/`SetInitialWorldName("Albion")`/`SetInitialLevelName("BowerLake")`) to boot
straight into the adult stage in ~30s (vs 24-min childhood replay). ACTIVATE:
`copy /Y mystartup_skip.lua mystartup.lua`; REVERT: `copy /Y mystartup.lua.orig_backup mystartup.lua`.
Full dev startup/warp/Debug.LoadLevel system documented in memory **`fable2-lua-dev-startup-warp`**.
(mystartup_gold.lua also staged but the "5 gold" is a QUEST COUNTER not currency, so Money.Modify is the
wrong lever — needs quest/gameflow, see the console memory.)

**OPEN FRONTIERS (next):**
- **Lua console/injector** (IN PROGRESS, the highest-value modding tool) — memory
  **`fable2-lua-console-injection`**. **★ 2026-07-15 decomp NAILED the loader** (Lua VM = Lua 5.1): the
  console primitive is **`luaL_loadfile` @0x82A25990** (C-level, reads `game:\` via the game VFS, bypasses
  stripped `io`) + protected run via **`lua_cpcall` @0x8219A840 / `luaB_pcall` @0x82A27E00**. **lua_State
  has NO simple global** → capture L by weak-overriding **`lhCreateLuaState` @0x82451B38** (cache its r3
  return). ~36 Lua VM funcs named in Ghidra (ghidra_out/lua_labels.tsv). REMAINING: a SAFE per-frame
  injection point on the script/GameThread, then wire the runtime hook (loadfile+pcall on a command file)
  + rebuild. (The old `Function_8222CED0` lead was the script-NAME setter — dead end, ignore.)
- **Adult hero BLACK SKIN** — memory **`fable2-adult-hero-black-skin`**. Confirmed NOT a skip artifact
  (black via natural playthrough too; child skin is fine). GPU skin texture-morph compositing renders
  black. Cosmetic. GPU-emulation dig (rexglue-src/src/graphics/d3d12).
- **Intermittent GPU TDR** at the NewBeginnings adult-intro render (`DEVICE_HUNG 0x887A0006`, edram_mode=4)
  — crashed once (log 307), NOT on later runs, so timing-sensitive not deterministic. GPU-emulation.
- **Progress confirmed deeper:** reached the **Chamber of Fate** (met Theresa, loading Garth/Hammer).
  The **fable2.com "Hero's Legacy" popup** is a dead-server tie-in MESSAGE BOX (no network call) — A
  dismisses it. The real ask: the **Hero's Legacy CHEST** holds ancestor gold + Collector's/online
  ("Otherworldly", codename "Carbonated") content that dead servers gate off. SOLUTION = grant the items
  directly via the Lua console (`Inventory.AddItemOfType(GetPlayerHero(), '<id>')`); item IDs already
  found (see memory **`fable2-lua-console-injection`** — the Carbonated* set + GoldBag + GiftToyRagDoll).
  This is the console's FIRST concrete job.
- **Framerate hitching** (user-reported): most likely D3D12 **shader/pipeline (PSO) compilation stutter**
  on entering new areas/effects (pipeline_cache DOES persist — "Created N graphics pipelines from
  storage" — but new shader+state combos still compile on demand) — fix = shader pre-warm / async
  compile. ALSO: the runtime still carries heavy diagnostic INFO logging (physaddr-diag / audio-diag /
  xma-*), ~37k log lines/40min = real game-thread I/O overhead → **STRIP the diag logging** (a quick win;
  keep the FIX logic in MmGetPhysicalAddress/xma_context, remove only the log blocks). The 512MB RAM
  limit is a SEPARATE deeper goal (higher LODs/entities), probably NOT the hitching cause — Fable II's own
  allocator won't use extra RAM.

**★ Build gotchas:** config change → `rexglue.exe -f codegen` (REXSDK=rexglue-sdk-nightly) + `python
tools/fix_dangling_gotos.py generated` + FULL exe rebuild (~3min) + **re-copy `rexruntime.dll` from
rexglue-src/out/win-amd64/Release** (exe rebuild re-stages a clean SDK runtime, wiping the audio fix).
The runtime currently carries diagnostic INFO logging (audio/xma/physaddr/audio-sync) — safe to keep or
strip when done (see the audio memory for the list).

**Older START-HERE (world-load/gameplay, 2026-07-14 day) below — superseded by the above but kept.**

**Updated:** 2026-07-13 (night) — **★★★★★★ ADVANCE-CRASH SOLVED. IN-GAME: character creation +
world loading reached.** Read [../CLAUDE.md](../CLAUDE.md), this section, then memory
**`fable2-advance-crash-FIXED-rtlgetlasterror`** (the full writeup). Sections below this one are
older history (kept for context) and their "parked upstream heap/threading" conclusion is SUPERSEDED.

## ▶▶ START HERE (next session) — 2026-07-14: GAME REACHES GAMEPLAY (past intro movie)
★★★★★ The game now boots → new game → gender → loading world → **INTO GAMEPLAY (past the intro
movie)**. The world-load 1GB crash AND the codegen-missed cascade behind it are fixed for that path.
Full story: memory **`fable2-worldload-1gb-alloc`**. Working build = `out/build/win-amd64-nightly`
(boots clean, verified). Launch: `Fable2.exe --allow_game_relative_writes true --game_data_root
<assets\game> --gpu_plugin xenos --mnk_mode true`. Kept fixes: `_vsnprintf` clamp + `sub_82452EC0`
skip (`src/WorldLoadAllocTrace.cpp`); the runtime interpreter for codegen-missed leaf/thunk/dispatch
functions (`rexglue-src/src/system/function_dispatcher.cpp`, incl. DISCOVERY MODE env
`FABLE2_DISCOVER_MISSING=1` = log all missing fns in one run); ~85 functions added to
`Fable2_config.toml` (data-ptr scan + discovered + blob-splits). Diagnostic cruft REMOVED (allocator
overrides, the `[Fable2 diag]` block). ▶ TO GRIND DEEPER: deeper gameplay may hit more codegen-missed
fns or real bugs — run with `FABLE2_DISCOVER_MISSING=1`, collect `[DISCOVER] ...` addresses, verify
each is a real fn start (prev word = terminator) + size (mind internal branches / over-merged blobs →
codegen "Overlapping boundaries" = split the blob), add to config, regen + fix_dangling_gotos + FULL
rebuild + restage runtime. NOTE: static pre-emption of the codegen tail is EXHAUSTED — the reliable
data-ptr scan is fully covered; code-address scan is unsafe (switch-case false positives). Only
discovery-mode (running) finds the rest. Details in the memory. (Older 1GB-crash writeup below.)

## (older) ▶▶ START HERE — WORLD-LOAD 1GB CRASH SOLVED
The world-load "1GB alloc" crash (prior frontier) is **root-caused + fixed** — see memory
**`fable2-worldload-1gb-alloc`** (full story). TL;DR: Havok Visual Debugger formats
"Listening on host[%s] port %d" via guest CRT `_output_l` which returns the BUFFER POINTER instead of
a char count → hkString grow does `new_size=ptr+1 (~1GB)`. Fixed with a `_vsnprintf` return-clamp
(`rex__vsnprintf_0` override in `Fable2Recomp/src/WorldLoadAllocTrace.cpp`). The game now advances
PAST world-load into a **codegen-COVERAGE cascade** — many small indirect-only functions codegen
missed. Fixes applied (all KEPT + in the working build): (1) 79 vtable/callback functions added to
`Fable2_config.toml` via `FindAllCodePointers` data-ptr scan (RELIABLE); (2) extended the runtime
interpreter (`function_dispatcher.cpp`: leaf getters/setters + vtable/global-fn-ptr dispatch thunks);
(3) `sub_82452EC0` SetFunction-skip (Havok error-dispatch that near-null-crashes). ★ WARNING learned:
the CODE-address scan (`FindCodeAddrTaken`, lis/addi in .text) is UNRELIABLE — it picks up switch/
jump-table case labels and SPLITS functions → boot FATAL "Jump target unresolved at bctr". Reverted.
Current working build boots clean, runs deep into world-load, next crash ≈ `0x822D5F40` (a codegen-
missed VMX helper reached via bctr — not safely bulk-addable; needs per-function verification or
Ghidra switch-recovery to blacklist jump-table targets). Codegen regen recipe + the data-vs-code scan
reliability details are in the memory. Build note: after any codegen regen do
`python tools/fix_dangling_gotos.py generated` + FULL rebuild (~6min); exe rebuild re-stages a clean
runtime so re-copy the instrumented `rexruntime.dll` after.

## ▶▶ (older) START HERE

**Where we are now:** logo → press A → **new-game screen → character gender select → LOADING THE
WORLD**, with fully-rendered Fable II UI along the way (incl. a working save dialog). The
"advance-past-logo crash" that blocked us for many sessions is **fixed** — it was two concrete,
fixable ReXGlue bugs (not the "parked upstream" dead-end previously assumed):

1. **`rex_RtlGetLastError` (0x82CC84C8) was config-truncated to 1 instruction** (never set r3). The
   cultures.txt reader uses `RtlSetLastError(0x10D2)`/`RtlGetLastError()==0x10D2` as a comment-skip
   sentinel; broken -> reader aborted on line 1 -> **zero cultures** -> NULL-deref in `sub_82382558`
   on advance. Fixed: `Fable2_config.toml` size `0x4->0x20` + split-off tail `sub_82CC84E8` (0x160),
   AND a belt-and-suspenders override `src/RtlGetLastErrorFix.cpp`. valid-cultures 0 -> 49.
2. **Stale `PAGE_NOACCESS` on committed virtual pages.** Extended `AccessViolationCallback`
   (`rexglue-src/src/system/xmemory.cpp`) to recover virtual-heap READ faults (was physical+write
   only). Also killed the boot layout-roulette.
3. **Codegen-missed leaf thunks** (`0x82267CC8`…): safe runtime interpreter in
   `rexglue-src/src/system/function_dispatcher.cpp` (`TryInterpretLeafThunk`).
4. **World-load crash `0x82DB1E90`**: was inside over-merged blob `sub_82DB1E88`; split in the config
   (0x38 -> 0x8 + 0x82DB1E90/0x18 + 0x82DB1EA8/0x18) and **regenerated** (nightly bin).

**CURRENT FRONTIER = world-load 1GB allocation (PRECISELY DIAGNOSED 2026-07-14).** See memory
**`fable2-worldload-1gb-alloc`** for the full writeup. The single FATAL is `BaseHeap::Alloc page count
too big for requested range`: the guest calls **NtAllocateVirtualMemory to COMMIT ~1.06GB**
(`size=0x42740000`, varies ±1 page/run, type=LARGE_PAGES|NOZERO|TOP_DOWN|COMMIT, base=0) which cannot
fit the ~1GB v40000000 heap (usable window 0x40000000-0x6FFFFFFF after the 0x0F000000 stack reserve).
CONFIRMED via ghidra + stack-DATA forensics (correct LR convention is `lr=*(back+8)`, NOT back-8):
the real path is engine `82E37528` (vtable dispatch) → a **byte VECTOR grow/realloc** (helpers
`82D58F64` capacity-double, `82D56F40` realloc-copy, `82D57030` grow-install, `82D56F10`; vector layout
`[obj+0]=buffer [obj+4]=count [obj+8]=capacity|flags`) → allocator `8221F258` → slab (0x83236xxx) →
`82CC74F8` → NtAllocateVirtualMemory. At the fault, the raw guest stack shows `0x42750000` amid a
CLUSTER of live heap pointers `0x4274A0C0`(×3)/`0x4274A0D9`/`0x427496A0`/`0x427521E0` — and
`0x42750000 - 0x4274A0C0 = 0x5F40` (~24KB, a sane size). So `0x42750000` is a **HEAP ADDRESS used as a
container capacity/count** (elem_size≈1 → alloc size == capacity). A `reserve/resize(count)` is called
with a POINTER as the count during world load. Recomp defect (Xenia's identical model would fail the
same 1GB alloc, so the game can't normally make this call). ★ ROOT LOCUS PINNED (weak-override the
allocators, read ctx.lr = exact caller, + disasm): chain is `sub_8221F258` ← `Function_822F2418` ←
**`Function_82D56F10`** (real entry 0x82D56F10; Ghidra mis-split it at 0x82D56F40). Disasm proves
`82D56F10(obj,count,elem)` does `mullw r4,r27(count),r30(elem)` = `size=count×elem`, with **count =
0x4273A0C1 (a tagged heap POINTER = 0x4273A0C0|1), elem = 1** (byte vector) — a POINTER passed as the
element count to a byte-vector reallocate. ▶ FINAL STEP: get `82D56F10`'s direct caller (the
`reserve/resize(n)` site passing n=pointer) — weak-override `82D56F10` with a full faithful reimpl
(alloc 82D55248 + memcpy 82CA9480 + free 82D552C8 + update obj[0]/obj[8]; RISK: 60 hot callers,
test+revert) OR a cdb hardware write-watchpoint; then fix the mis-recompiled instruction leaking a
pointer into the count. Reusable overrides in `src/WorldLoadAllocTrace.cpp` (weak-override
`sub_8221F258`+`sub_822F2418`, log allocs >=256MB; a weak-override catches BOTH direct+indirect calls,
unlike the dispatcher SetFunction trampoline which only catches indirect). NOTE: the 3856x
`AllocFixed already reserved` burst is BENIGN (guest finds a 5.25MB hole below 0x60000000, boots fine).
Full detail: memory `fable2-worldload-1gb-alloc`. Instrumentation added (keep, gated): BaseHeap error
addr logging (`xmemory.cpp`) + `[Fable2 diag]` block in `xboxkrnl_memory.cpp` (fires >=256MB: caller
LR, back+8 walk, raw stack dump). Rebuild runtime via PowerShell (build_runtime2.cmd broken under
`cmd /c` from bash); exe rebuild re-stages a clean runtime so re-copy the instrumented dll after.

**★ BUILD GOTCHA (bit me hard):** rebuilding the exe (`scratchpad_build.cmd`) **re-stages a
rexruntime.dll WITHOUT my rexglue-src/src changes** (the Fable2 build links the prebuilt SDK
runtime). So AFTER EVERY exe rebuild you MUST: `cmd /c rexglue-src\build_runtime2.cmd` then copy
`rexglue-src\out\win-amd64\Release\rexruntime.dll` -> `Fable2Recomp\out\build\win-amd64-nightly\`.
Verify: `Select-String -Path <staged dll> -Pattern "codegen-missed"` should match.

**Repro / run** (input is IGNORED without `--mnk_mode true`, or use a real Xbox controller which is
auto-detected): `Fable2.exe --allow_game_relative_writes true --game_data_root <assets\game>
--gpu_plugin xenos --mnk_mode true`, then press A/Space at the logo.

**Diagnostic cruft still in tree (REMOVE when done iterating):** `Fable2Recomp/src/CultureReaderTrace.cpp`
(+CMakeLists line) — spams `[CultRdr]`; `DiagnosticHooks.cpp` VirtualQuery/QueryProtect crash-dump
additions; `rexglue-src/src/kernel/xboxkrnl/xboxkrnl_io.cpp` `[ReadCult-Nt]` + `crt/file.cpp`
`[ReadCult]` INFO logs. KEEP: xmemory.cpp recovery, function_dispatcher.cpp interpreter, the config
edits, RtlGetLastErrorFix.cpp, xam_notify.cpp 0xB fix.

**Scratchpad scripts (this session):** `capture_newgame.ps1` (launch+mash A+dump crash),
`peek2_culture.ps1` (live culture-vector read), `all_ghidra_funcs.txt` + the DumpFuncsInRange diff
(Ghidra-vs-generated missing-function finder), `sample2.ps1` (ASLR thread-RIP sampler).

**Deferred:** ~7 other codegen-missed funcs from the diff (luaG_typeerror 0x82A2C300 etc.) — they
introduced "unresolved calls"; add case-by-case. The "max saves reached" save popup = XamContent
device reports maxed (press A to continue) — fix the save/storage layer eventually.

---

## (older) ★★★★★ FABLE II BOOTS TO PICTURE AND IS PLAYABLE.

## ⭐⭐⭐⭐⭐ THE GAME RUNS. Playable command:
```
Fable2.exe --allow_game_relative_writes true --game_data_root <assets\game> --gpu_plugin xenos --mnk_mode true
```
Renders video via the D3D12 xenos GPU plugin; keyboard/mouse input via the built-in MnK driver
(move=WASD, camera=mouse, A=Space B=LShift X=R Y=E, LB=Q RB=F, LT/RT=mouse buttons, D-pad=arrows,
Start=Esc Back=Tab). Runs reliably (crash is now rare). Full story in memory
`fable2-rex-atexit-truncation-cs-deadlock-fixed`. Remaining polish: XMA audio ("no bits"), a rare
physical-mem/MMIO crash (task #11), GPU `WriteRegister 28685`. See that memory + `docs/MODDING.md`
for the modding decomp direction (decomp lives in `Fable2Recomp/generated/*.cpp`).

---

## ⭐⭐⭐ 2026-07-13 (latest) — CPU-SIDE BOOT COMPLETE. CS deadlock + blob-indirect FATALs fixed.

See memory **`fable2-rex-atexit-truncation-cs-deadlock-fixed`** for the full story. Two fixes
took the boot from the CS deadlock all the way to a clean GPU-wait steady state:

1. **CS deadlock (uninitialized `0x834A5E3C`) root cause = another truncation.** `rex_atexit`
   (0x82CA9F20) was config-truncated to size 0x14 — its stack-restore epilogue (`addi r1,r1,96`)
   was split into bogus `sub_82CA9F34`, so it returned with guest **r1 0x60 low**, corrupting the
   `_cinit` ctor-loop cursor (r31) and aborting static init before the CS-init ctor ran. Pinned via
   cdb `gu`-delta (rex_atexit in=…490 out=…430). Fixed `rex_atexit` + **15 more silent truncations**
   (this class produces no dangling branch — the tail becomes a valid-looking standalone fn; detect
   via `stwu` w/o `addi` restore + adjacent orphan-epilogue). Boot then runs past module launch.

2. **Over-merged blobs → indirect-call FATALs.** 15 config entries were huge over-merges
   (0x2f6c–**1MB**) hiding leaf vtable methods; codegen only emits flow-reachable code, so indirect
   targets (`0x82208020`, `0x8222C568`, …) were never generated → `[FATAL] Call to invalid or
   unregistered function`. **Systematic fix:** parsed XEX `.pdata` (46180 funcs; 4195 in blobs) for
   exact boundaries + scanned `.rdata`/`.data` for vtable pointers into pdata-gaps → split 14 blobs
   into **5253 real entries** (config 882→6121, 0 overlaps). Tools in `tools/ghidra_label/`:
   `ParsePdataInBlobs.java`, `FindPointersIntoBlobs.java`, `DisasmRange.java`, `DumpFuncsInRange.java`.

**RESULT:** boot log 063 = no crash / no FATAL through _cinit → module launch → Vd ring-buffer setup;
all guest threads then park in **clean waits** (game thread at `rex_lhGameThreadInit +0x4f3`, workers
in `rex_WaitForSingleObjectEx`/`rex_RtlSleep`). This is the expected steady state with **no GPU
plugin** — CPU boot is done; the game waits forever for a GPU that never drains the ring buffer.
**Next frontier = GPU emulation.** TESTED `--gpu_plugin xenos`: the D3D12 plugin loads (AMD Radeon,
binding tier 3 / tiled tier 4), spawns GPU Commands + VSync threads, the command processor runs, and
the game advances FAR past the CPU-only stall (log 30→3900 lines) into GPU/physical-memory setup —
then **crashes**. Root cause (cdb-traced): a guest allocator can't find a **5.25MB contiguous VA
block** at 0x40000000 (fragmented by scattered 64KB reservations) → `BaseHeap::AllocFixed` "already
reserved" ×3856 → alloc returns NULL → null-deref in guest `sub_82BF58F8+0xa8a`. This is a
**rexruntime memory-model / VA-layout limitation** (prebuilt SDK — maintainer's domain), NOT a
recomp/config bug. Also did a config↔pdata boundary audit: fixed 10 more truncations; 11 off-hot-path
REX_FATALs remain (need case-by-case fixes that respect the intentional pdata divergences — see
memory). See memory `fable2-rex-atexit-truncation-cs-deadlock-fixed` for the full GPU + audit writeup.

Run: `Fable2.exe --allow_game_relative_writes true --game_data_root <assets/game>` (build:
`out/build/win-amd64-nightly`; rebuild via `scratchpad_build.cmd`; watchdog: `FABLE2_WATCHDOG=<sec>`
→ `watchdog_stacks.txt`). Remaining ~40 pre-existing unresolved-branch warnings are off the hot path.

---

## ⭐⭐ 2026-07-13 (late) — BOOT WALL BROKEN. Root cause = ~104 config-truncated functions.

See memory **`fable2-boot-wall-broken`** for the full story. TL;DR: `Fable2_config.toml [functions]`
declared ~104 functions too SHORT, so codegen truncated them mid-body. Keystone: `rex__mtinit`
(0x82CB3080) size 0x80→**0x140** — it was cut off right after `_mtinitlocks`, dropping its 2nd
`KeTlsAlloc` + ptd-index setup → CRT stdio/locale half-init → the `sprintf/_flsbuf` AV that blocked
the project. Fixed via `tools/fix_truncated_functions.py` (iterate until 0 unresolved branches) +
the `rex_KeTlsAlloc_thunk`/`_mtinit` config edits. **Now:** CRT inits natively, **BootTrace.cpp
DISABLED** (native `_beginthread`→guest XThread boots), GPU works (`--gpu_plugin xenos`, D3D12), the
game boots deep into `rex_lhGameThreadInit`. **Next blocker:** game thread deadlocks in
`RtlEnterCriticalSection` on uninitialized global CS `0x834A5E3C` (its static ctor `sub_832928E8`
never runs; `_cinit` skips it — needs a step debugger to reverse). Run:
`Fable2.exe --allow_game_relative_writes true --game_data_root <assets/game> --gpu_plugin xenos`.

---

## ⭐ 2026-07-13 UPDATE — nightly SDK is the new baseline; BUG A FIXED; CRT wall precisely localized

The ReXGlue `development` branch's threading rewrite (the maintainer's WIP) **is real and fixes the
guest-thread bug**. Migrated Fable2 to the **nightly prebuilt SDK** `0.8.1.68-dev.g8dadea6`.

- **SDK/build:** nightly staged at `rexglue-sdk-nightly/` (v0.8.0 `rexglue-sdk/` kept for rollback).
  Regenerated codegen with nightly `rexglue.exe`; build into `out/build/win-amd64-nightly` via
  `scratchpad/build_nightly.ps1`; run via `scratchpad/run_nightly.ps1`. Copy `rexgpu-xenos.dll`
  next to the exe (not auto-staged). `dev` source cloned at `rexglue-src/` for reference.
- **BUG A (guest threads never execute) = FIXED.** `XThread::Execute` now spawns a real host thread +
  fiber and calls the recompiled entry directly. Confirmed: `Fable2 Game Thread (thid 5)` runs
  `rex_lhGameThreadInit`; Main XThread runs guest `xstart`.
- **Codegen gap fixed (reusable):** `0x82CB2CB0` (a 4-byte `b 0x832b9cd4`->KeTlsAlloc tail-thunk) is
  *only address-taken* (indirect `bctrl` via a cached ptr) so codegen missed it. Declared it in
  `Fable2_config.toml [functions]` (`rex_KeTlsAlloc_thunk`) and shrank the stale `0x82CB2C7C` entry
  (0x4C→0x8) that was swallowing it. **Lesson: address-taken-only funcs need manual `[functions]`.**
- **CRT keystone STILL OPEN (was "BUG B"):** guest `rex__mtinit` runs, sets the getter TLS index
  (slot 0) but never the ptd index (`DAT_832ef270` stays 0xFFFFFFFF) → CRT stage-2 (locale/stdio)
  skipped → engine `LhDebugLogInit`→`rex_sprintf_0`→`rex__flsbuf` **AV** (same wall as v0.8.0).
  Localized: `_mtinit` bails in the `_mtinitlocks`/indirect-dispatch region *before* its 2nd
  (indirect) KeTlsAlloc; runtime `KeTlsSetValue(0)` succeeds; registering the thunk + forcing the
  per-lock wrapper direct did NOT complete it. **This is the maintainer's core CRT-init issue —
  report the repro (getter=0/ptd=-1) upstream, or work decomp/modding (unblocked).**
- BootTrace (XHostThread + ptd repair) re-enabled = furthest-advancing baseline. See memory
  `rexglue-dev-branch-fixes-boot`.

---

**Prior update:** 2026-07-12 (late — deep boot-runtime session + labeler built).

## ▶ START HERE TOMORROW — the plan is: FIX REXGLUE (the runtime)

Boot is blocked on **two ReXGlue-runtime bugs** (not our recomp/config — those are fine). Both are
precisely characterized with repros (2026-07-12 session). The goal for tomorrow is to fix the
ReXGlue runtime itself.

**PREREQUISITE — ✅ SOURCE FOUND: `https://github.com/rexglue/rexglue-sdk`** (branches
`development` = ahead, use this; `main` = behind). It has the full runtime source (`src/`,
`include/rex/`, C/C++, CMake-buildable) — not just the prebuilt `rexglue-sdk/win-amd64/` we've been
linking (v0.8.0 — confirm which tag/commit it matches before swapping). Tomorrow's step 1: **clone it** (e.g. to
`D:\Documents\Fable2RE\rexglue-src`), build it, and locate the two bug sites:
- BUG A code: `XThread::Execute` / guest-thread start path (search `src/` for `XThread`,
  `guest_thread`, `Execute`, `ExCreateThread`, `HostToGuestFunction`) — compare to UnleashedRecomp
  `cpu/guest_thread.cpp` (the working pattern, below).
- BUG B code: indirect-call dispatch — search for `REX_CALL_INDIRECT_FUNC`, the function
  table / perfect-hash lookup, and the "invalid-function trap". This is where `bctrl` targets
  (cached fn-ptrs / CRT static-init table) fail to dispatch.
Then either fix + rebuild the runtime and relink Fable2 against the local build (point
`CMAKE_PREFIX_PATH` / `REXSDK` at your build output), or file precise issues/PRs upstream. NOTE the
dev branch may have API changes vs the v0.8.0 SDK we build against — check the delta before swapping.

**BUG A — guest threads never execute.** `rex_CreateThread` and a direct guest
`XThread(...,guest_thread=true)->Create()` return a valid handle but never run the recompiled entry.
Every working thread in our logs is `<host>`. Reference: UnleashedRecomp `cpu/guest_thread.cpp` shows
the correct pattern (spawn a host `std::thread` that builds a `GuestThreadContext` — r13→PCR block
`[PCR 0xAB0|TLS 0x100|TEB 0x2E0|stack 0x40000]`, r1→stack top, publish to a per-thread `g_ppcContext`
— then call the recompiled start fn with arg in r3). Fix ReXGlue's `XThread::Execute`/guest-thread
path to actually enter guest code. (We work around this in `src/BootTrace.cpp` via `XHostThread`.)

**BUG B — indirect-call (bctrl / `REX_CALL_INDIRECT_FUNC`) dispatch fails for CRT-init targets.**
The recompiled guest CRT init runs but doesn't initialize stdio/locale, so `sprintf`→`_output`→
`rex__flsbuf` faults **process-wide** (crashes on the MAIN thread too, even for `sprintf("D:\")`).
PROVEN: `rex__mtinit`'s 2nd `KeTlsAlloc` (via a cached fn-ptr, indirect `bctrl`) never set the ptd
TLS index `DAT_832ef270`, while the *direct* `bl KeTlsAlloc` worked. `rex__cinit` runs its
static-init table via the same `REX_CALL_INDIRECT_FUNC`. Re-running mtinit/ioinit/cinit from a hook:
they return OK but sprintf still crashes → the init executes but its indirect calls don't dispatch to
the real init routines. `REX_CALL_INDIRECT_FUNC` is defined in the opaque runtime (SDK `context.h`
only references it; notes an "invalid-function trap"). **Fix this in the ReXGlue runtime/codegen** —
it's likely the single keystone (CRT locale/stdio → whole boot).

**IMPORTANT — do NOT redo these dead ends:** (1) the CRT is ALREADY recompiled (rex__* have generated
bodies; `rexruntime.lib` does not override them — verified via dumpbin). Editing `Fable2_config.toml`
to "remove CRT redirects" + regen changes NOTHING. (2) Skipping individual crashes (log-init, etc.)
is whack-a-mole. (3) Bridging globals from `src/` (like the working ptd-index repair) can't reach the
sprintf/locale state. See memory `fable2-boot-thread-bug` for full detail + evidence paths
(`ghidra_out/*.txt`).

### Parallel tracks that are NOT blocked (do these if not touching ReXGlue)
1. ✅ **AssetBrowser VERIFIED on real game data**: headless autopilot loaded Bowerstone Market and
   captured a correct 3D render (`--autoroot=<data> --autoload=bwsmarket --autoshot=<png> --autoexit`).
   Built RelWithDebInfo+PDB. NOTE: level queries match `.engine_level` paths — "Bowerstone" =
   `bwsmarket`/`bwsslums`; 78 levels in `levels.bnk`.
2. **Function labeler** (`tools/ghidra_label/`, self-contained, no API key): `LabelDump.java` +
   `LabelApply.java`. 21 boot/CRT labels applied (`ghidra_out/labels_in.tsv`). See memory
   `fable2-ghidra-labeler` and the multi-hour plan below.
3. **Modding**: AssetBrowser + the Xenia patch library (`4D5307F1` patch.toml — 60fps/res/unlocks;
   the just-harry Xenia femtofork has graphics fixes: black-texture-at-adulthood, etc.).

### ⏱ AUTONOMOUS DECOMP + LABELING PLAN (hours of unattended work — high value)
Goal: systematically name the ~42k `sub_`/`Function_` functions, subsystem by subsystem, so the
Ghidra project becomes a readable map for modding/decomp (and to spot the mod/extend hooks). Loop:
`LabelDump` a batch → read the worklist (decomp + strings + callers/callees) → write
`labels_in.tsv` lines (`addr⇥name⇥comment`) → `LabelApply`. Commands in `tools/ghidra_label/README.md`.
Everything persists in the Ghidra project. Work through these batches (each = one or more LabelDump
runs, then label, then apply):
1. **Lua VM** (seeded: `luaV_gettable`@82227EA0, `luaG_typeerror`@82A2C300, `luaG_runerror`@82A2C520;
   allocator `rex_lhLuaAllocator`@82227748). Dump callers/callees of these + `auto:strings` hits with
   Lua messages ("attempt to", "'for'", "stack overflow" — see `ghidra_out/lua_strings_xrefs.txt`).
   Name the VM core (luaD_/luaH_/luaS_/luaT_/lua_* API, lparser/llex, lgc).
2. **`auto:calls:N`** — the most-called unnamed functions are high-leverage utility/core (allocator,
   string table, refcount, containers). Label these first; they clarify everything downstream.
3. **Resource/asset loaders** (BNK, .lev/.engine_level, terrain/.ehf, textures/Lh, models/MDL) —
   cross-ref names/offsets with the AssetBrowser decoders (`Fable2AssetBrowser/source/src/`) which
   already understand these formats; that's a Rosetta stone for naming the guest loaders.
4. **Item/appearance, quest Lua, entity/GDB** — the modding targets (see ROADMAP/MODDING).
5. **Networking/session** (for MP later).
Keep appending to `ghidra_out/labels_in.tsv` (it's the running label set). Re-run `LabelApply` after
each batch. Tip: `auto:strings` is the best signal for nameable functions; feed specific address
lists (a file of hex addrs) to target a subsystem. NOTE: `analyzeHeadless` prints "exit code 255"
even on success — trust the script's own `LabelDump/LabelApply: ...` summary line.

## One-paragraph state

We recompiled Fable II and got it booting, hit a deep Lua-VM crash, and discovered the real cause:
we'd been recompiling the disc's **gold** executable, but the recomp targets **GOTY TU1**. Applying
the TU1 patch (`xextool -p default.xexp`) produced `default_tu1.xex`; recompiling *that* made the Lua
crash vanish. The game now runs to a later **CRT-exit / threading crash**, which the ReXGlue
maintainers have flagged as a known **core heap/threading** issue they're actively rewriting — so
the boot track is **parked**. Focus pivoted to **decompilation** (Ghidra analysis of the TU1 exe is
**done**) and **modding** (AssetBrowser building), which advance the long-term goal (moddable,
eventually multiplayer Fable II) without waiting on upstream.

## What's true right now

- ✅ `default_tu1.xex` exists and is the exe in `Fable2Recomp/assets/game/default.xex`
  (gold backup: `assets/game/default.xex.gold_backup`).
- ✅ Recomp builds; `Fable2.exe` boots deep. Current crash: `sub_832B7688 ← rex_doexit ← xstart`
  (guest CRT teardown; main thread returns without spawning game threads = the upstream threading
  issue). `rex_exit`/`rex_abort` are NOT called.
- ✅ **Ghidra analysis of `default_tu1.xex` COMPLETE** — project `Fable2_TU1` in `ghidra_proj/`.
  XEXLoaderWV decrypted it and loaded all PE sections (.text @ 0x82170000, Lionhead .lhmem/.lhtrc,
  BINK video). GhidraMCP (249 RE tools) installed in Ghidra.
- ✅ **Fable2AssetBrowser BUILT** — `Fable2AssetBrowser/source/build/Fable_2_Asset_Browser.exe`
  (~38 MB, valid PE, no external DLLs). Deps auto-fetched (ImGui/zlib/stb/miniaudio/DirectXMath).
  The modding toolchain (model/texture/anim/level/terrain/Lua/audio decode + BnkWriter injection).

## Background jobs (finished)
- Ghidra headless analyze — **finished** (`ghidra_analyze.log`; project `Fable2_TU1`).
- AssetBrowser build — **finished, exit 0** (`assetbrowser_build.log`).

## Immediate next steps (pick a track)

### Decomp track (active, highest value)
1. Launch Ghidra GUI on `ghidra_proj` / `Fable2_TU1`, enable GhidraMCP, start its server (port 8089).
   Then drive RE by hitting the HTTP API directly (curl) — decompile, list, rename, search — or via
   the MCP bridge (`REPlugins/GhidraMCP/bridge_mcp_ghidra.py`). Headless `-postScript` also works.
2. Seed known labels from the recomp: `sub_82227EA0` = Lua `luaV_gettable`, `sub_82A2C300` =
   `luaG_typeerror`, `sub_82A2C520` = `luaG_runerror`; heap/Lua-allocator = `rex_lhHeapRealloc`
   (hooked in `Fable2Recomp/src/heap.cpp`). Use these to orient in the Lua VM + resource systems.
3. Begin decompiling the subsystems we want to mod/extend: item/appearance, quest Lua, level/terrain
   loaders, and (for MP) networking/session. See ROADMAP.

### Modding track (parallel)
1. ✅ AssetBrowser is built — run it and browse `Fable2Recomp/assets/game/data` to confirm the
   decoders work on real data.
2. Prove an asset round-trip (extract a model/texture → edit → repack via BnkWriter, or loose-file
   override). The recomp-side loose-file overlay is scaffolded in `Fable2Recomp/src/ModSupport.cpp`.
3. Target the depth in [MODDING_REFERENCE.md](MODDING_REFERENCE.md): grow AssetBrowser toward a
   CK-style editor (records/levels/terrain/quests) + a native modding API via the recomp/decomp.

### Boot track — chronological investigation log (2026-07-12)
> ⚠️ The subsections below are a CHRONOLOGICAL log of how the diagnosis evolved. Some intermediate
> framing (e.g. "split CRT / redirected to runtime", "recompile the guest CRT") was later DISPROVEN.
> The corrected, authoritative conclusion + the tomorrow plan are in **▶ START HERE TOMORROW** above.
> Net: two ReXGlue runtime bugs (guest threads don't execute; indirect-call dispatch fails in CRT
> init). Our config/recomp are fine; the CRT is already recompiled.

#### (a) game thread now runs via local workaround
- **Root cause found** (Ghidra decomp + `src/BootTrace.cpp` instrumentation, cross-checked vs the
  co-located **Xenia** source — ReXGlue's `XThread` is literally Xenia's, "Adapted for ReXGlue").
  Guest `main` (0x822EA1C0) does `_beginthread(0x822EA228 /*game thread*/, 0x40000)` then
  `WaitForSingleObject(h, INFINITE)`. The real bug: **ReXGlue's prebuilt runtime can't execute
  GUEST threads** — `rex_CreateThread` *and* a direct guest `XThread::Create()` return a valid
  handle but never run the recompiled entry (every working thread in our logs is `<host>`; no guest
  thread has ever run). The "wait returns immediately" is a *symptom*: the guest thread's host
  carrier starts, `Execute()` can't reach guest code so it returns at once, the OS thread exits and
  its handle signals. So main's wait fell through → main returned → `rex_doexit` → AV in
  `sub_832B7688` (CRT cleanup). That was the whole crash.
- **Note on XenonRecomp/**: it's the *translator only* (README: "does not provide a runtime… Making
  the game work is your responsibility"), so it can't help the threading bug directly. The useful
  reference is **xenia/** (full checkout) — ReXGlue's runtime is adapted from it, so
  `xenia/src/xenia/kernel/{xthread.cc,xboxkrnl/xboxkrnl_threading.cc,xobject.cc}` is the ground
  truth for correct thread create/resume/wait, and `XenonRecomp/XenonUtils/xbox/xboxkrnl_table.inc`
  gives kernel ordinal→name for Ghidra labeling.
- **Workaround in `src/BootTrace.cpp`** (weak overrides of `sub_822EA1C0` main +
  `sub_82CA9CD0` _beginthread): spawn the game thread as an exported `rex::system::XHostThread`
  calling the guest CRT shim `sub_82CA9C50(ptd)`, and park main forever (hardware-faithful; on
  hardware the wait never returns). Result: `'Fable2 Game Thread'` executes the engine bootstrap
  (`rex_lhGameThreadInit`) — process runs with ~38 threads / 300+ MB WS instead of insta-crashing.
- **Report both runtime bugs upstream** (ReXGlue Discord/GitHub) — the workaround is local and
  should be retired when the maintainer's core threading rewrite lands.
- **Next blocker localized (2026-07-12): the guest CRT thread-startup shim `sub_82CA9C50`.**
  With the workaround the game thread runs but *stalls inside the CRT shim* before reaching the
  engine (never gets to `rex_lhGameThreadInit`'s 2nd call; process stays alive, no crash).
  Localization: temporarily bypassing the shim and calling `rex_lhGameThreadInit` directly makes the
  engine advance — it reaches its log-init `sub_82B3DB90` (which opens **`lhdebug.log`**, mode `"w"`,
  resolved path `assets/game/lhdebug.log`) — then AVs in the CRT
  (`rex__flsbuf` ← `rex_sprintf_0` ← `sub_82B3DB90` ← `rex_lhGameThreadInit`) because the shim never
  installed this thread's per-thread CRT state. So *with* the shim it hangs in the shim; *without*
  it the engine crashes in the CRT for lack of per-thread state. Both are downstream of the same
  upstream guest-thread gap (the CRT shim was written for a real guest thread).
- **Attempted fix (didn't break through):** installed this thread's CRT ptd on the game thread
  (`sub_82CB2D10` = `_getptd`, returns the static default ptd `0x832B9CE4`) then called the entry
  directly. Engine still AVs in the same guest CRT helper during log-init (`sub_82B3DB90`), *before*
  the fopen. The log path resolves to `D:\lhdebug.log` (format base `0x820f97ec` = `"D:\"`, mode
  `"w"`). So the default ptd is not the missing piece — the fault is deeper in the guest CRT/engine
  init running on a host-carried thread. This is genuinely coupled to the upstream guest-thread
  environment; recommend engaging the maintainer with `docs/UPSTREAM_BUG_guest_threads.md` rather
  than grinding CRT internals locally.
- **`lhdebug.log` as a boot-visibility lever is blocked for now** — the game thread doesn't reach a
  successful open+flush (historical file is 0 bytes). Revisit once the shim/CRT env is solved.
- CRT thread shim decomp: `ghidra_out/crt_shim.txt` (shim `sub_82CA9C50` → `_getptd`
  `sub_82CB2D10` → `Function_82CA9BA8` calls entry via `(*(ptd+0x54))(ptd+0x58)`).

### Boot track — DEEPER DIAGNOSIS + PARTIAL FIX (2026-07-12, later session)
- **The game thread wasn't hanging — it was self-terminating.** Bisected the CRT shim
  (`ghidra_out/shim_disasm.txt`): the ptd-getter (`sub_82CB2CB8` `bctrl` callback) returns 0, and
  create-ptd (`sub_82CB2D68` = `KeTlsSetValue`) also returns 0, so the shim hits `ExTerminateThread`
  (`Function_82CC6F80`). No hang, no crash — the thread quietly dies, main stays parked.
- **Root cause: the CRT per-thread-data TLS index `DAT_832ef270` = `0xFFFFFFFF` (never allocated).**
  Every `_getptd` does `KeTlsGetValue(0xFFFFFFFF)` → 0 and create does `KeTlsSetValue(0xFFFFFFFF,…)`
  → fail. CRT init `Function_82CB3080` (called by `xstart`, `ghidra_out/range_dump.txt`) allocates
  the *first* TLS index (`DAT_832ef274`, getter callback — via a **direct** `bl KeTlsAlloc`, works)
  but its *second* alloc for `DAT_832ef270` is an **indirect** `bctrl` through a cached fn-ptr
  (`DAT_83336abc`) and does not take — so stage 2 is skipped. KeTls itself works (probed:
  Alloc→1, Set ret=1, Get round-trips). Strong hypothesis: **ReXGlue indirect calls (`bctrl`) to
  cached kernel-import thunks don't resolve** where direct `bl` does.
- **PARTIAL FIX applied in `src/BootTrace.cpp`**: `EnsureCrtPtdTlsIndex()` allocates the ptd index
  via `KeTlsAlloc()` and writes it to `DAT_832ef270` before spawning the game thread. Result: the
  shim's create-ptd now SUCCEEDS, the game thread stops self-terminating and runs
  `rex_lhGameThreadInit` **through the real CRT shim** (crash stack now includes `sub_82CA9BA8`, the
  shim's entry-caller — provably further than before).
- **Remaining crash**: engine log-init `sub_82B3DB90` → CRT `sprintf`/`_flsbuf` AV. CRT stage-2
  init (locale/stdio globals, main-thread ptd) was skipped along with the TLS index, so the CRT
  formatter still isn't fully set up. Next: either fix the indirect-`bctrl`-to-kernel-thunk issue so
  `Function_82CB3080` completes on the main thread (proper root fix — likely a ReXGlue codegen/
  runtime concern), or hand-run stage 2's remaining setup. Evidence: `ghidra_out/{shim_disasm,
  ptd_init,range_dump,gate,lockinit}.txt`.
- Committed `src/BootTrace.cpp` keeps the ptd-index repair (advances boot into engine CRT-init);
  it AVs at the CRT-stdio wall rather than parking cleanly — this is forward progress, not a
  regression.
- **Tooling note**: `auto-re-agent/` is an LLM-driven Ghidra RE automator (Python, needs the
  separate `ghidra-ai-bridge` CLI + an API key; expects our GhidraMCP-on-8089 to be wrapped as that
  CLI). Useful for batch-naming many `sub_` functions but needs setup; not wired up yet.
- Committed `src/BootTrace.cpp` = the clean, non-crashing baseline (game thread via CRT shim, main
  parked). Diagnostic experiments (fopen trace, thread-name skip, direct-entry call) were reverted;
  findings recorded here. Ghidra decomp evidence in `ghidra_out/` (`engine_bootstrap_decomp.txt`,
  `plhdebug.txt`, `boot_thread_decomp.txt`).

## Native renderer checkpoint — 2026-08-02

Read [SESSION_SNAPSHOT_2026-08-02_NATIVE_RENDERER.md](SESSION_SNAPSHOT_2026-08-02_NATIVE_RENDERER.md)
first. The native path still presents frontend/loading/UI and the final compositor, but the loaded
Hero001 world is flat blue-gray. The latest run completed cleanly as `Fable2_1459.log`; no Fable
process remains.

Critical shader-probe correction: late-world `gameplay draw ... ps=` values are translator cache keys,
while `REXGPU_NATIVE_FORCE_PIXEL_COLOR_TARGET` compares guest microcode hashes. Exact mappings are:

- `18F21758F4DC7D83` → `70E7786A87CAEFF5`
- `62BC938CB35BCC47` → `4E0825A6D3B60F88`
- `802C36EB2C3261DC` → `A820293DEE9C9A2D`
- `CFCC25A84593468B` → `CCA25F8031D8EADD`
- `2938CD4379D4F909` → `DB9F19BA0E43675E`

The earlier `TARGET=world` and `TARGET=18F...` runs did not force the intended steady-world shaders
and must not be used as evidence against their coverage. The next valid A/B starts with guest target
`70E7786A87CAEFF5`.

The 1D texture resource correction must remain: it made loading screens richly detailed. Existing
measurements still stand: interpolator/full-color probes prove coverage and present; exposure=1,
predicate-true, and always-pass-depth do not fix the world; ×16 output scaling raises HDR from
approximately `max=1.0605/mean=0.0811` to `16.97/1.297`; the final compositor binds live varied
`0x19C67000` HDR. This is an under-scaled/material-output frontier, not a blank compositor path.

Screenshot caveat: `winshot_confirm.ps1` activates the game window by default, which can itself make
title/menu/loading visible. Treat it as an active presentation intervention; use `-NoActivate` only
when Fable is already foreground.

## Gotchas to remember

## Native renderer A/B continuation - 2026-08-03

The valid-focus probe runs changed the frontier from predicate state to draw acceptance/visibility:

- Fresh native build/stage is SHA-256 `D0DB6590EF64144CE0026F9874DD7876AD47EFD0C07A9AA75090B210DC9D17F0`.
- `Fable2_1460.log` / `Fable2_1461.log`: guest target `70E7786A87CAEFF5` mapped to translator key
  `18F21758F4DC7D83`; the targeted solid-magenta probe was active on repeated material draws, but the
  client remained flat blue-gray (gameplay-region mean approximately `(55,69,89)`).
- `Fable2_1462.log`: `TARGET=all` turned the complete client frame solid magenta. This proves the forced
  pixel export survives native RTV, resolve, compositor, and present.
- `Fable2_1464.log`: `REXGPU_NATIVE_FORCE_PREDICATE_TRUE=1` initialized successfully, but the loaded-world
  image remained unchanged. Predicate false is not the sole cause.
- `Fable2_1465.log`: guest target `4E0825A6D3B60F88` produced a real magenta geometry patch in the upper-left,
  proving a targeted world shader can reach visible pixels; the rest of the world stayed blue-gray.
- `Fable2_1466.log`: guest target `CCA25F8031D8EADD` was active across 92 color-writing material draws but
  produced no visible change. The main-world gate is still ordinary kill/depth/raster/vertex acceptance
  or draw visibility, not the compositor path.

Next: instrument one representative main-world draw through vertex output, depth-test outcome, raster
coverage, and color-write acceptance, preserving the existing default-off diagnostics.
- Recompile **TU1, not gold**.
- Run `tools/fix_dangling_gotos.py generated` after **every** codegen.
- Ghidra extension version must match (edited to 12.1; they shipped 12.1.2).
- Don't hand-edit generated code; use weak overrides in `src/`.
- Don't add band-aids that mask upstream bugs (the `luaG_typeerror` stub was one — removed).
## 2026-07-26 native renderer: first real textured frame

- `Fable2_692.log` is the current renderer proof. Sequence 104 snapshots and uploads the game's
  three real linear `k_8` YUV planes (1280x720 Y, two 640x360 chroma; 1,474,560 bytes total),
  binds R8 UNORM/SNORM views plus original fetch/sign constants, and executes the translated
  Xenos YUV shader.
- `REXGPU_NATIVE_EAGER_TEXTURE_PROOF=1` is a disabled-by-default diagnostic used because this
  static frontend run emits no later swap. It submits the retained draw and writes
  `rexgpu_native_texture_proof.bmp`.
- The first submitted attempt was uniformly black because the translated shader's final
  `color_exp_bias` multiplier was left zero. Neutral `(1,1,1,1)` fixes it. Final readback changes
  all 921,600 pixels (`first=FE7F87E6`, `hash=EA1BD6D13ECFB239`) and shows the near-white
  intro-video frame. There are no D3D12 validation or device-loss errors.
- Focused PM4 tests pass 383 assertions in 22 cases. Release hashes are
  `rexgpu-native.dll` `4A786011F16955728D29EDAD9640457F221DC46FE097466D01C445E99ED47FAA`
  and `rexruntime.dll` `AE4D560983F22BEC92D4C18C3AD33CA3D3F0010FBD22ED51126AFDE47682FB00`.
  Normal nightly DLLs are restored; no Fable process remains. Ghidra is still free.

## 2026-07-27 native renderer: resolved-frame presentation and flash fix

- Native D3D12 now translates, draws, resolves, copies, and retires the real planar-video frame
  continuously on the AMD RX 9060 XT. The final clean, visible/topmost runtime smoke reached native
  resolve 256 in 13 seconds with no queue timeout, device removal, or GPU-loss message
  (`Fable2_928.log`).
- The apparent AMD queue hang was narrowed to presentation. DXGI was returning
  `DXGI_STATUS_OCCLUDED` while the automated launch had no foreground desktop, but the presenter
  kept recording and presenting frames anyway. This filled the direct queue and manifested as
  screen flashes followed by a stalled fence.
- `D3D12Presenter` now starts each swap chain in an occluded state, probes visibility with
  `DXGI_PRESENT_TEST`, and submits no paint work until DXGI says the window is visible. If a real
  present later reports occlusion, it returns to the probe state. This is renderer-independent and
  preserves the normal shared D3D12 queue when visible.
- The automatic R10G10B10A2 UAV diagnostic clear remains disabled by default because it caused the
  visible flashes and could wedge RDNA 4. It is available only with
  `REXGPU_NATIVE_DIAGNOSTIC_CLEAR=1`. Textureless boot/resolve-companion draws remain filtered
  before PSO creation because their unused legacy DXBC pipelines also poison RDNA 4.
- Temporary draw, copy, paint, queue-splitting, and DXGI-present diagnostic switches and queue
  self-tests were removed. The normal runtime needs only `REXGPU_NATIVE_RESOLVE_HEAP=1` with the
  native D3D12 backend arguments.
- Release verification: `pm4_parser_tests` passes 482 assertions in 29 cases; `native_plugin_smoke`
  passes plugin load, factory, backend selection, and headless setup.
- Staged SHA-256:
  - `rexruntime.dll`: `ADF03C618D6A36065CFC7837B9734A764DFE591BF868B88B467F1CB053C0EB47`
  - `rexgpu-native.dll`: `79D351D127307D7CFAC4692C3E8BAE8E51CB0B5A6B3E7A303C7E643F0448928F`

## 2026-07-27 native renderer: gameplay tessellation, textures, and depth round trip

- Adaptive Xenos quad tessellation now runs end to end through fixed VS/HS stages and the
  translated guest VS as a D3D12 domain shader. `Fable2_961.log` accepts and submits mode-2
  terrain patches with all 8 referenced textures retained.
- Live gameplay texture coverage now includes tiled R8, R8G8, packed 1-5-5-5 (with guest
  red/blue repacking), DXT3A alpha expansion, and six-face cube textures. The only remaining
  unsupported-layout inventory entry is the 1x1 bootstrap sentinel at address `10000000`.
- The proof renderer now applies normalized guest Z enable/write/compare state with a real D32
  attachment. Fable's live convention is reversed Z (`GREATER_EQUAL`); clears select 0 for that
  convention instead of assuming forward Z.
- Depth resolves no longer publish the color attachment. They copy the typeless R32 depth surface
  into shader-readable resolved resources and preserve depth EDRAM generations.
- Depth persistence is now bidirectional. A resolved prepass is rebound before color draws that
  test against it, and color-pass Z writes are copied back to the persistent depth target.
  `Fable2_961.log` proves both operations in one run:
  - stored color-pass depth writes at base 720;
  - rebound persistent depth base 224 for a later color pass;
  - captured the 1120x720 base-1008 depth resolve as R32.
- Validation remained clean: 497 assertions in 29 PM4 tests, native plugin smoke passed, and the
  live run had no D3D12 rejection, device removal, allocation failure, or queue timeout.
- Current staged `rexgpu-native.dll` SHA-256:
  `F4D79C52206E3DB86DE78158FD6F9F5A3EEE820AE5ED02598B0ACC2B17FF4AD6`.

## 2026-07-29 frame-cadence investigation

- The top-right FPS counter was measuring host window presents, not unique frames produced by the
  guest. An always-registered but idle achievement-toast dialog kept requesting UI redraws, so the
  presenter repeatedly submitted the same guest image at the desktop refresh rate. This explained
  readings around 200 FPS while motion still looked choppy.
- `ImGuiDialog` now exposes `NeedsContinuousDraw()`. The achievement toast requests continuous
  drawing only while its queue is non-empty, and the F3 debug overlay is refreshed by guest frames
  instead of running its own present loop. Interactive dialogs retain the old continuous behavior.
- Live D3D12/native validation after the fix:
  - title screen without F3: external counter 56 FPS (previously roughly 200+);
  - loaded world with F3: `Guest: 30.5 FPS (32.78 ms)`, external counter 29 FPS;
  - opening F3 no longer raises the external counter to the monitor refresh rate.
  The two counters now agree closely. Gameplay choppiness is therefore genuine approximately
  30-FPS guest output, not hidden D3D12 stalls or bad host frame pacing.
- The TU1 presentation patch is persistent in `tools/fix_dangling_gotos.py`: guest
  `0x82BA3018` is emitted as `li r11,1` (`0x39600001`) for XEX hash
  `EE56F849188A6A20`. It removes the guest swap-interval request for 30-Hz presentation, but Fable
  still produces only approximately 30 unique world frames per second in this scene.
- Do not enable the published "High Tick Rate" patch by guessing a TU1 address. The public patch is
  for the no-title-update GOTY/Platinum executable, changes a mostly UI/input update path, and has
  reported gameplay compatibility problems (stairs, Fairfax Gardens, Crucible, and DLC). The first
  apparent TU1 address match was confirmed not to execute during the loaded-world run and was
  reverted.
- Release rebuild succeeded. Focused verification passed all 30 PM4/native-plugin tests, and a
  second `python tools/fix_dangling_gotos.py generated` pass reported `TOTAL fixes: 0`.

## 2026-07-29 frame-cadence follow-up: native batching and deliberate pacing

- A 10-second loaded-world CPU sample showed that the apparent 30-FPS limit was renderer
  saturation, not another hidden 30-Hz sleep: `Native GPU Commands` used 98.1% of one core, `3D
  Engine` 97.3%, and `GameThread` 92.2%.
- Dense instruction-pointer sampling found a major native-worker hotspot in
  `NativeGraphicsSystem::ReadMemoryRangeGeneration`. Every texture query was repeatedly enabling
  physical-memory callbacks and doing an `unordered_map` lookup for every 4-KiB page.
  Physical-memory generations now use a direct 1-MiB dense table, and callback registration is
  skipped when the requested range is already watched. The sampled generation-query hotspot
  disappeared after rebuilding.
- The remaining native samples were overwhelmingly in D3D12/AMD driver submission. The backend
  recorded many command lists but called `ExecuteCommandLists(1, ...)` for every draw even though
  its resources, fences, and command slots were already managed in groups of 64. Resolve and draw
  proof paths now submit command lists in matching batches of up to 64.
- Batching exposed a separate pacing flaw: the swap consumer did not back-pressure the producer.
  Without the renderer's former submission overhead acting as an accidental limiter, the guest
  counter ran at approximately 10,162 FPS while the external window counter stayed near 31 and the
  frame was black. `D3D12Pm4Backend::Present` now applies an absolute `steady_clock` schedule:
  16.667 ms with vsync enabled and 1 ms with it disabled, with catch-up reset after a missed
  interval. This deliberately restores command-ring back-pressure.
- Exact final-build title validation now agrees at refresh cadence:
  `Guest: 58.7 FPS (17.05 ms)`, external counter 59
  (`captures/cadence_final_exact_title.png`). During the childhood-save transition, output improved
  from the previous 2-3-FPS bursts to approximately 35 FPS before the save entered its known long
  hero-streaming/loading stall.
- Do not claim stable loaded-world 60 FPS yet. The only accessible `Hero000` childhood save
  repeatedly stalls while streaming after `NewBeginnings.bik`, so this session could not reproduce
  the already-loaded world used for the original 30.5-FPS measurement. Title cadence proves the
  pacing and counter agreement; a valid already-loaded/adult save is still needed for the final
  gameplay benchmark.
- Release verification passed all 30 focused `pm4.*` and `native_plugin_smoke` tests. The staged
  `rexgpu-native.dll` SHA-256 is
  `2E5CD6A344C215C7E6954C9113AE5B21B7071A238ED6A001B827EC5791E6C56F`. No Fable process or debugger
  remains running.

## Frontend full-pass handoff — 2026-08-05

Read [`FRONTEND_FULL_PASS_NEXT_SESSION.md`](FRONTEND_FULL_PASS_NEXT_SESSION.md) before touching
the frontend. It defines the required start-to-finish parity pass: retail BGF/Lua/decompilation
and ReXGlue tracing first, one authoritative native render path, then deterministic capture and
input validation. The current Resolution and Anti-Aliasing entries are UI/state only until the
PC swapchain/backend is wired; do not call them complete.
