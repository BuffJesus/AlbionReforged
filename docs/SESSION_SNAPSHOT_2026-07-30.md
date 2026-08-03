# Session snapshot — 2026-07-30 (black loaded world: RenderDoc pixel-history diagnosis)

## Resume here

The native D3D12 loaded world is still black (UI/heart/quest-text/glows only,
~19 FPS). This session did NOT fix the visual, but it produced the first
**decisive RenderDoc pixel-history evidence** of the mechanism and ruled out two
targeted fixes. Read this before touching the renderer.

Staged `rexgpu-native.dll` (built this session, contains the new opt-in flag,
default OFF so normal runs are unchanged):
`Fable2Recomp/out/build/win-amd64-nightly/rexgpu-native.dll`
SHA-256 `9BE34501D2133BA50BC6B2D511CA62E8BCC4839A171DC8414DA1C0ECC3944D31`.

## Decisive evidence (RenderDoc)

Capture: `ghidra_out/title_ui_re/renderdoc_black_world/fable2_frame2054.rdc`
(83 MB, the real black gameplay frame — contains the 1120x720 2xMSAA pair:
color id=154793 `R16G16B16A16_FLOAT`, depth id=154796 `D32S8`). The earlier
`fable2_frame1788.rdc` is only the 1280x720 loader — not useful.

Replay scripts (run with
`& 'C:\Program Files\RenderDoc\qrenderdoc.exe' --python <script>`; each writes a
txt beside the capture; qrenderdoc stays open after BlockInvoke — kill it):
- `renderdoc_pixhistory.py` → `pixhistory.txt` (color-target pixel history, 7 pixels)
- `renderdoc_shadercmp.py` → `shadercmp.txt` (VS disasm per event; constant-dump
  API was wrong — `PipeState.GetConstantBuffer` does not exist in this build)
- `renderdoc_blackpass.py` → `blackpass.txt` (identifies the recurring passes)

### Pixel history findings (color target 154793)

- Depth test is **reverse-Z GEQUAL, depth-writes on**.
- At terrain pixels a **stored depth is present before any draw touches it**
  (e.g. (560,360) preD=0.250616 from the very first tested event ev17128; no
  this-frame draw wrote it). It is coherent, non-uniform scene depth.
- **Almost every material/color draw arrives with a much smaller incoming depth**
  (0.003–0.016) and fails GEQUAL → `DEPTHFAIL`, so it never writes color.
- **Rosetta pixel (820,520):** stored depth 0.008611; material draws ev17170 /
  ev25102 recompute *exactly* 0.008611, pass, and write **real HDR color
  `[2.43,1.18,0.56]`**. So the pipeline/shaders are correct where the material
  pass agrees with the stored depth.
- The failing draw (ev17128) and the passing draw (ev17170) use the **same VS
  hash `7332bc5e`** — so this is **not** a per-shader translation bug. Same
  shader passes at one pixel, fails at another.
- `depth_float24` is uniformly false (`UseDrawProofFloat24PixelDepthConversion`
  returns false), so the 0.5-vs-1.0 viewport-MaxDepth asymmetry is NOT the cause.

### The recurring full-screen "black" events are CLEARS

ev16421, ev24353, ev34628 (which pixel history shows as passed / depth=-1 /
postCol=`[0,0,0]`) are **`ClearRenderTargetView`** calls, not draws — the opaque
black `kDrawProofClearColor`. So the frame is a sequence of **clear→replay
cycles** on the same 1120x720 color target. Where color succeeded (ev25102) was
an *earlier* cycle that the next clear wiped. After the **final** clear (ev34628)
the replayed draws (34978/35335/35377/43329) all fail (`scissor`+`DEPTHFAIL`) →
the target ends black → resolved/presented black.

## Root-cause model (best current understanding)

The gameplay **color resolve** carries its own reverse-Z depth prepass draws
(depth writes) followed by the material draws, and it **seeds a persistent depth
attachment from a prior frame** (log: `seeded color pass from persistent depth
prepass`, depth_base=1008=0x3F0, size 1120x720). Under GEQUAL, this frame's
inline prepass fails against stale nearer depth, so nothing writes → black.
Two contributing structures:
1. Stale seeded depth (proven: stored depth exists before any draw).
2. Clear→replay-every-resolve architecture: the presented cycle can end on a
   clear + a truncated/failing draw subset. This is the same "stop replaying
   historical populations" problem flagged in the 2026-07-29 snapshot
   (~1.43M retained draws).

