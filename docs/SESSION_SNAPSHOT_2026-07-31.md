# Session snapshot — 2026-07-31

## ★★★★★★ LATE NIGHT (2026-07-31) — EXPOSURE IS DEAD. The tonemap's HDR INPUT is ~0. Native-first directive. (current)

**Read memory `fable2-blackworld-compositor-exposure` UPDATE 15 first.** Everything below the daytime
`UPDATE 2` heading is now historical — the whole exposure/LUT/NaN/tfetch1D line is a **refuted dead end.**

### The decisive test that closed the exposure branch
Hardwired the compositor exposure LUT (`0x1FC20000`, 512x1 R32F) to a **constant 8.0** for every entry
(log `Fable2_1399` confirms engaged: `loaded_world LUT min=max=mean=8`). If the tonemap sampled the real
resolved HDR (`0x19C67000`, RenderDoc mean **0.317**), then `color = saturate(8 * 0.317) = 1.0` everywhere
→ a blindingly white scene. **Actual result (measured with PIL, not eyeballed): gameplay-region mean =
0.17/1.0, only ~32% of pixels bright — identical to the ramp run and to plain black+bloom.** An 8x multiply
did *nothing*.

⇒ **The HDR the tonemap actually samples (slot tf0) is itself ~0.** You cannot brighten a black input.
Exposure / LUT magnitude / tone curve / tfetch1D coordinate are ALL downstream red herrings. This kills
~50 runs of exposure theories in one measurement.

### What is actually going on (the reconciliation)
RenderDoc sees a REAL 0.317 scene at the RESOLVED buffer `0x19C67000`, but the compositor's tf0 input is
dark (~0.08 or less — matches the old UPDATE 6 "compositor binds HDR mean 0.08"). **So the tonemap binds
the WRONG HDR resource / wrong generation / wrong region as tf0 — not the fresh fully-resolved scene.**
This is a resolve-target / present-ordering / SRV-binding bug in the native backend, NOT exposure.

### The terminal next step (native — one shot, no more guessing)
At the tonemap draw, log the **base address + a GPU-readback mean** of the host texture bound to the **HDR
SRV slot (tf0)** — not exposure, not depth:
- `tf0 base != 0x19C67000` → wrong resource bound → fix the SRV resolution for the compositor HDR input.
- `tf0 base == 0x19C67000 but content ~0` → right address, wrong **generation** (stale/pre-resolve) → fix
  resolve/present ordering so the compositor samples the peak-16 scene.
Then diff against what **Xenia** binds for the same compositor draw (Xenia = the correct reference; our
black is NOT a Fable-2 quirk — GummiFableII + femtofork ship NO tonemap fix).

### Methodology fix (why we circled)
Every one of the ~50 dead runs was a **theory tested by eyeball**. The rule now: **every hypothesis dies to
one A/B measurement (native vs xenos), never an eyeball.** The const-8 test is the model.

