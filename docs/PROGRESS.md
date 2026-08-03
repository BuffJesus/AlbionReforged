# Progress Log

> Living log of milestones and current state. Newest context at the bottom of each section.

## Current status snapshot (2026-07-13 night)

| Track | State |
|---|---|
| **Recompilation** | ★ **Advance-past-logo crash SOLVED** — game now runs logo → new-game → **character creation → world loading** with real UI. Root causes were 2 fixable ReXGlue bugs (truncated `rex_RtlGetLastError`; stale `PAGE_NOACCESS` recovery), NOT the "parked upstream" issue. **Current frontier = heap/VA-layout conflicts during world load.** See memory `fable2-advance-crash-FIXED-rtlgetlasterror` + HANDOFF ▶▶ START HERE. |

### 2026-07-13 (night) — advance-crash solved, reached in-game world load
- Fixed truncated `rex_RtlGetLastError` (0x82CC84C8, size 0x4→0x20 + `sub_82CC84E8` split + `src/RtlGetLastErrorFix.cpp`) → cultures.txt reader works, valid-cultures 0→49 → the logo NULL-deref (`sub_82382558`) is gone.
- Extended `AccessViolationCallback` (`rexglue-src/src/system/xmemory.cpp`) to recover stale `PAGE_NOACCESS` on committed virtual pages (read+write) — also fixed the boot layout-roulette.
- Added a safe leaf-thunk interpreter in `function_dispatcher.cpp` for codegen-missed constant thunks (0x82267CC8…).
- Split over-merged blob `sub_82DB1E88` in `Fable2_config.toml` + full codegen regen → fixed the world-load `0x82DB1E90` FATAL.
- Now: `BaseHeap` reserve/alloc conflicts during world load (next target). Build gotcha: exe rebuild re-stages a runtime without rexglue-src changes — re-stage after every build.

<details><summary>Prior snapshot (2026-07-12)</summary>

| Track | State |
|---|---|
| **Recompilation** | TU1 exe recompiles + builds + boots deep into engine init. Blocked at an upstream ReXGlue core heap/threading crash (CRT exit path) — **parked** (maintainer WIP). [SUPERSEDED — see above] |
| **Decompilation** | ✅ Ghidra 12.1 + XEXLoaderWV analysis of the TU1 exe **complete**; project `Fable2_TU1` saved. GhidraMCP (249 tools) installed. Ready to drive RE. |
| **Modding toolchain** | ✅ Fable2AssetBrowser **BUILT** (`.../source/build/Fable_2_Asset_Browser.exe`) — decodes models/textures/anims/levels/terrain/Lua/audio + BnkWriter injection. |
| **Multiplayer** | Design-stage; recomp/decomp sidesteps cross-arch FP-determinism issues. |

</details>

## Milestones achieved

### 1. Game image → executable
- Located the full ISO (`Fable 2 PLT.iso`, XGD2). Wrote an **XDVDFS parser** to extract
  `default.xex` (21 MB) and later the full ~7 GB game data (`assets/game`).
- Confirmed the disc's exe is the **gold** build (`gold_version.txt`), not TU1.

### 2. Recompilation pipeline stood up
- Installed prebuilt **ReXGlue SDK v0.8.0**.
- Ran `rexglue codegen` → 150 generated C++ files (~300 MB) translating the PPC code.
- Fixed a pile of **v0.8.0 API drift** (the repo was written against an older SDK): CMake link
  targets, the `rex::ReXApp` app model, the `REX_HOOK`/`u32` hook API, config macro renames.
- Wrote `tools/fix_dangling_gotos.py` for a codegen inconsistency (undefined `goto` labels).

### 3. First build + boot
- **Clang 19.1.5** (from VS 2022) compiles the generated code fine (project nominally wants 20;
  don't use winget's LLVM 22).
- Fixed a link issue (STL vectorized `find_first_of` shims) and a heap-arena placement issue.
- **`Fable2.exe` links and boots**: D3D12, audio, VFS mount, XEX load, guest code executes.

### 4. The run→crash→fix grind
- Built a debugging loop: run → read log → symbolize crashes via a linker map + a vectored
  exception handler that captures the guest call chain.
- Cleared several early blockers: `update:` device mount, `build_version.txt`, the TU data
  archive request, and two false function-boundary splits.

### 5. The Lua-VM wall (long debugging arc)
- Installed **WinDbg/cdb** and did deep debugging: the crash was in the game's **Lua VM**
  (`luaV_gettable`, "loop in gettable" = Lua's `MAXTAGLOOP`).
- Found a prior-session band-aid was itself a bug (stubbed `luaG_typeerror`, a *no-return*
  function, to return — corrupting control flow). Removing it exposed a malformed `lua_State`.

### 6. ★ THE breakthrough: wrong executable
- Realized the recomp targets **GOTY TU1** but we'd been running the **gold** exe.
- The Title Update package (`716F0A0D/TU_...`, a LIVE STFS) contains `default.xexp`
  (a `MODULE_PATCH|DELTA_PATCH` XEX) + `tu1_data.bnk`.
- Applied the patch with **`xextool -p default.xexp`** → `default_tu1.xex`. xextool validated the
  patch against the base (proving gold is the correct TU1 base).
- Re-ran codegen + build on the TU1 exe. **The Lua "loop in gettable" crash vanished** — it was a
  gold-code bug that TU1 fixes. Confirmed via the module code range growing (`...832CA03C`).

### 7. New wall = upstream core issue → pivot
- Post-TU1, the crash is in the guest CRT exit path (`xstart → _doexit`): the main thread returns
  without spawning game threads. Fable2Recomp Discord confirms this class of bug: **"core heap and
  threading issues in both Fable 2 and ReXGlue,"** actively-rewritten WIP (only the child section
  works even in the maintainers' build).
- **Decision:** park the boot track (don't duplicate upstream engine work); pivot to the
  decompilation + modding tracks that advance the real long-term goal.

### 8. Decompilation + modding toolchain
- Reused the existing Ghidra 12.1 install; installed **XEXLoaderWV** + **GhidraMCP** (fixed the
  12.1.2→12.1 version gotcha).
- **Ghidra headless analysis of the TU1 exe completed** — XEXLoaderWV decrypted it and loaded all
  PE sections; the `Fable2_TU1` project is analyzed and saved.
- Started building **Fable2AssetBrowser** (the modding-toolchain foundation).

## Next
See [ROADMAP.md](ROADMAP.md). Immediate: drive RE on the analyzed `Fable2_TU1` project (launch
Ghidra GUI + GhidraMCP, or headless export scripts); finish the AssetBrowser build.
