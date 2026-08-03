# Session snapshot - 2026-07-28 late night

## Resume here

The current native-renderer blockers are no longer best described as a generic
"EDRAM problem." The guest EDRAM allocation and guest-memory render-target
round trips have already been removed from the host architecture. Tonight's
60 fps captures and focused logging isolated two missing native render-graph
semantics in the loaded-world compositor, plus a separate throughput problem.

### Do not confuse the visual oracle with a native regression

- `captures/run1159_xenos_postskip_reference_60fps/transition_contact_sheet.png`
  is the run with visible, correctly colored outdoor gameplay. It loaded
  `rexgpu-xenos.dll`, as proven by `Fable2_1159.log`. It is the mature-renderer
  visual oracle, not a successful native-renderer frame.
- Runs 1156 and 1157 used `rexgpu-native.dll` and already produced the
  peach/orange loaded-world frame. Therefore the diagnostics added after those
  runs did not reintroduce the peach output.
- Runs 1160 and 1161 also used `rexgpu-native.dll`. The harness was interrupted,
  leaving both processes alive; PIDs 12064 and 19064 were terminated during
  cleanup. `mystartup.lua` is restored to the normal 26-byte empty script.

User-visible ground truth from the xenos oracle run:

- gameplay was visible but extremely laggy while audio continued normally;
- hero skin was black;
- dog textures still contained artifacts.

User-visible ground truth from the latest native run:

- peach/orange gameplay output remained.

### Decisive compositor finding: the exposure input is 512 NaNs

`Fable2_1161.log` captured the loaded-world final compositor draw:

- draw sequence `352292`;
- pixel shader `F1ABBFF68DE8DBFD`;
- triangle-list fullscreen draw, 3 vertices, not indexed;
- destination target and guest viewport are genuinely 1280x720:
  `surface_pitch=1280`, viewport scale/offset `640/640/-360/360`;
- scissor covers the full 1280x720 target;
- all three referenced textures are captured, with no missing descriptor.

The compositor inputs are:

1. slot 0, `0x19C67000`, 1120x720, pitch 1120,
   `k_16_16_16_16_EXPAND`, resolved host target;
2. slot 1, `0x1FC20000`, 512x1, pitch 512, `k_32_FLOAT`, linear,
   endian `k8in32`, not matched to a resolved host target;
3. slot 2, `0x0C786000`, 280x180, pitch 288,
   `k_2_10_10_10_AS_16_16_16_16`, resolved host target.

The new CPU-side diagnostic inspected slot 1 after guest endian conversion:

```text
count=512, finite=0, nonfinite=512,
first=-nan/-nan/-nan/-nan/-nan/-nan/-nan/-nan
```

This is a direct explanation for the peach/overexposed tonemap. The most likely
architectural cause is that `0x1FC20000` is a GPU-produced luminance/exposure
resource whose host resolve edge is not being matched, so the compositor falls
back to stale/sentinel guest RAM. Do not "fix" this by clamping NaNs in the
pixel shader. Find its producer and keep that resource host-resident through an
explicit render-graph dependency.

Next checks:

1. Instrument every resolve whose destination range overlaps `0x1FC20000`, not
   only exact logged destinations.
2. Log the raw four bytes and converted four bytes for the first few entries to
   distinguish an all-`FF` guest snapshot from a conversion bug.
3. Compare resolved-target matching rules for 1D fetches and
   `k_32_FLOAT`/destination-format compatibility against the mature D3D12
   backend.
4. Bind the producer's host resource directly once identity and format are
   proven. Guest RAM remains metadata/ABI state, not the normal transport.

### Separate width defect: native scaling is missing

The orange frame is recognizable scene geometry, not a flat clear. Its black
right strip is about 160 pixels wide: exactly the difference between the
1120-wide HDR input and the 1280-wide frontbuffer. The final viewport and
scissor are full width, so the problem is not a clipped destination draw.

The native graph currently passes a 1120x720 dynamic-resolution HDR resource
to the 1280x720 final compositor without fully reproducing the guest sampling
or resolve-scale semantics. Implement this as a PC-native upscale/compositor
edge. Do not recreate EDRAM tile limits. Validate full-width output at 1280 and
then make the edge resolution-independent for PC output sizes.

### Performance handoff

The user's timing was accurate. The large slowdown begins after skipping the
save-load intro movie, at the world-render handoff:

```text
first native depth resolve:
sequence=344660, source=1008, destination=1A2B1000,
1120x720, draws=1536
```

The video path is smooth enough and audio does not slow, which points at the
native graphics worker rather than global emulation timing. At checkpoint
1024, the native backend had already made 7,599 target switches, while the
texture cache showed 115,819 hits, 1,424 misses, no eviction, and zero
draw-slot waits. This exonerates cache capacity and the 32-slot submission
ring as the primary hitch in this run. The expensive behavior is repeated
per-resolve replay of large retained draw populations and excessive attachment
switching.

After color correctness:

1. Record CPU time separately for PM4 capture, shader/PSO lookup, texture
   snapshot/hash, draw submission, and resolve replay.
2. Replace repeated retained-draw replay with persistent pass ownership in the
   explicit host render graph.
3. Batch consecutive work per attachment and submit graph transitions once,
   rather than rebuilding Xbox resolve history every presentation.
4. Re-capture frame-time percentiles; do not judge performance from the
   encoder video alone.

### Hero and dog appearance

The user still saw black hero skin and dog texture artifacts in the visible
xenos-oracle run. Neither native run 1160 nor 1161 logged a
`0x12704000` player/dog resolve candidate or successful selective
materialization. Hero000 loads a valid 31,968-byte `texturemorphs.bin`, but this
automation path does not trigger the currently recognized atlas-regeneration
event.

This means the selective materialization implementation is still only a
proven path for the femtofork's exact regeneration event, not a complete fix
for this save/load case. Tomorrow, capture the appearance textures actually
sampled by the hero and dog in the visible frame, trace their producer
generations, and determine whether Hero000 is consuming a stale saved atlas,
using a second resolve address, or missing a CPU composite upload.

### Current source/build state

- The combined depth/stencil experiments were fully reverted. Native execution
  is back to the D32 depth path; no stencil capture or pipeline-key changes
  remain.
- Current source additions are diagnostic only:
  detailed final-resolve dependency state and one post-endian exposure scan.
- Focused verification passes: 530 assertions in 29 test cases.
- Staged `rexgpu-native.dll`:
  SHA-256
  `587AC506606A740C19C73B048BC01D9B3B9E2AD5817B0C4A4FDFA1198B52FAD2`,
  1,619,968 bytes.
- Primary native evidence:
  `Fable2Recomp/out/build/win-amd64-nightly/logs/Fable2_1161.log`.
- Primary visual oracle:
  `captures/run1159_xenos_postskip_reference_60fps/transition_contact_sheet.png`.