### Code / build state (uncommitted, rexglue-src)
- **KEEP**: `d3d12_pm4_backend.cpp:3543` host-texture fast-path now re-validates per-range
  `memory_generation` for small (<=64KB) sources (stale-snapshot correctness fix; safe, didn't fix black).
- **KEEP (default-OFF)**: env flags `REXGPU_NATIVE_FORCE_EXPOSURE_ONE` (LUT=1.0) and
  `REXGPU_NATIVE_EXPOSURE_RAMP` (LUT[i]=i/511), sanitize block ~`:8892` (+ `.h` ~789). Source is REVERTED
  to clean env-gated form (the temp `exposure_ramp_=true` hardwire + const-8.0 have been removed).
- ⚠ The **staged nightly DLL is the const-8.0 DIAGNOSTIC build** (`rexgpu-native.dll` SHA `0d7bbfd0`).
  **Rebuild from the now-clean source before playing** (`rexglue-src/build_native.cmd` → copy over nightly).
- ⚠ **Env diag flags do NOT reach the game through `winshot_confirm.ps1`'s `Start-Job`** — hardwire the
  flag or launch `artifact_repro.ps1` directly (it `Start-Process`es, inheriting parent env).

### Tooling proven this session
`winshot_confirm.ps1 -Value 0` (depth-fresh OFF) + a PIL mean-brightness measure of the gameplay region =
the reliable A/B (eyeball fails — bloom smear reads as "scene"). `artifact_repro.ps1 -Arm loadsave
-GpuPlugin native -TargetMorphs 48976` = the Hero001 black Bowerstone repro. The in-engine "compositor
exposure texture diagnostic" logs the actual uploaded LUT curve.

### User directive
**Native renderer is the goal — not a pivot to xenos.** Decomp does NOT accelerate this backend bug
(reference is Xenia, not the game's code); decomp is for modding systems. The native path forward is the
tf0-HDR-bind measurement above.

---

# Session snapshot — 2026-07-31 (black loaded world REDIRECTED: it's the COMPOSITOR/EXPOSURE, not depth) [HISTORICAL below]

## ★★★★ UPDATE 2 (2026-07-31 late): exposure LUT is written EVERY FRAME by the CPU with NaN — memexport is NOT the producer

Two more investigations refined the root cause further (staged DLL SHA-256
`DBA78A30087EDA0822DC15330A86E1F3B04956D7EDCDFBB6ACDCA143BC873736`, 1,706,496 bytes — adds only
default-OFF / log-only diagnostics; normal runs unchanged):

**A. Memexport IS dropped by the native backend, but it is NOT the exposure producer.**
- Added memexport detection (shader + per-draw). Result: exactly ONE memexport shader exists — a
  **vertex shader** (`hash=5B4A74595AD90328`, key `7615ACD6BABE4894`, `eM_written=01`). Its draws
  DO reach `SubmitDrawProof` during the loaded world (so the backend *can* execute them), but the
  backend currently binds no UAV and does no writeback, so **all memexport writes are silently
  dropped** — a real feature gap.
- Its 11 distinct export targets are all `0x1BBF____`–`0x1BC2____`, format **38 =
  `k_32_32_32_32_FLOAT`** (float4 arrays of 9/18/30/36 elements) — GPU transform/skinning-style
  data. **`0x1FC20000` (the exposure LUT) is NEVER a memexport target.** So implementing memexport
  will NOT fix the black world (it may fix other artifacts). Per user direction, memexport
  implementation was deferred in favor of hunting the real exposure producer.

**B. The exposure LUT `0x1FC20000` is written EVERY FRAME by the CPU, with NaN content.**
- Sampled the guest memory GENERATION of `0x1FC20000` at loaded-world compositor draws
  (`memory_generation_reader_`). Sample 0: `lut_generation=0` while `global=131787`; sample 1
  onward: `lut_generation` jumps to 131788 and then **increments every frame, tracking the global
  generation**. So the LUT range IS being written continuously (the earlier "static content hash"
  was because it writes the *same* NaN every frame). Since NO GPU resolve/memexport targets
  `0x1FC20000`, the writer is the **recompiled game CPU**.
- ⇒ **The CPU tonemap/exposure producer RUNS every frame but computes NaN** (not "never runs"). The
  −NaN (`0xFFC00000`) uniform curve most likely comes from a **divide-by-zero / domain error** on a
  bad input — probably a **zero or NaN average scene luminance** (auto-exposure adaptation), which
  the CPU reads from a small GPU-downsampled/readback luminance target that our backend may be
  producing as 0/NaN.

### NEXT (real fix — the CPU tonemap producer + its luminance input)
1. **Find the CPU tonemap/exposure function** that writes the 512-float curve to `0x1FC20000`
   every frame (Ghidra: search for the auto-exposure/eye-adaptation system; the compositor binds
   the exposure texture at fetch base `0x1FC20000` — trace the code that fills it). cdb is NOT
   installed on this machine (checked), so a write-watch needs either installing WinDbg/cdb or a
   VEH page-guard on host `0x11FC20000` (membase `0x100000000`) logging the faulting RIP →
   symbolize via `Fable2.map`.
2. **Find the average-luminance input** the CPU reads (a small GPU readback/locked target). If it
   is 0/NaN in guest memory, that is the divide-by-zero source — and its producer (a luminance
   downsample resolve/readback) is the actual GPU-side break. Fix the luminance so the CPU curve
   is finite.
3. The `REXGPU_NATIVE_SANITIZE_EXPOSURE=1` flag remains the confirmation/stopgap lever (flat 1.0
   makes the world visible but not correctly tonemapped).

Diagnostics added this session (all default-OFF or log-only, in the staged DLL): `MEMEXPORT shader
captured` / `MEMEXPORT draw target` (per-draw memexport enumeration), `exposure LUT 0x1FC20000
generation sample` (loaded-world compositor generation sampler). Safe to keep as scaffolding.

## ★★★ Headline (read first) — ROOT CAUSE CONFIRMED END-TO-END

The black loaded world is **NOT** a depth / occlusion / coverage / VS-Z bug. After ~13
prior iterations chasing depth, this session proved **empirically** that the world
geometry renders **correctly** into the HDR buffer, and the frame is black because the
final **compositor / tonemap / exposure** pass crushes that real HDR scene to near-black.

**CONFIRMED root cause:** the final-compositor **auto-exposure LUT** at guest **0x1FC20000**
(512×1 R32_FLOAT) is **entirely NaN** (guest memory holds big-endian −NaN; readback logged
`entries=512/512 non-finite`). The compositor microcode does `mul_sat(exposure * color)`, and
**`saturate(NaN) = 0`** zeroes the tonemapped scene, leaving only the bloom term — exactly the
observed near-black frame + glow orbs.

**End-to-end confirmation (this session):** a flag-gated diagnostic
`REXGPU_NATIVE_SANITIZE_EXPOSURE=1` that overwrites the LUT's non-finite entries with 1.0 at
upload made the frame **visibly change** — real scene structure (pillars, horizon, ground
detail, defined glows) appeared where before there was a featureless flat gradient
(`ghidra_out/title_ui_re/depthfix/win_v1_t203_t203.png`). Still dim because a flat 1.0 is not
the game's adaptive tonemap curve, but the causal chain is proven.

**Every depth-centric hypothesis is refuted by measurement. Stop working depth. The REAL fix
is the exposure-LUT producer (why 0x1FC20000 is NaN), NOT the sanitizer (a diagnostic).**

## The decisive evidence chain (all this session)

1. **Reconciliation ultracode workflow (wb0ip8ntb, 5 agents)** read the exact backend code
   paths and refuted VS-Z translation, depth-target lifetime, and depth-resolve writeback
   against the measured constraints (single shared VS Z path; depth cache-hit preserves
   content; balanced writeback barriers). Its *fallback* (composite coverage loss) is ALSO
   refuted below.

2. **Tile-mapping diagnostic (NEW, added this session; safe log-only, gated behind
   `gameplay_depth_fresh_`)** — `gameplay resolve MAP` log proves the per-frame tile layout
   and that **coverage is COMPLETE**:
   - Per frame, 3 resolves: **depth** `1A2B1000` 1120x720; **upper color** `19C67000`
     copy `1120x576+0,0`; **lower color** `1A153000` copy `1120x144+0,576`.
   - `0x1A153000 ≈ 0x19C67000 + 576·1120·8` — the lower tile is the **continuation** of the
     same linear guest surface. Its resolve logs `seed_found=true, reused=true`: it folds
     back to root `0x19C67000` and writes rows 576–720 **into that shared texture**.
   - ⇒ The `0x19C67000` resolved texture gets **full 720 coverage every frame**. The
     "coverage-blind last-wins replace drops rows" hypothesis does **not** occur. A coverage
     edit would have been a no-op. (Instrumenting first instead of applying that edit blind
     was the right call.)

3. **Fixed game-window capture** (`Fable2Recomp/tools/winshot_confirm.ps1`, NEW — foregrounds
   the Fable2 window and crops its client rect; the old `depthfix_confirm.ps1` grabbed the
   whole desktop and captured the IDE, not the game). Real ground truth: the loaded world is
   a **flat near-black gradient** with only UI (quest text / heart / "Search" prompt) and
   additive **glow particles**; **zero opaque geometry** visible.
   (`ghidra_out/title_ui_re/depthfix/win_v1_t199_t199.png`.)

4. **Depth-always test (no rebuild):** `REXGPU_NATIVE_DEBUG_GAMEPLAY_COLOR_DEPTH_ALWAYS=1`
   forces the gameplay material draws' depth comparison to ALWAYS-pass (log confirms
   "gameplay color-pass depth comparison forced to always"; per-draw `func=7`). The frame is
   **IDENTICAL** — still flat near-black. ⇒ **Depth is NOT the gate.** This refutes the
   entire depth-rejection / occlusion / VS-Z line.

5. **Per-draw log:** every gameplay frame submits **229 depth writers** (`color_wm=0`) then
   **236 material draws** (`color_wm=F` full color write, `submitted=236 submittable=236
   total=236` — none dropped). So the geometry draws reach the backend, pass, and write color.

6. **In-engine readback of `0x19C67000` (no rebuild:** `REXGPU_NATIVE_DEBUG_RESOLVE_BASE=19C67000`
   `REXGPU_NATIVE_DEBUG_RESOLVE_MIN_SEQUENCE=653000`) — the compositor's HDR input:
   - `changed 645120 pixels` = **all** 1120x576 upper-tile pixels written; later frames
     530k+ changed, `bounds 0x0..1119x575`.
   - First-pixel half-floats `3C00387B3CBB40DB` → RGBA ≈ **(1.0, 0.56, 0.97, 2.93)** — real
     non-black HDR color.
   - The dumped BMP (`rexgpu_native_resolve_19C67000_08.bmp`, converted to
     `ghidra_out/title_ui_re/depthfix/hdr_19C67000_08.png`) **visibly shows the rendered
     scene** — the Hero character holding a weapon, a green interaction glow, environment
     structure, the 'A' prompt — blown out to yellow only because the dump applies no
     tonemap/exposure (HDR > 1, alpha 2.93).

**Conclusion:** the world renders correctly into `0x19C67000`; the black frame is produced
**downstream** by the compositor/tonemap/exposure pass.

## Mechanism (confirmed): NaN exposure LUT → saturate(NaN)=0 → black + bloom-only

The final compositor PS is `FBE91459C01C61BF` (dump: `../compositor_shader_dump_1177/
shader_FBE91459C01C61BF.ucode.frag`). Its inputs (from the `loaded-world final compositor
state` log): slot 0 = HDR `0x19C67000` (1120×720), slot 1 = **exposure LUT `0x1FC20000`**
(512×1 R32_FLOAT, sampled with `tfetch1D`), slot 2 = bloom `0x0C786000` (280×180). The tonemap:
```
tfetch2D r2.xyz, tf0            ; HDR scene
dp3      r0.z,  r2, c255        ; L = luminance(HDR)
mulsc    r1.w,  c46.x, r0.z     ; LUT coord = c46.x * L
tfetch1D r0.x,  r1.w, tf1       ; exposure = ExposureLUT[coord]   ← 0x1FC20000 (NaN)
mulsc    r0.w,  c77.y, r0.x     ; r0.w = c77.y * exposure          = NaN
mul_sat  r0.xyz, r0.wwww, r0    ; saturate(NaN * color) = 0        → scene BLACK
mad      r1.xyz, -r1, r0, r1    ; bloom*(1 - 0) = bloom
add      r0.xyz, r1, r0         ; result = bloom + 0               → only BLOOM survives
... log/mul c78/exp             ; gamma
```
`saturate(NaN) = 0` on D3D hardware, so an all-NaN LUT zeroes the tonemapped scene and the
frame is just the bloom (the glow orbs). This matches the captured frame exactly.

**Confirmed** by `REXGPU_NATIVE_SANITIZE_EXPOSURE=1` (this session's flag-gated diagnostic):
overwriting the 512/512 non-finite LUT entries with 1.0 made scene geometry appear
(`win_v1_t203_t203.png`). ⚠ The 2026-07-29 "don't pursue 512 NaNs" note was for the *peach*
state (bloom clear-color); the *black* state is a different, now-confirmed lead.

## NEXT (in priority order) — attack the exposure-LUT PRODUCER (the real fix)

1. **The producer is almost certainly a DROPPED MEMEXPORT.** `0x1FC20000` is NOT a color/depth
   resolve (no resolve log touches it) and is served from the host texture cache with a
   **static** content hash (`5ABE336E4F359325`) — i.e. it is never re-written. Decisive code
   finding: **the native PM4 backend (`src/graphics/native/`) has ZERO memexport support** —
   grep for `memexport|export|uses_memexport|stream_constant` in `d3d12_pm4_backend.cpp` /
   `native_graphics_system.cpp` finds no execution path (only the narrow `memory_writer_` used
   for the player/dog atlas readback-resolve). The upstream Xenia-derived tree DOES have it
   (`pipeline/shader/dxbc_translator_memexport.cpp`, `spirv_translator_memexport.cpp`, and
   memexport handling in `d3d12/command_processor.cpp`), but the from-scratch native backend
   does not. So Fable's auto-exposure/tonemap-LUT pass (a shader that memexports computed
   values to guest `0x1FC20000`) is silently dropped → the address stays uninitialized guest
   RAM (=−NaN) forever → black world.
   - **Confirm** by adding a diagnostic: the native backend calls
     `shader_translator_->TranslateAnalyzedShader` (~line 2112); log when a submitted draw's
     analyzed shader uses memexport and its export base address — expect a memexport to
     `0x1FC20000` during the loaded world.
   - **Fix** = execute memexport in the native backend: bind a UAV over the guest memory region
     (via `memory_writer_`/`NativeGraphicsSystem`), run the memexport draw, and write the
     exported stream to guest `0x1FC20000` (mirror the upstream d3d12 memexport path). This is
     a real feature (likely a workflow-sized task), but it is the correct fix and will also fix
     any OTHER game systems that rely on memexport. This is the single next task.
2. **If it is produced from scene luminance**, a NaN/overflow in the average-luminance
   reduction (HDR has values >1, alpha ~2.93) or an endian/format error in that pass would
   yield the NaN LUT. Fix the producer, don't clamp the consumer.
3. **Only as a shippable fallback** (if the producer can't be made correct), a *bounded*
   consumer-side clamp of the exposure LUT to a sane range is acceptable — but the
   `SANITIZE_EXPOSURE` flat-1.0 is a DIAGNOSTIC, not that fix (it ignores the adaptive curve;
   the scene stays dim). A correct fallback would reconstruct a plausible tonemap curve.
4. Cross-check the compositor constants: `c2 = +inf` (7F800000) is suspicious — confirm it's a
   legitimate clamp-max and not another NaN/inf source in the tonemap.
5. Only after the world is properly lit: the ~19 FPS retained-replay perf and Lionhead
   white-flash remain open.

## Build / tooling state

- Staged `rexgpu-native.dll` (this session): `Fable2Recomp/out/build/win-amd64-nightly/
  rexgpu-native.dll`, 1,695,744 bytes, SHA-256
  `58CE738B35715ACB01A6A65D9F4C9D3ED8E2EBB6090F87169781A40AA17CD8F8`. Adds two additive,
  default-OFF diagnostics only — normal runs (flags unset) are unchanged:
  - `gameplay resolve MAP` log (gated behind `gameplay_depth_fresh_`).
  - `REXGPU_NATIVE_SANITIZE_EXPOSURE=1` → overwrite non-finite compositor-exposure-LUT entries
    with 1.0 at upload (the end-to-end confirmation lever; a DIAGNOSTIC, not the fix).
- Build: `rexglue-src/build_native.cmd` (VS2022+Clang, BUILD_EXIT=0), then copy
  `rexglue-src/out/win-amd64/Release/rexgpu-native.dll` over the nightly one (not auto-staged).
- NEW harness `Fable2Recomp/tools/winshot_confirm.ps1 -Value 1 -Seconds 200 -Tag <id>` runs
  the Hero001 native loadsave arm AND captures the **Fable2 window** (foreground + client
  crop) to `ghidra_out/title_ui_re/depthfix/win_*.png`. Use this, not depthfix_confirm.ps1,
  for real gameplay captures. Extra env vars (e.g. `REXGPU_NATIVE_DEBUG_RESOLVE_BASE`,
  `REXGPU_NATIVE_DEBUG_GAMEPLAY_COLOR_DEPTH_ALWAYS`) set in the launching PowerShell process
  are inherited by the game.
- Decisive no-rebuild diagnostics available: `REXGPU_NATIVE_DEBUG_GAMEPLAY_COLOR_DEPTH_ALWAYS/
  _LEQUAL/_READ_ONLY/_BIAS` (depth-func override), `REXGPU_NATIVE_DEBUG_RESOLVE_BASE` (hex) +
  `_MIN_SEQUENCE` (decimal) (resolve BMP dump + half-float stats).

## What NOT to do (refuted this session — do not re-open without NEW evidence)

- ✗ Depth prepass / stale-seed / occlusion / GEQUAL as the cause of the black world
  (depth-always changes nothing).
- ✗ Tile composite / coverage loss / last-wins partial-tile replace (MAP log: coverage complete).
- ✗ VS-Z translation disagreement (single shared VS Z path; refuted by the workflow + C1).
- ✗ "Black material / black textures" as the cause (readback: HDR buffer has the real scene).
- The `gameplay_depth_fresh_` flag + tile-shared-depth scaffolding remain in-tree but are a
  DEAD END for the visual; leave default-OFF. Their only value now is that
  `gameplay_depth_fresh_=1` also gates the useful MAP diagnostic.