## What was tried this session and did NOT fix the visual

New opt-in env flag **`REXGPU_NATIVE_GAMEPLAY_DEPTH_FRESH=1`** in
`rexglue-src/src/graphics/native/d3d12_pm4_backend.cpp` (+ `.h`). Two guarded
edits, both confirmed firing on the correct gameplay identity (depth base 0x3F0,
color dest 0x1A2B1000, 1120x720) via log lines
`gameplay depth prepass forced fresh` and `gameplay color pass depth seed
dropped`:
1. Depth-only 1120x720 prepass resolve: drop the stale persistent-depth seed so
   it clears to reverse-Z far each frame (near line 5713).
2. Color 1120x720 resolve with its own depth writes: drop the stale persistent
   depth seed so `clear_depth_target` fires and the inline prepass rebuilds
   (near line 5760, guarded by `color_depth_written`).

Result: **still black** (`ghidra_out/title_ui_re/depthfix/depthfix_v1_*.png`,
runs Fable2_1339/1340). Conclusion: stale seeded depth is a real sub-bug and is
now addressable, but it is not the sole cause. The flag is default-OFF and inert
unless set, so the staged DLL is safe; the edits are kept as scaffolding.

## ★★★ DEFINITIVE CONCLUSION (2026-07-30 late): prepass-VS vs material-VS clip-Z disagreement

After ~13 build/run iterations + 3 ultracode workflows, EVERY resolve/binding/depth-content
hypothesis is ELIMINATED by direct measurement, leaving one cause: a **shader-translation Z
disagreement** between the depth-only prepass VS and the material VS.

Eliminations (all measured, flag ON, run 1348-1349):
- Resolve ORDERING ✗ — per-frame order is depth->color->color every frame (ORDER log).
- BINDING / identity ✗ — per-draw `gameplay draw material` log: material draws bind
  bound_identity=1695 (SAME as the prepass writers), depth_created=false, clear_in=clear_final=false
  (NO clear), dsv_gate=2/3 (depth-test enabled), func=6/4 (GEQUAL/Greater), depth-write off.
- clear-of-seed / null-DSV ✗ — never fire for material draws.
- Depth CONTENT far ✗ — `debug depth readback statistics` for dest 0x1A2B1000 (a copy of 1695):
  finite=806400, **min=0.00235, max=0.25448, zero=0, half=0** = correct populated scene depth
  (same 0.002-0.254 range the very first pixel history saw).

So: material draws test GEQUAL against a correctly-populated shared depth resource, no clear — yet
they pass EVERYWHERE (overdraw). The only remaining explanation: the material VS computes clip-space
Z >= the prepass VS's stored Z for the same geometry, i.e. the two vertex paths DISAGREE on Z. This
also explains the ORIGINAL black world (material Z 0.003-0.016 << prepass 0.25 -> GEQUAL FAILS
everywhere -> black) vs the CURRENT overdraw (material Z now >= stored -> passes everywhere): same
prepass-vs-material Z mismatch, sign depends on what the material VS emits. Confirmed the game is a
depth-prepass renderer: 225 depth-only writers (func=6 GEQUAL, depth-write, color_wm=0) + material
draws (test-only, color_wm=F). The Xenos oracle (xenos_same_save_reference_live.png) shows the full
Bowerstone Cemetery indoor room; native shows only sparse fragments -> occlusion truly broken.

### PostVS measurement + capture-timing obstacle (renderdoc_postvs.py → postvs.txt)
Read the transformed SV_Position from RenderDoc PostVS:
- The depth-prepass draws produce **clip-z ≈ 0.098-0.101 (near-constant), w = view depth** —
  correct reverse-Z with an infinite far plane (NDC depth = z/w). Material draws (old 2054
  capture, VS 7332bc5e) similarly clip-z ≈ 0.100. So the projection STRUCTURE is correct;
  any prepass-vs-material disagreement is a small (~1-2%) clip-z delta, not a gross error.
