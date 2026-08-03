# Black hero/dog skin — static RE of the appearance-morph composite path

## 2026-07-28 late-night live correction

The user still observed black hero skin and dog texture artifacts in the
visible run1159 xenos-oracle gameplay. Native runs 1160/1161 emitted no
`0x12704000` player/dog resolve candidate and no successful selective
materialization while loading Hero000, even though the 31,968-byte
`texturemorphs.bin` was read successfully.

Therefore the selective femtofork-derived dependency below is real but is not
yet a complete fix for this save/load path. Hero000 may consume a stale saved
atlas, use another GPU producer/destination, or miss a CPU composite upload
without performing the recognized regeneration event. Next capture the exact
hero and dog texture fetch addresses in the visible frame and trace their
producer generations. Do not broaden to general resolve readback.

## 2026-07-28 correction — one selective resolve is a required guest dependency

The earlier conclusion that the black-skin bug has no resolve dependency was
too broad. The game's higher-level CustomAtlas assembly is CPU-managed, but
the Fable-specific Xenia implementation proves that player/dog texture
regeneration includes one GPU-produced surface that the guest subsequently
reads. The exact destination is `0x12704000`; reading back every resolve fixes
the symptom at unacceptable cost, while reading back only this address fixes
it selectively.

Primary upstream evidence:

- repository:
  https://github.com/just-harry/unofficial-xenia-femtofork-for-fable-ii
- selective fix:
  https://github.com/just-harry/unofficial-xenia-femtofork-for-fable-ii/commit/746213fb8d13b79ff985c69cb45c81d7ce5dbc5c

The native ReXGlue renderer now mirrors that edge only. A color
`k_8_8_8_8` resolve to `0x12704000` is read back, converted from host-linear
RGBA to the guest Xenos tiled/endian layout, and materialized in guest memory.
Every other resolve stays in the direct-owned host render graph, so this does
not restore an EDRAM heap or general guest-memory round trips.

The older pool/residency analysis below remains useful for the later compressed
source textures and explains why pool expansion changed black into visible
artifacts. It was incomplete as a total root-cause statement: guest-side CPU
management and the selective GPU-to-guest atlas dependency both exist.

## ▶▶ 2026-07-19 (round 3) — LIVE FILL-PATH RE + partial fix (BLACK → VISIBLE-WITH-ARTIFACTS)

