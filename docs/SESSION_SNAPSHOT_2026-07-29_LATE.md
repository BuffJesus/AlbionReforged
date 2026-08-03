# Session snapshot - 2026-07-29 late night

## Resume here

The native D3D12 renderer is not currently visually correct in loaded
gameplay. The loading screen is correct, but once the loader hands off to the
world, the 3D scene is black. UI, the heart, interaction text, and a few
emissive/glow elements remain visible. Gameplay is also genuinely slow after
the movie/loading transition. Treat both as open regressions; do not rely on
the older top-of-handoff claims that the native world was fully fixed.

The Lionhead intro has also regressed. It now alternates valid decoded frames
with white/overexposed frames and skips visibly. The movie path is not the
source of the post-load gameplay slowdown, but it is a separate visual bug.

The current staged native DLL is:

```text
Fable2Recomp/out/build/win-amd64-nightly/rexgpu-native.dll
SHA-256 454DCAB80F5A8AF16D907E15C6E98674BD481030B0DBEF742A73CF04C13056FA
```

Focused native verification passes 30/30 tests (`pm4.*` plus
`native_plugin_smoke`). That only proves the isolated contracts; it does not
mean the live visual result is correct.

## Captured visual ground truth

Every meaningful A/B was recorded from the real desktop. The clean composed
run is:

```text
ghidra_out/title_ui_re/clean_native_20260729_222238_desktop.mp4
ghidra_out/title_ui_re/clean_native_45s.png
ghidra_out/title_ui_re/clean_native_55s.png
Fable2Recomp/out/build/win-amd64-nightly/logs/Fable2_1333.log
```

At about 45 seconds the 1280x720 loading screen is complete and correctly
colored at roughly 49 displayed FPS. At about 55 seconds the composed
gameplay frame is black except for UI/glows, at roughly 19 displayed FPS.
`clean_native_live.png` confirms this was not an extraction artifact.

The full-run contact sheet from the first RenderDoc launch is:

```text
ghidra_out/title_ui_re/renderdoc_run_contact.png
ghidra_out/title_ui_re/renderdoc_run_20260729_224430_desktop.mp4
```

It independently shows valid frontend/loading art, white flashes in the
Lionhead sequence, and the black loaded-world result.

The mature Xenos visual oracle for the same save remains
`ghidra_out/title_ui_re/xenos_same_save_reference_live.png`. Do not switch the
project back to Xenos as a fix; use it only to identify the expected scene.

## What is proved about the black world

The final compositor is consuming the native resolved HDR target. In the
loaded world it logs `resolved=true, cached=false` for the frontbuffer-matching
texture. Therefore this is not a stale guest-RAM fallback or merely a
compositor ownership miss. The black content already exists in the native HDR
color resource.

The first loaded-world pair is:

```text
depth:
  source EDRAM base 0x3F0
  1120x720, 2x MSAA, D24FS8 guest format
  destination 0x1A2B1000
  about 225-243 retained draws

color:
  source color base 0
  1120x720, 2x MSAA, RGBA16F host storage
  destination 0x19C67000
  about 232-262 retained draws
```

The directly resolved depth is coherent scene-shaped data, finite, and
roughly 0.002 through 0.254. Geometry and attachment creation therefore exist.

Normal reverse-depth `GEQUAL` leaves only a few fragments in the color target.
Forcing the narrowly selected gameplay HDR pass to `LEQUAL` fills almost the
entire upper HDR surface with pale yellow/orange overdraw:

```text
ghidra_out/title_ui_re/depth_lequal_narrow_20260729_223515_desktop.mp4
Fable2Recomp/out/build/win-amd64-nightly/rexgpu_native_resolve_19C67000_01.bmp
Fable2Recomp/out/build/win-amd64-nightly/logs/Fable2_1335.log
```

This proves color geometry is submitted and reaches the target. It also shows
that color-pass depth is generally behind the stored reverse-Z prepass depth.
`LEQUAL` is diagnostic only; it destroys occlusion and is not a fix.

A positive D3D12 integer depth bias of 8, applied only to the same gameplay
HDR color pass while retaining `GEQUAL`, made no visible change:

```text
ghidra_out/title_ui_re/depth_bias8_20260729_223953_desktop.mp4
ghidra_out/title_ui_re/depth_bias8_live.png
Fable2Recomp/out/build/win-amd64-nightly/logs/Fable2_1336.log
```

The log confirms the biased PSO was submitted. Do not repeat bias 8. A larger
bias may still be useful as a scale probe, but it must not become the product
fix.

Float24 truncation, sparse/full constant packing variations, no-pixel-shader
experiments, depth disabled/ALWAYS, read-only depth, and skipping early depth
draw prefixes were already tried or narrowed. None restored a correct world.
The constant upload layout was verified and restored.

## Prepass/color pairing result

`Fable2_1335.log` contains full fingerprints for one depth batch (243 draws)
and the following color batch (262 draws). Comparing draw index 0 through 242:

- every color draw is exactly 269 guest draw sequences after its prepass
  counterpart;
- the gap is constant for all entries, so this is a deterministic same-frame
  pair, not arbitrary historical work from another camera frame;
- the first five terrain draws use identical vertex/index payloads but
  different optimized depth and material vertex shaders/constants;
- later material draws generally use different shader/fetch layouts, as
  expected for depth-only versus fully shaded variants;
- 11 entries have identical geometry, vertex shader, vertex constants, and
  fetch constants.

This weakens the simple “wrong frame’s depth was copied” theory. The leading
hypothesis is now a disagreement in the native translation of the separate
depth-only and material vertex paths, guest depth precision, or the exact
persistent-depth load/store point. RenderDoc pixel history on a real black
world frame is the shortest route to distinguish them.

