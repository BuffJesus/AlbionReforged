# Technical Findings (research)

Deep-dive explanations of the key discoveries — the "why," for someone who wants to understand.
For dense reference values see [MEMORY.md](MEMORY.md); for commands see [TOOLCHAIN.md](TOOLCHAIN.md).

## 1. Gold vs. TU1 — the root cause of "everything downstream was wrong"

The Fable2Recomp project is built for **GOTY Title Update 1**. We recompiled the disc's `default.xex`
— which is the **gold** (release) build (`gold_version.txt` in the data confirms it). The two are
different code: an Xbox 360 Title Update ships a **XEX delta patch** (`default.xexp`,
`module_flags = 0x50` = MODULE_PATCH | DELTA_PATCH) that rewrites specific functions plus a data
archive (`tu1_data.bnk`).

Recompiling gold produced a game that booted but crashed in the Lua VM. Applying the TU1 patch
(`xextool -p default.xexp`) produced `default_tu1.xex`; recompiling *that* made the crash **vanish**.
`xextool` validated the delta against the base before applying — proving the disc's gold exe is the
correct base for TU1, and that the recomp genuinely expects the patched image. Lesson: **for a
recomp titled "TUn," you must apply the title update's XEX patch, not just its data.**

## 2. The Lua-VM crash arc (and a band-aid that was itself the bug)

Long cdb debugging traced the gold crash into the game's **Lua interpreter** (Fable II is heavily
Lua-scripted):
- `sub_82227EA0` = Lua **`luaV_gettable`**. Its 100-iteration loop is Lua's `MAXTAGLOOP` (guards
  against infinite `__index` metatable chains). On timeout it raises `luaG_runerror("loop in
  gettable")` — that string is literally in the binary.
- `sub_82A2C300` = **`luaG_typeerror(L, obj, "index")`** — Lua's "attempt to index a non-table"
  error. Crucially it's a **no-return** function (it `longjmp`s to a protected call).
- A prior session, fighting an apparent infinite spin, **stubbed `luaG_typeerror` to return.** But
  making a no-return function return corrupts `luaV_gettable`'s control flow — it runs on to
  `MAXTAGLOOP`. The stub *was* the loop's cause. Removing it exposed a genuinely malformed
  `lua_State` (a null internal pointer), which is what the underlying gold bug produces.

All of this was a **gold-code defect that TU1 fixes** — hence the crash disappearing on the patched
build. Takeaway: **don't band-aid over upstream bugs by making functions lie about their contracts;**
it masks the real cause and creates new ones.

## 3. The post-TU1 wall: a core heap/threading issue (upstream)

On the TU1 build the game gets past Lua init and crashes later in the **guest CRT exit path**
(`xstart → _doexit → sub_832B7688`, a null-deref walking the atexit list). Neither `rex_exit` nor
`rex_abort` is called, and **no guest game-threads are ever created** — the main thread runs init and
returns instead of staying alive in the game loop. The Fable2Recomp maintainers have publicly named
this class of problem: **"core heap and threading issues in both Fable 2 and ReXGlue,"** which they're
actively rewriting (their own build only reaches the child section). So this is upstream engine work,
not a game-specific bug we can surgically fix — hence the boot track is parked.

## 4. The memory & translation model (for debugging)

- **Guest↔host:** ReXGlue reserves guest memory at host base **`0x100000000`** (guest address `G` =
  host `0x100000000 + G`). Guest memory is stored **big-endian**; host debuggers show it byteswapped.
- **Recompiled functions call each other as real host functions** and carry x64 unwind info — so a
  host backtrace (or `RtlCaptureStackBackTrace`) directly reveals the guest `sub_XXXX` call chain.
  That's what `src/DiagnosticHooks.cpp` exploits to log crash stacks.
- **Guest registers live in a `PPCContext` struct** (pointer usually in `rsi` mid-function; `rcx` at
  entry). GPRs are 8 bytes each in a fixed order (r3,r0,r1,r2,r4,r5,…r31), so e.g. `r28 = [ctx+0xE0]`.
- Host addresses symbolize via the linker map: `map_target = 0x140000000 + (hostaddr - modulebase)`.

## 5. Format internals worth knowing
- **XDVDFS** (the ISO game partition, base `0xFD90000`): tree of 0x40-byte dir entries; file data at
  `base + sector*2048`.
- **STFS** (the TU package, "LIVE"): 0x1000-byte blocks with interleaved hash tables;
  `offset = 0xC000 + fix_blocknum(block, shift)*0x1000`, shift determined empirically (0 here).
- **BNK** (Fable archives): v2/v3; v3 = base offset + version + a zlib chunk stream that inflates to
  a file table (name, offset, sizes, compression flag). `BnkWriter` can REPLACE/ADD entries.
- **Levels/terrain:** `EHF` chunked environment format + heightfields + a terrain-texture registry;
  decoded by AssetBrowser, importable to Blender via `FableLevelImporter.py`.

## 6. Why the pivot to decompilation is sound
The remaining boot blocker is upstream WIP; the user's real goals (modding, multiplayer) live in the
**decompilation** track, which doesn't need a fully-booting game. The recomp is a perfect scaffold
and runtime *oracle* for an incremental, *functional* (not byte-matching) decompilation — we can run
a hand-decompiled function next to the machine-translated one and diff behavior. And because a
recomp/decomp runs identical x86 code on every machine, it sidesteps the cross-architecture
floating-point desync that breaks emulator multiplayer.
