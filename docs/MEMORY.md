# Technical Memory (deep notes)

Dense, non-obvious facts a resuming session needs. Companion to the narrative docs. This is the
portable version of the working memory built up during development.

## Native renderer checkpoint — 2026-08-02

See [SESSION_SNAPSHOT_2026-08-02_NATIVE_RENDERER.md](SESSION_SNAPSHOT_2026-08-02_NATIVE_RENDERER.md).
The native D3D12 path presents frontend/loading/UI and compositor output, but the loaded Hero001
world is under-lit/flat. The final compositor reads live finite HDR from `0x19C67000`; normal native
HDR is approximately peak `1.0605`, mean max `0.0811`. A diagnostic ×16 output reaches approximately
peak `16.97`, mean max `1.297`, proving real output exists and the issue is upstream material/output
scale or semantics.

Late-world draw logs print translator cache keys. The forced pixel probe compares guest microcode
hashes. Exact mappings are:
`18F21758F4DC7D83→70E7786A87CAEFF5`, `62BC938CB35BCC47→4E0825A6D3B60F88`,
`802C36EB2C3261DC→A820293DEE9C9A2D`, `CFCC25A84593468B→CCA25F8031D8EADD`, and
`2938CD4379D4F909→DB9F19BA0E43675E`. Target guest hashes directly in the next A/B.

The screenshot helper foregrounds Fable unless `-NoActivate` is supplied; this can alter visible
title/menu/loading state and is not passive capture. No process remains after the last run.

### Native probe results - 2026-08-03

The next A/Bs are complete. A targeted solid-magenta probe for guest shader
`70E7786A87CAEFF5` (translator key `18F21758F4DC7D83`) was active but did not change the loaded-world
frame. A `TARGET=all` probe turned the entire client magenta, proving the native output/resolve/present
chain. Forcing all translated predicates true also left the world unchanged, so a false p0 is not the
sole black-world cause. Guest target `4E0825A6D3B60F88` produced a small visible magenta geometry patch;
guest target `CCA25F8031D8EADD` was active for 92 color-writing material draws without visible change.
The next useful seam is representative vertex/raster/depth/color-write acceptance for the main world.

## Executable / TU
- Recomp targets **GOTY TU1**. Disc `default.xex` = **gold** (has `gold_version.txt`). Recompiling
  gold caused the Lua "loop in gettable" crash; **TU1 fixes it**.
- TU1 package: `716F0A0D/TU_16L61VH_...` = a **LIVE STFS**. Contains `data/` (dir), **`default.xexp`**
  (2.99 MB, XEX2, `module_flags=0x50` = MODULE_PATCH|DELTA_PATCH), and `tu1_data.bnk` (94,535 B; a
  v3 BNK: gameface Lua + localized `book.babel`). Game requires `update:\data\tu1_data.bnk`.
- Apply patch: `xextool -p default.xexp -o default_tu1.xex <gold>` → 21,282,816 B, "retail encrypted
  uncompressed"; xextool validates patch vs base (proves gold is the correct base). `-c u`/`-e u`
  force uncompressed/decrypted; `-u` bakes patch standalone.
- Title id **4D5307F1**. Module code range TU1 = `82170000-832CA03C` (gold was `...832C2A3C`).

## Extraction
- ISO `D:\Downloads\Bitcomet\fable2\Fable 2 PLT.iso` = XGD2, game partition base **0xFD90000**
  (XDVDFS magic `MICROSOFT*XBOX*MEDIA` at 0xFDA0000). Root: `/default.xex`, `/data`, `/$SystemUpdate`
  (a *system* update only, not the game TU), `/nxeart`. Full data extracted to `assets/game` (~7 GB).
- STFS block math: `block_offset = 0xC000 + fix_blocknum(block,shift)*0x1000` where `fix_blocknum`
  adds `(b//0xAA+1)<<shift` (and `(b//0x70E4+1)<<shift`). **table_size_shift=0** for this TU
  (brute-force 0/1, validate). File table @ volume descriptor 0x379: entries 0x40 B, name[:0x28],
  start_block@0x2F (u24 LE), size@0x34 (u32 BE).