## Performance is a separate, confirmed problem

The black-world run is CPU-bound inside native retained-draw/resolve replay,
not GPU-bound and not an encoder illusion. A late checkpoint reported:

```text
draw events:                       about 2.33 million
resolve events:                    about 58 thousand
SubmitDrawProof calls:             about 1.41 million
retained draws:                    about 1.43 million
historical resolve draws skipped:  about 1.10 million
pending retained draws:            bounded near 872
draw slot waits:                   about 18,198 / 5.12 seconds total
```

Presentation pruning keeps the pending list bounded, but it does not prevent
the renderer from repeatedly walking and resubmitting large retained
populations. The old “64 slots fixed the cadence” conclusion is not valid for
this late Hero001 scene. Increasing rings or trusting the uncapped host FPS
counter will not solve it.

After color correctness:

1. Attribute CPU time by PM4 capture, resolve matching, retained-list scans,
   texture binding, PSO lookup, and command recording.
2. Stop replaying historical Xbox resolve populations as fresh native work.
3. Record each guest draw once into an explicit host pass/attachment
   generation, then lower resolves to graph edges/copies.
4. Measure fresh guest swaps and desktop frame-time percentiles, not the
   uncapped presenter loop.

## RenderDoc and unattended replay

RenderDoc is installed at `C:\Program Files\RenderDoc`. Injecting into an
already running `Fable2.exe` failed, but launching through
`renderdoccmd capture` works.

`Fable2Recomp/tools/artifact_repro.ps1` now has `-RenderDocCapture`. It launches
through RenderDoc, discovers the child Fable2 process, and requests one F12
capture. The first implementation captured too early because
`region_specific_*.bnk` only proves that loading started. It produced:

```text
ghidra_out/title_ui_re/renderdoc_black_world/fable2_frame1788.rdc
```

That file is replayable, but it contains a 1280x720 loading/compositor frame:
112 actions and no 1120x720 gameplay attachment. It is not useful for the
depth failure.

The harness is corrected to wait for
`captured first native depth resolve as R32`, then wait two seconds and request
F12. This targets the actual black world. A desktop MP4 is still recorded in
parallel.

`ghidra_out/title_ui_re/renderdoc_analyze.py` demonstrates unattended replay:

```powershell
& 'C:\Program Files\RenderDoc\qrenderdoc.exe' `
  --python 'D:\Documents\Fable2RE\ghidra_out\title_ui_re\renderdoc_analyze.py'
```

The script calls `pyrenderdoc.LoadCapture`, inspects resources/actions on the
replay thread, writes `analysis.txt`, saves selected textures, and exits before
opening the normal UI. It currently has the first capture path hardcoded and
must be pointed at the corrected gameplay `.rdc`.

## Source state and diagnostic switches

The native backend source is:

```text
rexglue-src/src/graphics/native/d3d12_pm4_backend.cpp
rexglue-src/src/graphics/native/d3d12_pm4_backend.h
```

The native directory is untracked in the surrounding rexglue Git checkout.
Do not use reset/checkout operations that would discard it. Both worktrees
contain unrelated user changes.

These diagnostics compile but are inactive unless explicitly set:

```text
REXGPU_NATIVE_DEBUG_GAMEPLAY_COLOR_DEPTH_ALWAYS
REXGPU_NATIVE_DEBUG_GAMEPLAY_COLOR_DEPTH_LEQUAL
REXGPU_NATIVE_DEBUG_GAMEPLAY_COLOR_DEPTH_READ_ONLY
REXGPU_NATIVE_DEBUG_GAMEPLAY_COLOR_DEPTH_BIAS
REXGPU_NATIVE_DEBUG_GAMEPLAY_DEPTH_SKIP_PREFIX
REXGPU_NATIVE_DEBUG_RESOLVE_PRESENT_BASE
REXGPU_NATIVE_DEBUG_RESOLVE_MIN_SEQUENCE
```

Before every clean validation, remove all of them from the environment. The
gameplay selector is intentionally narrow: known 1120-pitch surface, 2x MSAA,
color base 0 with writes enabled, and depth base `0x3F0`.

Local-history reconstructions are preserved at:

```text
ghidra_out/title_ui_re/localhistory_before_20260729_2028.patch
ghidra_out/title_ui_re/localhistory_before_20260729_2032.patch
ghidra_out/title_ui_re/history_2032/
```

The known-good Lionhead recording `strip_restart_full_desktop.mp4` starts
around 20:29, but gameplay was already bad in that revision.
`native_final_visual_validation.png` from 08:15 also shows bad gameplay. There
is no verified fully correct native gameplay revision from 2026-07-29 to
restore wholesale.

## Resume order

1. Finish the corrected RenderDoc gameplay capture and verify its thumbnail or
   resource inventory contains the 1120x720 2x-MSAA color/depth pair.
2. Use action/resource usage plus pixel history at a pixel that should contain
   terrain. Record stored depth, incoming depth, compare function, sample
   index, and which prepass/color draw wrote or failed it.
3. If incoming depth differs only by guest precision, implement the guest
   D24FS8 behavior at the attachment boundary for both paths. Do not ship
   `LEQUAL`, `ALWAYS`, or an arbitrary bias.
4. If stored depth comes from the wrong generation, fix the persistent depth
   graph edge and add a same-frame generation assertion.
5. Restore world visuals in a clean composed desktop capture.
6. Then profile and eliminate historical retained-draw replay.
7. Finally isolate Lionhead white flashes with a video-only RenderDoc capture
   and compare planar-frame ownership/clear behavior against
   `strip_restart_full_desktop.mp4`.