- OBSTACLE: the native backend renders via per-resolve replays, so a RenderDoc capture
  taken at depth-resolve time (`fable2_frame1674.rdc`, F12'd 2s after the depth resolve)
  contains ONLY the wmask=0 depth-prepass draws (all dwrite=True); the wmask=F material
  draws live in the separate color-resolve replay. So prepass and material draws are NOT
  in the same capture and cannot be paired from one .rdc with the current capture timing.

### TWO remaining hypotheses (need a properly-timed flag-ON pixel history to distinguish)
With depth CONTENT correct (0.0023-0.2544) and binding correct, the world still isn't the
reference scene. Either:
1. **VS-Z disagreement**: near-surface material draws compute a depth that doesn't match the
   prepass's stored depth, so they fail GEQUAL and the near surfaces (walls/floor/chest)
   never render. (Depth-gate.)
2. **Material draws render but write BLACK**: geometry occludes correctly but the material
   pixel shaders/textures output black (cf. the known black-skin/black-texture issue,
   [[fable2-adult-hero-black-skin]]). (Color/texture, not depth.)
Both fit the current evidence. DISTINGUISH by pixel-history on a flag-ON capture at a
near-surface pixel: if the near-surface material draw PASSES depth but postCol is black =>
(2); if it DEPTHFAILs => (1).

### TOOLING obstacle (be aware next session)
Both the RenderDoc -RenderDocCapture and the in-engine debug readback keep landing on the
WRONG resolve: captures at depth-resolve time contain only wmask=0 prepass draws; the debug
readback (REXGPU_NATIVE_DEBUG_RESOLVE_BASE + MIN_SEQUENCE=1) catches the 1280x512 LOADING
screen resolves, not the 1120x720 gameplay world. To capture gameplay: set
REXGPU_NATIVE_DEBUG_RESOLVE_MIN_SEQUENCE to a gameplay sequence (~640000+, the range the
`gameplay ... resolve` logs show) so the readback/capture fires on the real world resolves.
For a paired prepass+material capture, F12 during a color resolve or a present refresh that
replays both — not right after the depth resolve.

### NEXT: get a flag-ON capture that contains the material (color-resolve) draws
Fix the capture timing so F12 fires during a COLOR resolve (or a present refresh that
replays both), OR add an in-engine per-pixel readback of the color target after the color
resolve. Then pixel-history a wall/floor pixel. Only after (1) vs (2) is settled does the
fix location follow: VS-Z translation (rexglue-src translator) for (1), or the material
pixel-shader/texture path for (2). renderdoc_postvs.py auto-picks the newest capture and
the 1120x720 2xMSAA color target by properties.

### (earlier framing) compare the prepass VS and material VS translated Z for one matched draw
Use RenderDoc (renderdoc_shadercmp.py, extended) OR the Xenos->DXBC translator: for one object,
dump the depth-only prepass VS and the material VS SV_Position.z math + their vertex constants, and
find why they differ (e.g. a viewport Z scale/offset, W-divide, half-pixel, or constant-register
difference applied to one path but not the other). This is a shader-TRANSLATION bug in
rexglue-src (the VS Z path), NOT a resolve/depth/backend-state bug. All backend-state avenues are
exhausted. The gameplay_depth_fresh_ flag + diagnostics are correct scaffolding but the fix is in the
vertex-shader Z translation. Staged dll is a flag-gated DIAGNOSTIC (default OFF).

## Cycle-structure ground truth (renderdoc_cycles.py → cycles.txt)

The black frame's command stream on color target 154793 has **4
`ClearRenderTargetView`** (ev 16421, 24353, 34628, 44644) splitting it into
full-scene cycles:
- seg0 (pre-clear): 434 draws, 0 to 154793 (other targets).
- seg1 (after 16421): 243 draws, all to 154793, 118,684 indices.
- seg2 (after 24353): 261 draws, all to 154793, 142,685 indices.
- seg3 (after 34628): 282 draws, 265 to 154793, 142,749 indices. **← presented cycle**
- seg4 (after 44644): 34 draws, 0 to 154793 (tiny, other targets).

**seg3 is a superset of seg1**: positional index-count match = 243/243, multiset
common = 243. So the three cycles are **redundant re-renders of the same growing
retained draw set** (243→261→282 as the pending set accumulates), NOT distinct
accumulating sub-passes. The presented cycle is seg3 (the most complete). Guest
resolves log `clear=false/false`, so the per-cycle **color clears are the
BACKEND's own** (`clear_color_target`), and **depth is NOT re-cleared per cycle**.
Pixel history proves seg1's draws PASS and write color (ev17170/25102) while the
SAME draws in seg3 FAIL depth — i.e. the depth buffer state diverges across the
redundant replays (color cleared each cycle, depth carried/accumulated), so by
the presented seg3 the geometry fails GEQUAL and the (freshly color-cleared)
target ends black. Fixing depth for only the FIRST cycle (what the flag did)
cannot help because the PRESENTED cycle is the third.

## ★ SMOKING GUN (renderdoc_scissor.py → scissor.txt): presented cycle is a 1120x144 tile

Dumping the D3D12 scissor rect per draw for seg1 vs the presented seg3:
- **SEG1** (earlier cycle, has passing color draws): scissor **(0,0,1120,720)** — full frame.
- **SEG3** (PRESENTED, highest-sequence): scissor **(0,0,1120,144)** — top 144 rows ONLY.

So the presented cycle re-renders the same geometry but **scissored to a 1120x144 top
strip**; rows 144–720 are never drawn and stay at the opaque-black companion clear. This
is Fable's **EDRAM tiling** of the 1120x720 2xMSAA HDR target (the 2026-07-29 snapshot
already noted "576-line upper tile followed by a 144-line lower tile"). The black world is
therefore **NOT depth and NOT shader** — it is a **partial-tile present/compositing bug**:
the final resolved/presented HDR contains only one small tile's geometry instead of the
composited full frame. This matches the workflow's adversarially-verified root cause #1
(last-wins Present selection at ~6909-6914 picks the last partial-tile resolve) and #2
(per-cycle opaque-black companion clear at 5875-5877). The `[scissor,DEPTHFAIL]` flags seen
in pixel history at center pixels are the scissor rejecting everything below row 144.