## ReXGlue / codegen / build
- SDK v0.8.0 (`rexglue-sdk/win-amd64`). Codegen: `REXSDK=<sdk> rexglue.exe -f codegen
  fable2_manifest.toml` (~50s, 150 cpp ~300MB). `-f` migrates old config → v0.8.0 manifest.
- **Always** run `python tools/fix_dangling_gotos.py generated` after codegen (codegen sometimes
  emits `goto` to undefined labels instead of a REX_FATAL guard).
- Build: **Clang 19.1.5 from VS 2022** compiles the generated code (project nominally wants 20;
  winget LLVM 22 is too new — don't). VS dev shell + `...\VC\Tools\Llvm\x64\bin` on PATH.
  `cmake --preset win-amd64-release -DCMAKE_PREFIX_PATH=<sdk>/win-amd64`; `cmake --build
  out/build/win-amd64-release --parallel 10 -- -k 0`. No vcpkg (SDK bundles SDL3 etc.).
- v0.8.0 API drift fixes (repo was older): CMake link `rex::core/system/kernel/graphics/ui` → just
  `rex::runtime`; `main.cpp` → `rex::ReXApp` model; `PPC_HOOK`→`REX_HOOK`, `ppc_u32_t`→`u32`;
  config macros `PPC_CODE_BASE`→`REX_CODE_BASE` in `generated/Fable2_init.h` (has `PPCImageConfig`,
  `PPCFuncMappings[]`). Link shims: `src/msvc_stl_compat.cpp` (STL vectorized find_first_of).
- Heap: `src/heap.cpp` = the game's Lua allocator (`rex_lhHeapRealloc` = `lua_Alloc`). Allocs 256MB
  from the **0x40000000 virtual heap** via `mem->LookupHeap(0x40000000)->Alloc(...)` (lands @
  guest 0x60000000 / host 0x160000000). Also hooks PhysicalAllocCached `sub_82B53420`.

## Run / debug
- Launch: `Fable2.exe --allow_game_relative_writes true --game_data_root <assets/game>`. Needs
  `assets/update/` to exist (mounts `update:`) + `assets/update/data/tu1_data.bnk` +
  `build_version.txt` in game: and update:. Logs → `out/build/win-amd64-release/logs/Fable2_NNN.log`.
- Crash symbolization: linker map `Fable2.map` (via `-Wl,/MAP:Fable2.map` in CMakeLists); host addr
  → map via `target = 0x140000000 + rva`. `src/DiagnosticHooks.cpp` has a vectored-exception handler
  that writes the guest call chain (RVAs) to `crash_stack.txt`.
- **Guest↔host memory: membase = 0x100000000** (guest G = host 0x100000000+G). Guest mem stored
  big-endian (cdb `dd` shows byteswapped). REX_PHYS_HOST_OFFSET only for phys addrs (≥0xE0000000).
- **PPCContext (rsi/ctx) GPR layout** (all regs, 8B each; order r3,r0,r1,r2,r4,r5,r6,r7,r8,r9,r10,
  r11,r12,r13,r14…r31): r10@0x50, r27@0xD8, r28@0xE0, r29@0xE8, r31@0xF8. `REX_FUNC(x)` =
  `void x(PPCContext& ctx, uint8_t* base)` (rcx=ctx, rdx=base at entry).
- cdb: `C:\Program Files\WindowsApps\Microsoft.WinDbg_*\amd64\cdb.exe`. `cdb -c "g; <cmds>; q"
  Fable2.exe <args>` (run from outDir); catches the AV automatically.

## Lua-VM root cause (the big arc)
- `sub_82227EA0` = Lua `luaV_gettable` (hash const 0x5BD5E995; loops to r27==100 = MAXTAGLOOP).
  At timeout calls `sub_82A2C520` = `luaG_runerror("loop in gettable")`.
- `sub_82A2C300` = `luaG_typeerror(L, obj, "index")` — a **NO-RETURN** fn (longjmps). A prior session
  stubbed it to *return*, corrupting `luaV_gettable` control flow → the loop. **Removed** the stub.
- All of the above were **gold bugs; TU1 fixes them.** On TU1 the game gets past Lua init.
- Post-TU1 crash: `sub_832B7688 ← rex_doexit ← xstart` = guest CRT teardown; main thread returns
  without spawning game threads (no guest `XThread` created). `rex_exit`/`rex_abort` NOT hit → xstart
  calls `_doexit` directly. This = the upstream **core heap/threading** issue (Discord). PARKED.

## Decompilation toolchain
- Ghidra 12.1 @ `D:\Subuwu\tools\ghidra-public` (Java 21). Installed `REPlugins` pre-built
  **XEXLoaderWV** + **GhidraMCP 5.13.1** into `<ghidra>/Ghidra/Extensions/`; edited each
  `extension.properties` version 12.1.2→**12.1** (they shipped for 12.1.2).
- Analyzed: `analyzeHeadless ghidra_proj Fable2_TU1 -import default_tu1.xex -max-cpu 12` → **done**.
  XEXLoaderWV decrypted (file/session keys logged) + loaded all PE sections. VMX128 pcode warnings
  are benign.
- GhidraMCP = HTTP server (port 8089) inside Ghidra GUI exposing 249 RE tools; hit endpoints via
  curl, or the MCP bridge `REPlugins/GhidraMCP/bridge_mcp_ghidra.py`.

## Modding / formats
- ✅ **AssetBrowser BUILT**: `Fable2AssetBrowser/source/build/Fable_2_Asset_Browser.exe` (~38 MB,
  static, no DLLs). Build: `cmake -S source -B source/build -G Ninja -DBUILD_TESTING=OFF` then
  `--target Fable_2_Asset_Browser` (repo ships without `tests/` + `tools/ShaderBankExtract.cpp`;
  guarded in its CMakeLists). GUI app — point it at `Fable2Recomp/assets/game/data`.
- `Fable2AssetBrowser` (C++/ImGui, FetchContent deps — no FBX SDK) decodes: MDL models (→FBX/glTF),
  Lh textures (→PNG/DDS), anim banks + Havok, BNK archives (+**BnkWriter** = REPLACE/ADD entries =
  the injection path), levels/terrain (`HeightfieldLoader`, `EhfChunkParser`, `TerrainTextureRegistry`,
  `LevelLoader/Export`), Lua decompiler, XMA2 audio, ISO mount. Blender addon `FableLevelImporter.py`.
- `f2tool` exists as the headless Studio P1 beachhead: `Fable2AssetBrowser/source/tools/F2Tool.cpp`,
  target `f2tool`, output `source/build/f2tool.exe`. Commands: `list`, `extract`, `verify`, raw-entry
  `inject` with one-time `.bak`, `hash [--lower]` for the FNV-1 resource/text-name hash, `editset`,
  and `mod`. `source/tools/F2Tool.cpp` is explicitly unignored in `.gitignore`.
- Modding strategy: BNK/Lua = *read/on-ramp* (load base game); modern formats = *destination*
  (extend decompiled loaders). See MODDING.md.
- MP: recomp/decomp = all players run same x86 code → cross-arch FP-determinism desync largely gone.

## Gotchas
- Recompile TU1, not gold. Run fix_dangling_gotos after every codegen. Ghidra ext version must match
  the install. Don't hand-edit generated code (use `src/` weak overrides). Don't band-aid over
  upstream bugs. `unreal.Rotator`-style keyword-arg care N/A here (that's the other project).

## 2026-07-18 Current Modding State
- Launch env for mod features: `FABLE2_MODAPI=1 FABLE2_MODTEXT=1`. `FABLE2_CONSOLE=1` is now gated
  and no longer the boot heap-corruption crash path. Use `FABLE2_RESOLVE_DIAG=1` only when hunting the
  black hero/dog skin issue.
- Tooling rule from user: do not auto-open GUI apps. `qrenderdoc.exe --python` opens the Qt GUI even
  when used for scripting. `renderdoccmd thumb` is headless and acceptable.
- `register_native` is proven: host PPCFunc thunks are allocated through
  `Runtime::function_dispatcher()->AllocateThunk`, pushed with guest `lua_pushcclosure`, and bound into
  the plain `Debug` table at a `luaD_call` depth-0 safe point. Public Lua API is
  `Debug.Mod.Ping()`, `Debug.Mod.TextSet(tag, str)`, `Debug.Mod.Log(msg)`, plus prefixed aliases
  `Debug.ModPing`, `Debug.ModTextSet`, `Debug.ModLog`. `Debug.Mod.Log` writes `[modlua] ...` to host
  logs and is already used by modmenu breadcrumbs.
- Console/bridge injection safety gate: `fable2::modapi::ReadyForInjection(base, L, minSlots)` checks
  raw-populated class tables and Lua stack headroom before any hook-injected guest Lua. This closed the
  `FABLE2_CONSOLE=1` boot crash class caused by stack realloc under engine-owned live `StkId`s.
- Childhood skip core is user-confirmed: "Sparrow's Path" choice -> cutscene-free childhood -> custom
  sign text/toaster -> "Old Town's Fate" chooser -> skip to Fairfax. The chooser writes the real adult
  Old Town lever: `Gameflow.ChildhoodResolutionEvil` (Derek/good false, Arfur/slums true). Vanilla
  `SkipToLuciensStudy` only preserves the default good outcome, so this adds a real capability.
- Childhood skip staged fixes after latest user reports:
  - Extended-load fade mask removed. `LockScreenFade` likely held the loading screen while dialogue
    played underneath.
  - Fairfax camera jump theory: skipping before the crowd cheer leaves a queued look-camera that fires
    after the level jump at stale childhood coordinates. Breadcrumb showed `skip-flush lookat=false`,
    meaning the old active-only flush missed it. Current staged fix flushes the look-camera
    unconditionally and sets `__mm_block_lookat` so `SetLookAtCamera` is swallowed across the
    transition, then clears the block after post-skip cleanup.
  - Dog missing at adulthood: old limbo breadcrumb had an empty dog name. If skipping before qc040, no
    real dog exists and `GetDog()` can be a nameless placeholder. Current staged fix calls
    `ScriptFunction.CreateDog()` when needed, then limbos by non-empty name; if still nameless it sets
    `ChildhoodVars.DogName=nil` so QC060 can take the existing-dog reposition branch.
  - Morality stat applies (+5/-5 VFX observed), but the good/evil deed toast does not reliably show.
    Low priority; likely suppressed by the Lucien study arrival dialogue/cutscene.
- Pause-menu integration state: GUI hook appends a Mod Menu entry and the bridge now fires end-to-end.
  `Fable2_491.log` shows `MMDBG_SEL1_MM_Open` -> `ModBridge5` -> gameplay `modbridge_cmd.lua`
  executed. Current hook split: population in `tools/lua_mod/gui_modmenu_hook.lua`, selection and
  bridge dispatch in `tools/lua_mod/gui_expandablemenuinput_hook.lua`; `docs/MENU_SYSTEM_RE.md` has
  the extender map. Remaining work is first-class Gameface/native list integration, not bridge
  viability.
- Level-load RE state: `docs/LEVEL_LOAD_RE.md` maps LS_* as the graphics streaming machines
  `LevelGraphicsFile` and `EngineResourceList`, pumped from `TextureSystem_FrameUpdate`. They consume
  `.lmp`, engine level `LevelGraphicsFile`, and `<level>.engine_data`; they are not the gameplay
  entity/script load. Editor-added Type-2 model records hit the stock graphics parser, but new model
  paths must resolve through mounted-bank hashed resource lookup and matching `engine_data` hashes.
- Black hero/dog skin state: RenderDoc thumbnail confirmed exposed hero skin is black while clothing
  and nearby NPC skin are correct, so the bug is the hero appearance-morph render-to-texture composite
  shared with the dog, not lighting/global textures. User ruled out resolution scale; it existed before
  scale changes. **NEW DIAGNOSTIC (2026-07-19, DEFINITIVE):** Ran with `FABLE2_RESOLVE_DIAG=1` and found
  **the morph composite (0x1B169000) NEVER appears in ANY resolve logs.** Proves: (1) GPU RT rendering
  is happening (pixels exist GPU-side); (2) the resolve is NOT writing to guest memory; (3) readback
  DOES pull from raw EDRAM (that's why it shows when readback=on) but the readback result is corrupt.
  **Pragmatic fix deployed:** Skip readback for the 0x1B1xx000 range to isolate the problem (other
  objects readback cleanly, skin goes black—better than corrupt). Long-term fix = render-target→texture
  bridge (custom DXT1 shader or RT-direct guest copy). The memory-cache approach (fable2_morphcache)
  never fires because the composite never fills correctly in guest memory.