**Premise reset (from the user's own logs, no new run):** the composite is NOT a resolved/read-back
render target — 31k resolves + 4k readbacks/run never touch it (see memory
`fable2-black-skin-renderdoc`). It is a CPU-composited pool texture. RT→texture-bridge work retired.

**Stage 1 fix — pool boost (SHIPPED, user-confirmed black→visible):** `src/AtlasFillDiag.cpp`
`sub_82B60910` hook. The composite pool has 6 buckets keyed by format class: `f0=35`=DXT1 (8 B/blk),
`f0=36`=DXN (16 B/blk). The hero **skin** is DXT1 → row 4 (1024², stock count 28); the fur/normal is
DXN → row 5 (16). The original POOLBOOST only widened row 5, never the DXT1 bucket the skin uses.
Boost now covers DXT1+DXN at 512²/1024² (row4 28→64, rows2/3→80). Removed allocation starvation
(`bucket-alloc FAIL=0`). Did NOT fully fix — residual artifacts remain.

**The fill path (LIVE-traced, `src/MorphFillTrace.cpp` FABLE2_FILLTRACE=1):**
```
CustomAtlas_ProcessOnePending 0x82181828    (per-frame, head of pending list @0x8349FB58)
  per source: StreamLayerSources 0x82A5C690 -> FableTexture_TouchAndGetD3DTexture 0x8221EAF0(src, mip, flags)
  if any source NOT ready (cVar8==0): park, retry next frame
  when all ready: CustomAtlas_TouchTarget 0x82A5C250 = TouchAndGetD3DTexture(target@desc+0x1C, 0, flags=1)
TouchAndGetD3DTexture 0x8221EAF0 (for a non-resident tex, +0x54==0x7FFFFFFF):
  if (this+0x84 > 0)      FableTexture_StreamMips 0x821D7A98(this, mip, 1)   // CREATE into +0x48 (gated: only if +0x4c==0)
  if ((flags & 3) != 0)  FableTexture_FinalizeStreamedResource 0x82A7B148(this,1)  // PROMOTE +0x48->+0x40/+0x44, set +0x54
FinalizeStreamedResource 0x82A7B148: if (this+0x4c == 0) return 0 (NO-OP);  else promote + compute residState @+0x54.
```
So residency hinges on **StreamMips's CreateTextureWithFallback populating +0x48/+0x4c**; if it doesn't,
Finalize no-ops → `+0x54` stays `0x7FFFFFFF` → never resident → opaque black.

**FILLTRACE (log 526/527) verdict:** `TouchTarget` composites (512², mips=6) become resident
(`residState 0`, real pool res) — that's the skin the user now sees. The residual artifacts are a
batch of **mips=1** DXT1 textures (`tex=4216Axxx`) streamed via StreamLayerSources (they are composite
SOURCES) that never become resident. ⚠ The earlier "res 0→0 ⇒ create failed" read was a TRACER BUG:
the STREAM hook read `+0x44`/`+0x54` right after StreamMips but BEFORE Finalize, which is expected 0.
Tracer now also reads `+0x48`/`+0x4c` (pending-resource slot) to distinguish create-fail vs
finalize-skip — needs one more run to read.

**Two candidate roots (next run decides via +0x48/+0x4c):**
1. StreamMips create genuinely fails for these (pool/size) → +0x48 stays 0.
2. Sources streamed with `flags & 3 == 0` (StreamLayerSources passes flags 0/4) → Finalize SKIPPED
   for sources → source never resident → composite parked. (Re-trace of StreamMips create math for
   mips=1/512² gives a VALID create(512,512,1), so "mips=1 breaks the create" is doubtful → this
   flags-skip path is the stronger hypothesis.)

**Mip-fix (`src/MorphDiag.cpp` CreateTarget + `MorphFillTrace.cpp` StreamMips, env FABLE2_MORPHMIPS):**
bumps composite mips<=1 → floor(log2(min(w,h)))-3. CreateTarget-site version did NOT fire (the
failing textures aren't created there). StreamMips-site version built, UNVERIFIED, may be moot if
root is the flags-skip (#2). Dead end this round: give-up softening (FABLE2_MORPHKEEP) — ineffective.

**Corrections:** the 768x704 @ page 0x1B169 is NOT a CustomAtlas composite (absent from the whole
fill path) and not "the" hero skin — separate all-zero texture, identity TBD. Retire the
`command_processor.cpp:3016` 0x1B1xx morph-skip hack.

---



**Session 2026-07-18 (static only, no game launches).** Ghidra project `Fable2_TU1`,
all function labels below applied via `tools/ghidra_label/LabelApply`
(`ghidra_out/labels_blackskin_20260718.tsv`, 42 functions, also appended to `labels_in.tsv`).
Decomp transcripts: `ghidra_out/blackskin_*_20260718.txt`.

## TL;DR — three prior beliefs corrected

1. **The composite is NOT render-to-texture.** The whole CustomAtlas path contains no
   SetRenderTarget / resolve anywhere. That is *why* the `FABLE2_RESOLVE_DIAG`
   DROP/SKIP instrumentation never fired — the resolve path is simply not involved.
   The composite output is an ordinary **pool-allocated texture whose contents the
   game (re)generates in place**.
2. **"fmt 0x23 / 0x28" are GAME format enums, not Xenos formats.** The game's format
   table `g_TextureFormatTable @ 0x8331DAD8` (0x70-byte entries, first dword =
   X360 D3DFORMAT) maps:
   - game `0x23` → `0x1A200152` → base fmt **0x12 = k_DXT1**
   - game `0x25` → `0x1A200153` → base fmt **0x13 = k_DXT2_3**
   - game `0x28` → `0x1A200171` → base fmt **0x31 = k_DXN** (normal-map composite)
   - game `0x04` → `0x28280186` → **k_8_8_8_8** (only used when the low-quality flag
     at composite+0x24 is set)
   So on the host the skin composite is just a **DXT1** texture (raw_fmt=18) and the
   fur/normal composite is **DXN** (raw_fmt=49). The prior host-side hunt for
   raw fmt "0x23" (and the `FABLE2_MORPH_FMT_FIX` k_24_8_FLOAT experiment in
   `texture_cache.cpp`) was a category error — **red herring, can be retired**.
3. **The conclusion "composites are never sampled" is unproven.** The existing
   `[morphbind]` logs DO show many DXT1 (raw_fmt=18) bindings with healthy nonzero
   content (`Fable2_490.*.log`, e.g. base=17D77 1024x1024, base=155E7 512x512) — but
   nothing distinguishes the composite from regular clothes textures without knowing
   its guest base address. The new instrumentation (below) closes that gap.

## Guest-side architecture (all addresses = TU1 exe, labels applied in Ghidra)

The game's own class name is **`CustomAtlasTexture`** (string at `0x820F6D10`,
immediately after its vtable `0x820F6D00`).

### Objects

**CustomAtlasTexture (composite descriptor), 0x28 bytes, vtable 0x820F6D00**
| offset | meaning |
|---|---|
| +0x00 | vtable (`CustomAtlasTexture_dtor` = 0x82A5AD38) |
| +0x04 | refcount |
| +0x08 | source id (`*(channel+0x14)+8`) |
| +0x0C..| copied morph params (`Function_82A5D8F0` from channel+0x50) |
| +0x10/+0x14 | begin/end of **source-entry vector** (0x24-byte entries; entry+0x1C = morph texture, entries reference source textures via handle → `Function_821ABA98` lookup) |
| +0x1C | **target texture** (refcounted `FableTexture*`) — the composite output |
| +0x20 | weight/mode (from channel+0x48) |
| +0x24 | low-quality/immediate flag (forces fmt 4 + 0x80×0x80 min) |
| +0x25 | invalid/dirty flag, +0x26 = "notify world" flag |

**FableTexture (engine texture object), 0xE0 bytes, ctor `FableTexture_ctor` 0x82A79A70, vtable 0x820F70FC**
| offset | meaning |
|---|---|
| +0x04 | refcount |
| +0x14 | embedded stream-provider iface (vtable 0x820010C8) |
| +0x20 / +0x28 | D3D-header wrapper slots (linear / gamma) — what the material system binds |
| +0x40/+0x44 | low-level **texture resource** (pool allocation) |
| +0x54 | resident-mip state (0x7FFFFFFF = nothing resident) |
| +0x81 | valid flag; +0x84 = priority; +0x8C = content mode (0=pool create, 2=realloc w/ provider) |
| +0x90 | flags (bit5 = has-alpha → composite becomes DXT2_3) |
| +0x94/+0x98/+0x9C/+0xA0 | width / height / **game format enum** / mip count |

**Texture resource** (from `TexturePool_CreateTexture` 0x821E0D20): +0x28/+0x30 = the
two 0x34-byte X360 D3D texture headers (= GPU fetch constants; gamma variant =
table fmt `|0x7E00`), +0x3C = header block ptr, **+0x40 = guest data base address**
(set via `XGOffsetResourceAddress` in `TextureResource_InitHeaders` 0x82B5FBA8),
+0x38 = stream-provider (refcounted).

### The pipeline (per frame)

1. **`CustomAtlas_ManagerUpdate` 0x82A5CAC8** — walks morph channels (type 0x13),
   creates/refreshes composite descriptors; for each changed composite:
   - `CustomAtlas_CanUseSourceDirectly` 0x82A5AF00 → trivial case:
     `CustomAtlas_SetTargetToFirstSource` 0x82A5C138 (target = source texture, no
     compositing — this is why ordinary NPCs are unaffected);
   - else `CustomAtlas_EnsureTargetTexture` **0x82A5BCE8** — (re)creates the target:
     dims = max over sources (clamps `0x8331AB00/04`, min `0x8331ABB0/B4`), format
     DXT1/DXT2_3/DXN as above; allocated via `FableTexture_CreateTarget`
     **0x82A79E68** → `TexturePool_CreateTextureWithFallback` 0x821D79B8 (halves
     dims until a pool bucket fits);
   - queues the composite on the **global pending list `0x8349FB54/58/5C`**
     (`CustomAtlas_QueuePendingComposite` 0x82A5E918).
2. **`TextureSystem_FrameUpdate` 0x82181B68 → `CustomAtlas_ProcessOnePending`
   0x82181828** — one composite per frame: builds a layer per source entry
   (`CustomAtlas_MakeLayerFromSource` 0x82A5C4A0) and **streams the source textures
   to the mip matching the composite resolution** (`CustomAtlas_StreamLayerSources`
   0x82A5C690 → `FableTexture_TouchAndGetD3DTexture` 0x8221EAF0); layers still
   loading are parked in the work array `0x8349FB60/64/68/6C`; when everything is
   resident the target is touched (`CustomAtlas_TouchTarget` 0x82A5C250) and the
   node is unlinked. The actual pixel generation happens inside the texture
   streaming machinery (`FableTexture_StreamTick` 0x8225F9D0 →
   `FableTexture_StartAsyncFill` 0x82A7A680, async dispatch 0x82B60E20 with
   callback table 0x8331AB78) — **the exact DXT-domain blend routine was not pinned
   this session** (it runs in the async fill; a DXT1/DXN morph is a cheap
   endpoint-interpolation in block space, all-CPU). This is the main remaining
   guest-side unknown.
3. **Binding:** `Model_BindCustomAtlasComposites` **0x82A9F778** — for every model
   texture slot, `CustomAtlas_LookupCompositeForTexture` **0x82A5C318** (global map
   `0x8349FB48/4C` keyed by texture id; per-slot kind table `0x834A5278`) and
   **refcount-swaps the composite target into the model's texture slot**. From
   there the composite renders through the *identical* path as every other DXT
   texture (fetch constant = the pool texture's D3D header; base address = the pool
   memory at resource+0x40). There is no special bind path to break.

## Host side (rexglue-src)

- Fetch/bind: `src/graphics/d3d12/texture_cache.cpp` `UpdateTextureBindingsImpl`
  (existing `[morphbind]` diag) — the composite arrives as a plain k_DXT1/k_DXN
  fetch, fully supported (`texture_load_dxt1_rgba8_cs` etc.).
- Upload: `D3D12TextureCache::LoadTextureDataFromResidentMemoryImpl`
  (`d3d12/texture_cache.cpp:~1710`), driven by `TextureCache::LoadTextureData`
  (`src/graphics/pipeline/texture/cache.cpp:879`).
- **Invalidation**: CPU writes to guest texture memory are caught by shared-memory
  write watches: `Texture::MakeUpToDateAndWatch` arms
  `SharedMemory::WatchMemoryRange` on the base/mip ranges
  (`pipeline/texture/cache.cpp:686-703`); a write fires
  `TextureCache::WatchCallback` (`:746`) → texture marked outdated → re-uploaded
  before the next binding update. **This is the load-bearing mechanism for the
  composite**: the game rewrites the SAME guest memory when the morph changes, so a
  missed watch = the shader samples the stale first upload (which is zeros if the
  first upload happened before the first fill). Note our earlier runtime patch that
  recovers stale page protections (`xmemory.cpp` AccessViolationCallback extension)
  lives adjacent to this machinery — a recovery that restores access *without*
  running the GPU watch callback would produce exactly this bug.
- All-zero DXT1 memory decodes to **opaque black** (endpoints 0/0, indices 0) —
  matching the symptom precisely.

## Root-cause hypotheses, ranked

1. **Guest writer never completes (composite memory stays zeroed).** The compositor
   is a multi-frame state machine (pending list → stream sources → async fill) with
   several wait conditions (`FableTexture_StreamTick` gates on frame counters,
   `Function_82ABAAE8` device state, busy flag `cRam83496e20`). Any recomp quirk
   (frame counter, async fill thread, VMX blend producing zeros) leaves the target
   memory zero-filled → DXT1 zeros = opaque black, *clothes unaffected*, hero+dog
   both affected (both use CustomAtlas), child+adult both affected. No GPU errors
   logged — consistent with everything we've observed.
   **Test:** `[morphup] ALL-ZERO BC upload` / `[morphbind] ZERO-CONTENT BC bind`.
2. **Host invalidation miss (stale upload).** First upload happens when the model
   first binds the composite (memory still zero) → game later fills the memory →
   write watch fails to invalidate (possible interaction with the
   stale-page-protection recovery fix) → host keeps sampling the zeroed upload
   forever.
   **Test:** `[morphbind] STALE BC bind` (guest hash ≠ last-uploaded hash).
3. **Content good + fresh, decode/sampler-side issue.** Least likely: identical
   formats work everywhere else; would need a composite-specific key edge case
   (pitch/packed-mips at these sizes) — the verbose `[morphbind]` dwords would show
   it. Only pursued if 1 and 2 both come back clean.

## Staged instrumentation (this session)

`rexglue-src/src/graphics/d3d12/texture_cache.cpp`, env-gated
**`FABLE2_MORPHBIND_DIAG=1`** (pattern follows `FABLE2_RESOLVE_DIAG`):

- **Upload side** (`LoadTextureDataFromResidentMemoryImpl`): for every BC-format
  (DXT1/DXT2_3/DXT4_5/DXN/DXT3A/DXT5A) ≤1024² base upload, hashes the first 4KB of
  guest memory and records it per base page:
  - `[morphup] ALL-ZERO BC upload base=... fmt=... WxH upload#N` (WARN) — writer
    hasn't filled the texture;
  - `[morphup] base=... upload#N hash=... nz=...` (INFO) — **`upload#>1` entries
    identify in-place-regenerated textures, i.e. the composites and their guest
    base pages.**
- **Bind side** (`UpdateTextureBindingsImpl`): for every BC binding, compares
  current guest memory against the last uploaded hash:
  - `[morphbind] ZERO-CONTENT BC bind ...` (WARN) — hypothesis 1 smoking gun;
  - `[morphbind] STALE BC bind ... mem=H1 uploaded=H2` (WARN) — hypothesis 2
    smoking gun (persistent repeats = real invalidation miss; a one-off can be a
    mid-frame race).
- The old verbose `[morphbind]` firehose still requires `FABLE2_MORPH_DIAG=1`.

**Build status:** `rexgpu-xenos` target rebuilt OK (BUILD_EXIT=0). Note
`build_runtime2.cmd` only builds `rexruntime` — the GPU plugin needs
`cmake --build rexglue-src/out/build/win-amd64 --config Release --target rexgpu-xenos`.
`rexruntime.dll` was NOT modified this session (no copy needed for it).

**Staging status:** the exe-dir auto-staging only covers `rexruntime.dll` (and only
on exe relink). The patched **`rexgpu-xenos.dll`** must be copied manually:
`rexglue-src\out\win-amd64\Release\rexgpu-xenos.dll` →
`Fable2Recomp\out\build\win-amd64-nightly\rexgpu-xenos.dll`. Fable2.exe was RUNNING
during this session, so a background stager (`stage_rexgpu_after_exit.ps1`) was
launched to perform the copy as soon as the game exits — **verify
`stage_rexgpu_after_exit.log` (root) shows matching hashes before the next run**,
or copy manually.

## Next test run (user)

1. Confirm `Fable2Recomp\out\build\win-amd64-nightly\rexgpu-xenos.dll` hash matches
   `rexglue-src\out\win-amd64\Release\rexgpu-xenos.dll` (see staging note above).
2. Run with `FABLE2_MORPHBIND_DIAG=1` (add `FABLE2_MORPH_DIAG=1` only if the
   verbose stream is wanted). Get to any black-skin scene (childhood caravan works).
3. Grep the newest `logs/Fable2_NNN*.log` for, in order:
   `"ALL-ZERO"`, `"ZERO-CONTENT"`, `"STALE BC bind"`, then `"upload#"` (>1).
   - ZERO hits persist → hypothesis 1: pivot to guest side (wrap
     `CustomAtlas_ProcessOnePending`/`FableTexture_StartAsyncFill` with weak
     overrides in `Fable2Recomp/src` to find where the state machine stalls).
   - STALE hits persist → hypothesis 2: dig `SharedMemory` write-watch vs the
     `xmemory.cpp` protection-recovery patch.
   - Neither, skin still black → hypothesis 3: use the `upload#>1` base pages to
     identify the composite, then a user-driven RenderDoc capture of that texture.

## Open guest-side leads (for a future decomp session)

- Pin the actual DXT-domain blend: follow `FableTexture_StartAsyncFill` 0x82A7A680 →
  `0x82B61180` / `0x82B60E20` and the callback table at `0x8331AB78`.
- `Function_8221EA48` (called once per compositor batch from 0x82181828) — likely
  frame-counter/begin marker.
- `Function_82A51A60` 0x82A51A60 — synchronous composite path used while the device
  is in states 1–4 (loading screens).

---

## ★ 2026-07-18 15:35 — LIVE VERDICT (user test run, log Fable2_491.log)

`FABLE2_MORPHBIND_DIAG=1` fired 1006 lines. **Hypothesis 1 CONFIRMED, hypothesis 2 ELIMINATED:**

- **`STALE`: 0** — the host texture cache never served outdated content. It even re-uploaded
  textures when their guest memory changed (`upload#2` events observed). The host is exonerated.
- **`ALL-ZERO` uploads: 6, `ZERO-CONTENT` binds: 149.** The dominant offender:
  **base=0x1B169 (page units), DXT1 (raw fmt 18), 768x704 — uploaded all-zero once, then bound
  with zero content 123 times (every frame)** = the hero skin composite atlas rendering black.
- **Recycle-then-never-fill proof:** bases 0x16667 / 0x166C7 / 0x16BA7 (512x512) had REAL content
  at `upload#1` (15:34:45, nonzero hashes) and were **all-zero by `upload#2`** (15:35:08) — the
  atlas manager reallocated/cleared those regions and the compositor never wrote the new pixels.

**Conclusion: the guest-side CustomAtlasTexture writer never completes its fill.** The bug is in
the recomp'd guest compositor chain (0x82181828 → FableTexture_StartAsyncFill 0x82A7A680 →
0x82B61180/0x82B60E20, callback table 0x8331AB78) or an async-streaming completion it waits on.
NEXT RE: decompile the async fill + completion callback; find the actual memory-write site and why
it never runs (suspects: an async file-stream completion that never fires in the recomp, a
callback dispatched through a codegen-missed indirect target, or a physical/virtual address
translation issue in the write path — cf. the XMA input-buffer bug class). The 768x704 DXT1
target at guest page 0x1B169 is the concrete texture to trace.

---

## ★ 2026-07-18 (late) — THE ASYNC-FILL CHAIN IS MAPPED (follow-up RE, +17 labels)

The "decompile the async fill + completion callback" task from the live verdict is done. Full
chain (all names now applied in Ghidra; batch `ghidra_out/labels_atlasfill.tsv`):

```
FableTexture_StartAsyncFill 0x82A7A680
  └ CustomAtlas_FillDispatch 0x82B60E20        (real body …0x82B6117C; was mis-bounded in Ghidra)
      ├ CustomAtlas_CanDownsampleCheck 0x82B61180 / TexturePool_CreateTexture 0x821E0D20
      │    (either failing ⇒ return 0 — fill silently NOT scheduled)
      ├ r8 = DAT_834A4364 (async fill mgr) NONZERO → AsyncFillQueue_Push 0x82B61B40
      │    (work item vtbl 0x820F70F8 "Procedural…", queue at mgr+8)   ← GAMEPLAY PATH
      └ r8 == 0 → loop CustomAtlas_SyncFillStep 0x82B61260 (cb table 0x8331AB78)
           ← the path that WORKS (no mgr yet ⇒ boot/loading screens)
Draining (who empties the queue):
  DeviceState_FrameDispatch 0x82388380 (per-frame, unconditional main path)
    └ TextureStreaming_UpdateAndPump 0x821C3858
        └ TextureStreaming_PerFramePump 0x822B4F38
            ├ ProceduralTexMgrList_TickAll 0x82191F50   — mgr list 0x8349F9F0, vtbl+0x14 tick
            └ Compositor_DrainToTarget 0x821811A8(0x8331AA88, memBudget)
                └ Compositor_ProcessOneItem 0x82A23880 (while pending *(this+0x24) > target,
                                                        cap 2 × *(this+0x20) per call)
  ProceduralTex_FlushAllBlocking 0x82A519B0 — blocking drain-until-empty; only caller of
    ProceduralTexMgrList_PumpUntilIdle 0x82A518D8 (vtbl+0x18); invoked from the dispatcher
    ONLY when flag this+0x84 is set (state transitions = loading screens).
```

Key facts established:
- The callback table 0x8331AB78 entry 0 is a DATA descriptor `{0x82BEB5E8, 0x82BEB440,
  "PresList"}`; entries +0x14/+0x1C/+0x24 point to fn-ptr descriptor triples at 0x820F6B58/68/74
  (targets 0x82A505B8, 0x828FD4A0, 0x82B81A90, 0x82A1AF90, 0x82A50608, 0x82B8AC08, 0x82B8AD30,
  0x82A50610, 0x82A50660, 0x82B8A8E0, 0x82B8AA30). **Every function in the visible chain is
  registered in the generated code** (checked one by one), and a full gameplay repro run with
  `FABLE2_DISCOVER_MISSING=1` (log Fable2_492.log) logged **zero [DISCOVER] lines** ⇒ the
  codegen-missed-indirect-target hypothesis is DEAD. Log 492 also re-confirmed the morphbind
  signature (hero atlas base 0x1B169 ALL-ZERO; 512×512s real→recycled-to-zero at upload#2; 0 STALE).
- The async fill mgr global DAT_834A4364: created by the fn ending 0x821D7FD8 (registers into
  mgr list 0x8349F9F0), destroyed by AsyncFillMgr_Shutdown 0x832B49A0. The mgr's own vtbl+0x14/
  +0x18 ticks (reached via the list walks) are what must complete the CPU fill.
- The per-frame pump path is UNCONDITIONAL in DeviceState_FrameDispatch ⇒ the stall is most
  likely INSIDE the mgr tick / drain logic (budget math from DAT_83496E2C/DAT_83321620 vs consts
  0x280000/0x2400000, or a per-item completion wait), not "pump never called".

**NEW TOOL (built + staged): `src/AtlasFillDiag.cpp`, env `FABLE2_ATLASDIAG=1`** — weak-override
wrappers (via the `__imp__sub_X` alias scheme) on all 8 chain functions logging live call counts,
async-vs-sync dispatch mode per fill, compositor pending count before/after each drain, and a
periodic `[atlasdiag] pump#N …` chain-state summary. Next run's log decides between:
(a) pump runs, pending>0, items never process ⇒ budget/target math or completion wait;
(b) dispatch returns 0 (downsample/CreateTexture fail) ⇒ fills never scheduled at all;
(c) enqueue counts grow, drain counts zero ⇒ the mgr tick isn't reaching the queue.

---

## ★★ 2026-07-18 23:30 — LIVE ROOT-CAUSE NARROWED (runs 495/496, AtlasFillDiag v2/v3)

Run 495: **18 consecutive `FableTexture_StartAsyncFill` FAILURES (ret=0) at the moment the
composites go black; never retried** (fail cohort fill-total = 0x7FFFFFFF uninitialized vs 4 for
successes). Run 496 with the bucket-check probe nails the discriminator:

- ALL 18 failures: `mipbits=00000000` — the resource's header word `*(res+4)+0x2C` is ZERO
  (mip count bits6-9 = 0) → `0x82B61180` returns 0 → fill dropped.
- ALL 3 successes: `mipbits=0x140/0x100` (5/4 mips), same dims (512x512), same bucket limit (3).
- NOT memory corruption: failing res 0x404DB038 and succeeding res 0x404DB4B8 share the SAME page.
  Per-OBJECT: the failing resources were never initialized (header unwritten, stream ptr NULL).
- Stale-page-recovery fix EXONERATED: it only re-`Protect`s (never zeroes), and its only event in
  the run was the known cultures page 0x70500000.
- The instant-permanent-black mechanic: `FableTexture_StreamTick 0x8225F9D0` computes elapsed from
  the resource's stream object (`*(res+8)+0x28 - this->0x18`); **stream NULL → elapsed=0xFFFFFFFF
  → passes the ≥3 attempt gate AND the >29 give-up gate in the same tick** → one failed SAF →
  `FableTexture_ReleaseResourceWrappers` → reported done → black forever.

**REMAINING QUESTION (the actual bug): why do these 18 composite source-resources get created
with no header and no stream in the recomp** (the "recycle-then-never-fill" cohort — they HAD
content at upload#1, then the atlas manager recreated them broken). NEXT: hook the resource
creation/init path — `TextureResource_InitHeaders` (who skips it), the atlas manager recreate
(0x82A5CAC8 → 0x82A5BCE8 → source-resource setup), and log res ptr + header word at create vs at
bucketchk. Also worth checking `FableTexture_StreamMips 0x821D7A98` mode (+0x8C) for these.

---

## 2026-07-19 00:50 — Pool Exhaustion Fix Verified (runs 502-507)

The header-less composite resources were downstream of texture-pool exhaustion. The failing path
is the compressed 1024x1024 pool bucket configured by `TexturePool_Init 0x82B60910` with
`entrySize=1,441,792` and `entryCount=16`. `TexturePool_FindBucketBySize` succeeds;
`TexturePool_AllocFromBucket` exhausts the bucket.

The purge-level retry shim (`FABLE2_POOLRETRY=1`) recovered some allocations but was incomplete.
The stable fix is to rewrite that single config row before pool memory is sized:
`(f0=36, w=1024, h=1024, mips=0, count=16)` -> `count=64`.

Runs 506 and 507 confirmed the fix: `ALL-ZERO=0`, `ZERO-CONTENT=0`, `pool-create FAIL=0`,
`bucket-alloc FAIL=0`, and `poolretry=0`. The patch lives in `src/AtlasFillDiag.cpp` and is
default-on; set `FABLE2_POOLBOOST=0` only for A/B comparison.