The prior depth-fresh flag was a dead end for the visual (depth is at most secondary). The
correct fix is in the resolve tile placement / Present selection / companion-clear, NOT depth.
The workflow's finalized recommendation (docs + task output wunj6oltb) is: track which resolve
wrote real color / has full coverage, prefer it (or composite tiles by Y-offset) in Present,
and suppress the redundant black companion-clear for the gameplay identity. Refine "wrote_color"
to "coverage" since the 144-tile also writes color in its strip — discriminate by scissor/resolve
rectangle height or by not letting a partial tile overwrite the full-frame resolved entry.

## ★★ MILESTONE: geometry appears — depth WAS gating the upper tile (run 1341)

Broadening the color-pass gate from `height==720` to the real tile heights
(`576 || 144`) and dropping the `color_depth_written` requirement — so the tile
color resolves drop their stale depth seed and clear depth to reverse-Z far —
produced the **first non-black gameplay**: `depthfix_v1_t173.png` shows real
geometry fragments across the upper ~third of the screen (was a flat black
gradient). FPS 11.

Conclusions:
- **Depth was genuinely gating the upper 576-row tile** (the bulk of the
  compositor input at 0x19C67000). The adversarial verifiers dismissed depth by
  focusing on the small 144-row LOWER tile (which is also scissor-limited); they
  were wrong about the dominant upper tile.
- The tiles were seeding **stale** depth (black at the 720-only gate); the real
  color tiles are **576 and 144 rows**, which the earlier `height==720` gate
  never matched — a concrete gate bug now fixed.
- The result is **over-drawn/blurry (no occlusion)** because the tile color
  passes have NO inline depth writers (`is_submittable_resolve_draw` at
  ~5841-5849 drops textureless depth-prepass draws), so clearing depth to 0 makes
  GEQUAL pass everything. This is a DIAGNOSTIC state, not a shippable image.

### ★ Depth-state ground truth (renderdoc_depthstate.py → depthstate.txt)
EVERY gameplay draw has **depthEnable=True, depthWrites=True, GEQUAL** (some also
stencilEnable). So this is a **single-pass forward renderer** — there is NO
separate depth-only prepass; each draw writes color AND depth. Consequence:
GEQUAL + depth-write is order-independent-correct for opaque geometry, so clearing
depth to reverse-Z far (0.0) ONCE per tile cycle SHOULD self-occlude with no
prepass. run 1341 over-drew anyway, which means the native tile replay is NOT
honoring depth-write (PSO depth-write off / DSV not bound / depth re-cleared
per-draw), OR the clear value is wrong. That — not "preserve the prepass" — is the
occlusion fix to find. (Workflow wt19alpc1 is tracing depth_control->PSO depth-write
and the clear-value derivation at ~8734/8777.)

### Occlusion attempts (runs 1342-1343) — narrowed to "prepass leaves no depth"
Two ultracode workflows (wt19alpc1 + the tile analysis) plus builds established:
- The gameplay color RENDER TARGET is **720**, not 576/144. The 576/144 tiling is
  in the **scissor + resolve rectangle**, not the RT height. So color and depth
  are BOTH 720 and already share a `draw_depth_target_` resource (base 0x3F0). The
  earlier "separate 576/144 depth" theory (and a depth-key-height decouple) was
  WRONG — the decouple log never fired.
- The game is **single-pass forward**: every draw has depthEnable/depthWrites/
  GEQUAL (depthstate.txt) — BUT the color resolve's material draws are depth-TEST
  -only after the write_mask==0 depth-prepass writers are erased (erase_if ~5229)
  into the separate source_is_depth 720 depth resolve.
- **run 1342/1343 (flag ON): REUSING the shared 720 depth (seeded_depth_prepass=
  true, no clear) STILL over-draws** (blurry upper third, no occlusion, FPS 11-24).
  Since reuse (not clear) still passes everything, the shared depth is **FAR (~0)
  at color-read time** — i.e. the 720 depth PREPASS is NOT populating valid scene
  depth into `draw_depth_target_` that the color resolve reads. Likely causes:
  the depth prepass resolve is DRAWLESS (my forced-fresh clears it to 0 but no
  writers render), OR it resolves AFTER the color pass, OR into a different keyed
  resource (msaa/format). This is the next thing to verify.

### ★ Diagnostic result (run 1345): depth prepass DOES render 225 draws, shares identity
The `gameplay 720 resolve submitted draws` log proved:
- source_is_depth=true: **submitted=225, submittable=225, total=225, depth_identity=1695**
- source_is_depth=false (color): submitted=232, total=232, **depth_identity=1695 (SAME)**
So the depth prepass is NOT drawless — it renders 225 write-mask-0 depth writers into
the SAME depth resource (identity 1695) the color resolve reuses. Yet reuse STILL
over-draws (pass-all). Therefore, despite a populated shared depth buffer, the color
material draws' GEQUAL passes everywhere. Remaining candidates:
1. ~~ORDERING~~ **REFUTED (run 1346, `gameplay 720 resolve ORDER` log):** the per-frame
   order is **depth → color → color** every frame (e.g. #1 depth seq660969/225draws →
   #2 color seq661227/232 → #3 color seq661484/232; #4 depth seq662376/236 → #5/#6
   color), all depth_identity=1695. So the depth prepass ALWAYS runs before the two
   color tile resolves and populates the shared resource first. Ordering is fine.
2. **CONFIRMED (run 1348, per-draw `gameplay draw WRITER/material` log): the
   BINDING is fully correct; the cause is DEPTH CONTENT in resource 1695.**
   - WRITER (prepass) draws: func=6 (GEQUAL), depth-write on, color_wm=0,
     bound_identity=1695, cleared-to-far on first draw then written.
   - MATERIAL (color) draws: bound_identity=1695 (SAME), depth_created=false,
     clear_in=clear_final=false (NO clear — prepass depth preserved), dsv_gate=2/3
     (test enabled), func=6/4 (GEQUAL/Greater), color_wm=F, depth-write off.
   So material draws test GEQUAL against the SAME depth resource the prepass wrote,
   with no clear — textbook-correct, should occlude. This ELIMINATES ordering (A),
   identity divergence (C-identity), clear-of-seed (C-clear), and null-DSV
   (C-null-DSV). The ONLY remaining cause is **(B): resource 1695's depth CONTENT is
   far/empty** despite 225 GEQUAL depth-write draws — the prepass writers aren't
   establishing near depth (candidate: translated VS emits z≈0, or a 2xMSAA depth
   write/read subtlety, or the depth clear value/viewport-MaxDepth for depth_fmt=1
   float24 mismatches the writers' range). NEXT = read back resource 1695's depth
   VALUES after the prepass (RenderDoc capture with flag ON, or an in-engine depth
   sampler): if uniformly ~0/far, it is a prepass depth-WRITE content bug; compare
   to the writers' expected SV_Position.z. All binding hypotheses are exhausted.

   (superseded) earlier candidate: writeback/aliasing resets 1695 — The depth resolve renders 225 draws into
   1695, then does its EDRAM->guest resolve/persistent-store writeback; if that
   transitions/aliases/clears 1695, the two following color resolves read far depth ->
   pass-all. Also possible: the 225 write_mask==0 draws don't actually carry depth-write,
   or a depth clear-value/viewport-MaxDepth issue. (Workflow wnjsekk40 is tracing B/C;
   confirm with a RenderDoc readback of identity-1695 content right before a color draw.)
The staged flag-gated build now REUSES the shared depth (no clear) — still overdraw,
confirming the depth read is far/empty at color time. Next session: capture with flag
ON, readback depth 1695 at color-resolve time.

### NEXT (superseded — verify why the depth prepass leaves no depth)
Instrument the source_is_depth 720 resolve (base 0x3F0): log its submitted draw
count (does it render the write_mask==0 writers or is it drawless/replays_persistent
_edram?), its `draw_depth_target_identity_`, and compare to the color resolve's
`draw_depth_target_identity_` in the same frame + their submission ordering. If the
prepass is drawless or ordered after color, that is the bug. Then either force the
depth writers to render in the depth resolve before the color resolve, or keep the
write_mask==0 depth-prepass draws IN the color resolve (revert the 5229 erase for
the gameplay identity) so the color pass rebuilds depth inline before its material
draws. Current staged dll SHA `99F40C1E…` (flag-gated diagnostic overdraw, default OFF).

### The correct next fix (occlusion) — superseded framing (kept for history)
The tile color passes must test against **this frame's correct depth**, not a
stale seed and not a cleared-to-0 pass-all. Options, best first:
1. Generation-correct seed: make the tile color resolve seed the depth produced
   by THIS frame's 720 depth-prepass resolve (the `edram_targets_.find(depth_key)`
   lookup has no frame/generation filter — root cause from the workflow's
   depth-generation investigator). Tag depth attachments with a frame generation
   and only seed a same-generation source; otherwise clear.
2. Shared depth (Xenia model): bind ONE host depth resource across the 720 depth
   prepass and the 576/144 color tiles within a frame, cleared once at frame
   start, no copy-seed. (render_target_cache.cc:879-948 reference.)

Current staged `rexgpu-native.dll` (flag-gated DIAGNOSTIC, default OFF) SHA-256
`A448D9F19A2D60938FC32F5C8CE5529A7A29BC61F70C43EDD511B270349A692E`. With the flag
ON it produces the pass-all overdraw above; keep it OFF for normal runs.

## Next experiments (in priority order)

1. **Confirm depth actually clears in the presented cycle.** Re-capture with the
   flag ON and re-run `renderdoc_pixhistory.py` on (560,360)/(820,520). If stored
   depth is still ~0.25 with the flag ON, the presented resolve is a *different*
   depth generation than the one the flag cleared — chase which resolve/identity
   the final present consumes.
2. **RenderDoc pixel history on the FINAL present frame's own draws**, not the
   whole concatenated command stream. Determine whether the last clear→replay
   cycle simply lacks the scene draws (truncation / wrong resolve chosen for
   present) vs. has them but they fail depth.
3. **Attack the clear→replay architecture** (snapshot 2026-07-29 step: record
   each guest draw once into an explicit host pass/attachment generation; lower
   resolves to graph edges). The multi-clear structure is likely why even a
   correct depth doesn't survive to present.
4. Only after the world renders: the ~19 FPS retained-replay perf and the
   Lionhead white-flash remain open.

## Harness / tools

- `Fable2Recomp/tools/depthfix_confirm.ps1 -Value 1 -Seconds 190` runs the
  Hero001 loadsave arm (native) with the flag set and captures desktop shots
  every ~12s to `ghidra_out/title_ui_re/depthfix/`. Hero001 loads straight to
  Bowerstone Cemetery ("Community Service" quest) with NO NewBeginnings.bik.
- Build: `rexglue-src/build_native.cmd` (target `rexgpu-native`, VS2022 + Clang,
  BUILD_EXIT=0, no `__std_find_` link error this session). Then copy
  `rexglue-src/out/win-amd64/Release/rexgpu-native.dll` over the nightly one
  (not auto-staged — only rexruntime.dll is).
- Focused native tests (`pm4.*` + `native_plugin_smoke`) were NOT re-run this
  session; the flag is additive/default-off so they should be unaffected — run
  them before declaring any productized fix.
