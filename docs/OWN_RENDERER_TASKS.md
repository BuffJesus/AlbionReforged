# Own Renderer — active task list

Updated 2026-07-29 late night. The existing xenos backend remains the visual oracle until the native path
reaches parity; Ghidra is used for targeted emitter semantics where LTCG has erased direct call
boundaries.

Latest evidence and resume order:
[SESSION_SNAPSHOT_2026-08-02_NATIVE_RENDERER.md](SESSION_SNAPSHOT_2026-08-02_NATIVE_RENDERER.md).

Ghidra is not reserved for the current host-backend/shader-cache work and may be used by the other
agent. Reclaim it only when a named Fable-specific semantic question survives capture/replay.

## Native PC direction

- [x] Treat PM4, Xenos, and EDRAM behavior as the guest frontend ABI, not the host architecture.
- [x] Establish persistent host render-target rebinding without requiring a guest-memory round trip.
- [ ] Build an explicit host render graph with resource identities/generations, pass dependencies,
  load/store behavior, synchronization, and lifetime tracking.
- [x] Use unrestricted PC heaps and native formats without inheriting EDRAM or guest-RAM ceilings.
- [x] Lower resolves to native graph edges, copies, or blits and present directly from host resources.
- [x] Split persistent draw ownership into independently keyed color and depth
  resources so color-only/depth-only passes do not churn unrelated targets.
- [x] Make persistent resolve registry entries reference the actual draw
  attachments, eliminating duplicate seed/store snapshot textures and copies.
- [x] Remove the `REXGPU_NATIVE_RESOLVE_HEAP` compatibility gate; the host
  graph is unconditional and guest EDRAM bases are identity/alias labels only.

## Current visual-state parity focus (2026-08-02)

- [ ] Restore loaded-world material brightness under the native path. Frontend/loading/UI and the
      final compositor present, but the Hero001 1120x720 world is flat blue-gray.
- [x] Prove the final compositor consumes live native HDR from `0x19C67000` rather than stale guest RAM.
- [x] Prove native output is non-empty and under-scaled: normal HDR peak/mean-max is approximately
      `1.0605/0.0811`; a diagnostic ×16 output reaches `16.97/1.297`.
- [x] Correlate late-world draw cache keys with the guest shader hashes used by the translator probe.
      The exact mapping is recorded in `SESSION_SNAPSHOT_2026-08-02_NATIVE_RENDERER.md` and
      `Fable2_1459.log` (`ps_guest=`).
- [ ] Run the first valid per-material solid-color A/B against guest hash
      `70E7786A87CAEFF5` (cache key `18F21758F4DC7D83`), then the other four only as needed.
- [ ] If valid material probes reach the target but the world remains unchanged, instrument native
      vertex/raster/depth/resolve acceptance before changing exposure/compositor code.

Screenshot capture is an active intervention by default: `winshot_confirm.ps1` foregrounds the game.
Use `-NoActivate` for passive capture only when the game is already foreground.

## Historical visual-state parity focus (2026-07-29 late)

- [ ] Restore loaded-world color under the real reverse-depth `GEQUAL` path.
  Clean capture shows a correct loader followed by a black 3D world with
  visible UI/glows.
- [x] Prove the final compositor samples the native resolved `0x19C67000`
  target rather than stale guest RAM.
- [x] Prove gameplay color geometry is present: narrowly scoped `LEQUAL`
  fills most of the HDR surface, while normal `GEQUAL` rejects it.
- [x] Rule out an arbitrary cross-frame prepass/color pairing: all 243 paired
  fingerprint entries have a constant 269-sequence gap.
- [x] Reject a positive D3D12 integer color-pass depth bias of 8 as
  insufficient; the clean composed world remains black.
- [ ] Capture the actual black gameplay frame in RenderDoc after the first
  1120x720 depth resolve and inspect pixel history for stored versus incoming
  depth on one terrain pixel.
- [ ] Determine whether the root is D24FS8 precision across separate
  depth/material vertex programs or a wrong persistent-depth generation.
- [ ] Replace repeated retained resolve history with explicit host-pass
  ownership. The late Hero001 run reaches roughly 1.41 million
  `SubmitDrawProof` calls and 18,198 draw-slot waits.
- [ ] Restore stable Lionhead planar-video presentation; current desktop
  capture flashes white between valid decoded frames.

## Historical visual-state parity focus (2026-07-28)

- [ ] Preserve the GPU-produced `0x1FC20000` 512x1 R32_FLOAT exposure resource
  as a native render-graph edge. Current guest-snapshot fallback supplies 512
  NaNs to compositor PS `F1ABBFF68DE8DBFD` and produces peach/orange gameplay.
- [ ] Add a PC-native dynamic-resolution upscale/compositor edge from the
  1120x720 HDR scene to the 1280x720 frontbuffer. The final viewport/scissor
  are already full-width; the remaining 160-pixel black strip is an input
  scaling/resolve semantic.
- [ ] Eliminate repeated per-resolve replay of the 1536-draw world population
  and batch persistent passes by host attachment. Run1161 reached 7,599 target
  switches by presentation checkpoint 1024 with no texture eviction and no
  draw-slot wait, matching the user's post-skip graphics-only slowdown.

- [x] Reproduce the late loaded-world retention overflow: 768 total draws
  truncates the frame; 1024 still overflows with 691 draws on one target; a
  1536/1024 probe retains 1357 before the per-target bound is reached.
- [x] Retain the measured complete late-world population with bounded
  2048-per-frame / 1536-per-target limits and verify no overflow through the
  same transition under D3D12 debug+DRED (`Fable2_1072.log`).
- [x] Clear shared cached targets when their guest EDRAM color/depth identity
  changes without a format/MSAA change, preventing stale tiles from bleeding
  between same-configuration passes.
- [x] Raise the texture-cache budget to one quarter of dedicated VRAM, clamped
  to 256 MiB–4 GiB, and replace repeated full-map LRU scans with one sorted
  fence-safe candidate pass.
- [x] Logarithmically sample expected fixed-placement conflicts, reducing the
  observed 3,856-line startup probe storm to 12 lines.
- [x] Instrument presentation checkpoints with host texture-cache occupancy,
  hit/miss/eviction, draw-slot wait, submission-batch, and target-switch
  counters; use `Fable2_1057.log` to isolate the three-slot submission ring as
  the steady-state menu/gameplay limiter.
- [x] Expand the independently fenced draw ring from 3 to 32 slots and reuse
  power-of-two vertex/index upload allocations.
- [x] Replace per-texture committed staging resources with one grow-only,
  fence-protected texture upload arena per draw slot.
- [x] Size the host texture cache from dedicated VRAM (one eighth, clamped to
  256 MiB–2 GiB) and retain the observed 1,526 MiB / 3,192-entry streamed set
  without eviction through checkpoint 2048 (`Fable2_1064.log`).
- [x] Fence heterogeneous frame submissions in chunks of at most 32 command
  lists, reducing a 768-draw frame from 768 queue signals to 24 while
  preserving per-slot resource lifetime.
- [x] Revalidate the optimized upload arenas and chunked submission path
  through Hero000 load -> `NewBeginnings.bik` -> skip with the D3D12 debug
  layer and DRED, with no validation warning/error or device loss
  (`Fable2_1067.log`).
- [x] Cache independently owned color/depth/resolve resources and RTV/DSV
  descriptor heaps per width/height/format/MSAA configuration, eliminating
  target-switch allocation and whole-GPU waits.
- [x] Restore heterogeneous one-fence frame batching once queued command lists
  have stable per-configuration resource and descriptor ownership.
- [x] Make EDRAM draw/resolve/copy/presentation an ordered direct-queue chain;
  retain a CPU wait only for explicit diagnostic readback.
- [x] Give multisample depth resolves per-command-slot shader-visible
  descriptors so 32 resolve slots may safely remain in flight.
- [x] Benchmark the optimized load/menu path: checkpoint time drops 24% for
  512->1024 and 16% for 1024->2048 versus the pre-optimization run, with the
  latter sustaining 28.3 presentations/second (`Fable2_1055.log`).
- [x] Revalidate load -> cinematic -> skip under the D3D12 debug layer with no
  validation error or device loss (`Fable2_1054.log`).
- [x] Capture the native save-load exit as a live D3D12 debug-layer/DRED
  failure: an unsignaled heterogeneous batch final-released shared targets
  before command list sequence 319660 executed.
- [x] Temporarily restrict one-fence draw grouping to a single
  MSAA/color-target configuration; superseded by stable cached ownership above.
- [x] Route presenter GPU loss through the native backend first and name native
  draw/resolve command lists with guest sequence/submission IDs for actionable
  DRED output.
- [x] Re-run Hero000 load -> `NewBeginnings.bik` -> skip in normal mode for 45
  seconds and under the D3D12 debug layer for 15 seconds with no validation
  error or device loss (`Fable2_1050.log`, `Fable2_1051.log`).
- [x] Capture the actual post-skip native window and verify complete storybook
  art, particles, and subtitle rather than a dark/cinematic-stale frame
  (`native_post_skip_fixed.png`).
- [x] Restore genuine planar video draws across transition mask `0x20`; intro and post-load
  cinematics are visually active again.
- [x] Replay persistent host EDRAM for drawless resolves and preserve compatible prior content
  across partial resolved-target updates.
- [x] Retain ready resolved-target postprocess draws across proven masks
  `0x40/0x41/0x60/0x61`.
- [x] Retain concrete, nontrivial texture-backed transition draws after boot while continuing to
  reject textureless packets and all-1x1 synthetic markers.
- [x] Restore the expected loading-screen presentation after save selection.
- [ ] Complete the full-size depth/HDR/color dependency chain. Current 1120x720 depth and
  560x360 color resolves have no captured producer; the 320x180 chain works.
- [x] Transfer presenter ownership from the last cinematic frame to the first
  ready storybook-loading frame immediately after a cinematic skip.
- [ ] Transfer presentation from the storybook loader to the first complete
  gameplay/world frame.
- [ ] Use run 1008's bounded all-resolve and texture-backed-transition probes to find the exact
  ownership/identity edge; remove both probe blocks after the fix.
- [ ] Eliminate the remaining intermittent black-square artifacts without admitting the
  `0x10000000` 1x1 marker packets.
- [x] Use guest 1x/2x/4x sample counts in native targets and PSOs, perform exact color resolves,
  and resolve the guest-selected depth sample through an exact compute path.
- [x] Key native color PSOs/targets by live Xenos formats, including RGBA8, 10:10:10:2, and
  RGBA16F storage used by the loaded world.
- [x] Match resolve ownership by EDRAM base, sample count, and compatible storage format while
  preserving the valid 2/10 and 3/12 aliases.
- [x] Initialize new draw color/depth allocations deterministically and validate the format
  transition path with the D3D12 debug layer.
- [x] Retire superseded pending draws at incompatible ownership transitions and retain the
  complete observed 599-draw HDR gameplay pass within a bounded 768-draw compatibility batch.
- [x] Require stable lower-complexity frontbuffer resolves before replacing an established
  complete frame.
- [x] Capture and submit guest alpha-test enable/function/reference state.
- [x] Capture screen/window scissor rectangles and apply their intersection to D3D12 draws.
- [x] Convert front/back Xenos polygon offset to native integer/slope depth bias and prove the
  real textureless biased pass (`Fable2_970.log`).
- [x] Capture guest blend constants and submit them with `OMSetBlendFactor`.
- [x] Snapshot, cache, untile, upload, view, and sample packed/tiled guest mip chains.
- [x] Keep base and mip guest-memory generations coherent and include both byte ranges in host
  texture cache identity.
- [x] Honor guest base-map-only mip filtering and live anisotropy (`Fable2_975.log` reaches 2:1).
- [x] Capture `PA_CL_CLIP_CNTL`, key depth-clip enable into native PSOs, and apply it.
- [x] Prove the first stencil-enabled packet is rejected before native draw retention.
- [ ] Inventory a stencil-enabled draw that survives the actual native submission guards before
  changing D32 resources to a depth/stencil format.
- [ ] Continuously capture a user-visible glitch scene with the game foreground and compare every
  frame; window-target capture returns a hardware-overlay black surface when the game is occluded.

## Current title/UI focus (2026-07-27)

- [x] Render the title-screen `QuadList` sparkle layer.
- [x] Compose Xenos texture component swizzles into native SRVs.
- [x] Create native samplers from guest filter, address, and border state.
- [x] Isolate the missing `A` image draw between the `Press` and `to start` text runs.
- [x] Verify the `A` glyph's tiled RGBA8 decode, alpha, UVs, and screen-space geometry.
- [x] Trace the `A` draw downstream through descriptor binding, PSO execution, composition,
  resolve, and presentation; make the glyph visible.
- [x] Restore the opaque `FABLE II` logo layer.
- [x] Restore the scenic title background using persistent native depth/HDR render targets.
- [x] Eliminate short black frontbuffer resolves during the white transition and settled title
  with a bounded absolute/relative underfill hold.
- [x] Restore all save-selection and main-menu textures/UI layers.
- [x] Inventory the settled save-selector pass (405 draw events / 168 eligible textured draws)
  and retain the complete observed pass with a bounded 256-draw compatibility list.
- [x] Exclude resolve/ownership-transition packets that also decode as draw events from frontend
  geometry submission.
- [x] Capture `PA_SU_SC_MODE_CNTL` in normalized draws and apply guest culling/front-face state in
  native D3D12 PSO keys, removing the hidden back-facing black atlas quad.
- [x] Continuously capture the settled save fan for 30 seconds and scan every decoded frame for
  black output and abrupt luminance changes.

## Current boot-video focus

- [x] Recognize the genuine three-plane linear R8 4:2:0 draw and present its decoded frame through
      the owned D3D12 pipeline.
- [x] Add vblank-driven coherent Y/U/V polling, direct content fingerprints, duplicate suppression,
      and bounded three-slot submissions behind `REXGPU_NATIVE_STREAM_TEXTURE_PROOF=1`.
- [x] Prove with 600 direct polls that the frozen first frame is guest-producer state, not missed
      dirty tracking or presenter refresh.
- [x] A/B the same runtime: Xenos reaches the Fable II title in about 12 seconds; native stops
      producing commands after sequence 106.
- [x] Mirror event initiator/extent and `REG_RMW` mutations into the guest-visible MMIO bank.
- [x] Instrument the terminal `WAIT_REG_MEM` value and the packet-interrupt queue, dispatch time,
      pending mask, and GPU counter.
- [x] Dispatch PM4 interrupts immediately on the guest-aware host thread independently of vblank;
      prove all 9/9 delivered with 7 µs final latency (`Fable2_716.log`).
- [x] Identify the missing write to aligned semaphore word `0x1FC83004`: PM4 register indices
      above `0x3FFF` were incorrectly truncated through the MMIO aperture, so Fable's
      `0x4000..0x43FF` constant upload zeroed `SCRATCH_UMSK/SCRATCH_ADDR`.
- [x] Add a disabled diagnostic wait-release mode and prove four distinct native YUV frames,
      including new video draws at sequences 133 and 187 (`Fable2_717.log`).
- [x] Trace the wait chain to the `0x1FC83000` D3D swap-ring callback mailbox and known frame-end
      functions/device state (`Fable2_718.log`).
- [x] Run native GPU commands on an `XHostThread` and dispatch packet interrupts synchronously,
      matching Xenos execution context (`Fable2_719.log`; stall remains).
- [x] Recover the exact `Function_821F6050` callback packet and
      `D3D_GraphicsInterruptCallback` (`0x82BA26B0`) mailbox protocol.
- [x] Rule out primary-ring writeback cadence and fix the actual PM4/MMIO register-domain alias.
- [x] Capture a diagnostic run with at least two distinct native YUV content hashes.
- [x] Remove the diagnostic wait releases and record the unforced real-time boot-video proof
      (`Fable2_733.log`: 291 submitted frames in 600 polls; frame 256 at sequence 7017).
- [x] Capture the live D3D12 window and disprove the stale first-frame BMP's apparent washout
      (`Fable2_738.log`; `native_738_boot_sequence/frame_14.png`).
- [x] Correct planar-video row orientation while preserving the translated D3D NDC transform;
      timed captures now show upright Microsoft and Lionhead frames.
- [x] Promote planar streaming into the normal presenting D3D12 path; no proof variable is
      required (`Fable2_740.log`: automatic activation and frame 128 within eight seconds).
- [x] Keep headless initialization unchanged, decouple normal streaming from diagnostic artifact
      dumps, and cover planar classification/bottom-up rows with a focused regression.
- [x] Give planar video exclusive presentation ownership and suppress incomplete companion proof
      batches (`Fable2_741.log` isolated the black-square overwrite).
- [x] Pace planar updates at completed guest draw boundaries instead of polling decoder memory on
      host vblank (`Fable2_743.log`; clean/stable captured frames 37-49).
- [x] Replace eager proof presentation with guest-swap pacing and filter incomplete planar
      companions (`Fable2_746.log`).
- [x] Inventory the first frontend layouts and add tiled RGBA8/10:10:10:2/RGBA16F plus
      BC1/BC2/BC3/BC4 snapshot, untile, upload, and sampling.
- [x] Capture Xenos color blend/write state in normalized draws and native PSO keys; translate it
      to D3D12 so frontend alpha composition no longer emits black quads.
- [x] Correct frontend clip-space orientation independently of bottom-up planar video and capture
      the first upright readable non-video frame (`native_753_frontend_oriented/frame_00.png`).
- [x] Add native 1D R32F/RGBA/RGBA16F lookup textures (`Fable2_755.log`).
- [x] Trace the missing menu/title UI to the all-zero resolved surface at guest
      `0x0C6E4000`, not a missing glyph or BC texture format (`Fable2_759.log`).
- [x] Prototype an opt-in host-only D3D12 resolve heap keyed by guest destination address,
      including per-draw color/depth target identity and direct host-texture rebinding.
- [x] Preserve compatible EDRAM/resolved-target contents across drawless and partial resolves.
- [ ] Make the full-size `0x0C6E4000` source non-black before enabling the resolve heap by default;
      its 320x180 chain already captures successfully.
- [ ] Add the remaining depth/HDR postprocess formats used by the title frontend.
- [ ] Replace the guarded 128-draw prefix with complete render-target/resolve-aware frontend
      composition.

## Completed foundation

- [x] Parse packets at actual execution time, including dynamically populated nested IBs.
- [x] Prove live framing completeness: `UNKNOWN=0`, `malformed=0`, zero real-CP packet errors.
- [x] Add a small dedicated test target (`pm4_parser_tests`).
- [x] Shadow inline and memory-backed writes across the full 15-bit PM4 register-index space
      without aliasing them through the 16-bit byte-addressed MMIO aperture.
- [x] Mirror bin predication, REG_RMW, extended registers, and gamma-ramp side effects.
- [x] Emit normalized indexed/auto-index draw records.
- [x] Compare known shadow state with xenos: 2,473 standard registers, zero live mismatches.
- [x] Prove API-neutral device/queue/command recording and live 1280x720 diagnostic presentation
      on D3D12 (`Fable2_656.log`) and Vulkan (`Fable2_659.log`).

## P1 — command processor parity

### P1.1 Deterministic executed-stream capture/replay

- [x] Define a versioned, endian-explicit capture format containing execution-ordered packets and
      memory dependencies used by `LOAD_ALU_CONSTANT`, pointer-based shader loads, and
      memory-polled `COND_WRITE`.
- [x] Add a bounded `FABLE2_PM4_CAPTURE=<path>` writer, off by default
      (`FABLE2_PM4_CAPTURE_MAX_MB`, default 256 MiB).
- [x] Add a standalone `pm4_replay` executable that feeds `Pm4Parser` + `Pm4StateShadow`.
- [x] Generate compact synthetic fixtures in tests and retain the representative live capture
      locally under the ignored build output rather than checking game-derived data into source.

Acceptance: the same capture produces byte-identical stats, register state, and draw-event hashes
across repeated runs without launching Fable II. **Met:** the bounded 8 MiB boot capture contains
175,764 packet records + 1,487 dependency records with zero missing dependencies. Two offline
replays produced identical packet (`03F8E1006C0E7534`), draw (`CB0E7E75AEC07B26`), and register
(`A5C658D6DCD87A30`) hashes with the P1.2 event semantics enabled, with `unknown=0`,
`malformed=0`, `unclassified=0`, and `invalid=0`.

### P1.2 Finish state/control semantics

- [x] Model `EVENT_WRITE*` and `VIZ_QUERY` event-register effects.
- [x] Model or explicitly classify `COND_WRITE`, waits, coherency, interrupts, and memory writes.
- [x] Add semantic/control/unclassified coverage counters distinct from packet-framing coverage.
- [x] Extend parity checks to selected extended registers and to state snapshots at every swap.

Acceptance: no unclassified executed opcode semantics; zero known-state mismatches through a
boot → loaded world → combat/effects trace.

Current evidence: the 8 MiB deterministic boot corpus replays 481 swap snapshots with deterministic
snapshot-stream hash `2559219BD700E6D7`, alongside 19,746 state-semantic packets, 15,698 control
packets, and zero unclassified/invalid/unknown/malformed packets. Controlled live log 630 reached
958,950 executed packets, 68,049 draws, and 656 swaps with `unclassified=0`, `invalid=0`, `oor=0`,
and zero cumulative swap mismatches. Every sampled swap matched 2,473 known standard registers plus
the exercised sparse extended register. A longer loaded-world/combat/effects trace remains for the
full scenario acceptance criterion.

### P1.3 Draw assembly

- [ ] Complete `DRAW_INDX_BIN` / `DRAW_INDX_2_BIN` normalization if live captures exercise them.
- [x] Resolve active vertex-fetch descriptors from shadow registers and shader microcode.
- [x] Resolve index-buffer address, format, length, and endian mode.
- [x] Attach active vertex/pixel shader identities and render-target state to each draw.
- [ ] Hash normalized draw records and compare frame-by-frame with xenos trace metadata.

Acceptance: every live draw becomes a complete backend-neutral draw record with no pointer into
xenos command-processor classes.

Current evidence: DMA draws carry an aligned guest index-buffer address, 16/32-bit format and
element size, endian mode, available element count, and byte length. A dependency-free microcode
scanner tracks active vertex/pixel shader hashes and extracts the active vertex-fetch mask, so each
referenced fetch resolves to slot, validity type, endian, guest address, dword count, and byte
length. Draws now also carry knownness, pitch, MSAA, EDRAM mode, four color
base/format/bias/write-mask descriptors, depth base/format/control, and a seven-bit attachment
transition mask. The deterministic corpus draw hash is `CB0E7E75AEC07B26`; it analyzed 3,441
shader loads with zero failures and found 1,962 render-target transition draws. Live log 636
reached 219,370 draws and 29,836 transitions with zero fetch-mask, semantic, or register-parity
errors.

### P1.4 Shader inventory

- [x] Capture `IM_LOAD` and `IM_LOAD_IMMEDIATE` microcode payloads at execution time.
- [x] Deduplicate shaders by stage + bytecode hash.
- [x] Record constant/fetch usage and translator output cache keys.
- [x] Build an offline `pm4_replay --inventory` report for the captured shader population.
- [x] Retain exact host-endian microcode payloads and reject hash/key collisions by full-byte
      comparison.
- [x] Resolve and prepare the active VS/PS payloads through the backend contract before each draw.

Acceptance: repeated runs discover no new shader hashes after the representative scene suite.

Current evidence: the 8 MiB boot corpus contains 3,441 shader loads deduplicated to 7 vertex and
12 pixel shaders. Each inventory row reports stage, stable bytecode hash, dword count, load count,
vertex/texture-fetch masks, float/bool/loop usage, dynamic float addressing, and a versioned
translator cache key. Repeated replay produces identical keys and draw hash `8EECEFDE3D074DC3`.
Live log 638 compared the independent constant masks with `Shader::ConstantRegisterMap` 250,910
times with zero mismatches; final key-bearing staged build log 641 reached 135,816 zero-mismatch
constant checks, alongside zero fetch-mask, shader-analysis, semantic, or parity errors.
Representative loaded-world/combat/effects captures are still needed to establish population
saturation.

The saved boot capture now proves translation-cache ownership: 26,526 draw-stage prepare requests
collapse to 19 unique owned payloads, 26,507 hits, and zero rejected/colliding records. The focused
suite passes 361 assertions in 21 cases.

### P1.5 Resolve, texture, and frame events

- [x] Normalize resolve/copy events and their EDRAM/source/destination descriptors.
- [x] Normalize texture fetch descriptors referenced by draws.
- [x] Emit explicit frame boundaries from `XE_SWAP`.
- [x] Record render-target and depth-target transitions needed by a host backend.

Acceptance: replay yields an ordered frame IR containing draws, resolves, resource dependencies,
and present events.

Current evidence: copy-mode draws emit `Pm4ResolveEvent` records containing source color/depth
EDRAM base and format, surface pitch/MSAA, copy/sample/clear controls, and destination base,
pitch/height/array/slice/format/endian/bias/swap state with explicit knownness. All 497 resolves in
the saved corpus are complete (496 color, 1 depth), with deterministic resolve hash
`FF3915A416DED5CC`. Live log 636 reached 9,997 complete resolves (9,398 color, 599 depth), 1,079
swap boundaries, and zero unknown/malformed/unclassified/invalid packets or state mismatches.
Targeted Ghidra scans confirmed the cold `D3D_Resolve_EmitCopyDraw` has no direct references after
LTCG and that its strongest packet constants overlap export/frontbuffer emitters; executed
`RB_MODECONTROL=kCopy` plus `RB_COPY_*` state is therefore the authoritative classifier.

Texture fetches are now scanned independently from both active shader stages and joined to draws
as backend-neutral six-dword descriptors with explicit per-dword knownness, stage ownership,
addresses, dimensions, format/endian/sign/swizzle, filtering, LOD, and mip state. The saved corpus
contains 2,857/2,857 complete descriptors across 967 draws and produces deterministic draw hash
`0EE9EC8C218CA3EA`. Live log 637 reached 181,136/181,136 complete descriptors and 516,243
per-stage comparisons with the runtime analyzer, with zero mask mismatches.

### Native validation seam complete

`rexgpu-native` now builds and loads independently as a headless validation plugin. Its owned ring
worker and offline replay both feed `Pm4Stream`, which assembles normalized state and dispatches
draw/resolve/present IR through `Pm4Backend`. `Pm4ValidationBackend` retains the established
aggregate hashes and adds 481 deterministic frame-indexed hashes for the saved boot capture. The
DLL smoke covers ABI/factory/headless setup/shutdown/unload, and the focused suite passes 342
assertions in 20 cases.

Acceptance for the next slice: `--gpu_plugin=native` loads independently and validates the saved
frame stream through the backend interface with deterministic hashes, without calling xenos draw
assembly. **Met.** The plugin now also owns presentation, guest vblank/interrupt dispatch,
scratch-register memory writeback, endian-aware PM4 memory writes, and blocking waits. Live log
`Fable2_656.log` reaches `XE_SWAP` and continues consuming clean ring spans.

### Backend foundation and Vulkan viability

- [x] Make backend construction injectable without putting D3D12/Vulkan types in `Pm4Stream` or
      normalized PM4 event headers.
- [x] Add API-neutral backend identity, capability, and diagnostics reporting.
- [x] Add `--gpu_backend` host selection and forward it through the existing plugin ABI.
- [x] Create and own a D3D12 device plus direct graphics queue behind `D3D12Pm4Backend`.
- [x] Add D3D12 allocator/list/descriptor/fence recording primitives and presenter ownership.
- [x] Submit and present a diagnostic clear from a live Fable `XE_SWAP`.
- [x] Prove a Windows `D3D12=OFF`, `Vulkan=ON` configuration builds `rexruntime` and
      `rexgpu-native`; fix the common presenter's hidden DXGI link dependency found by that build.
- [x] Add a selectable `VulkanPm4Backend` that owns the provider, device, and graphics queue.
- [x] Add Vulkan command-pool/buffer/fence recording and the equivalent diagnostic clear.

Vulkan is viable, not speculative: the SDK already has a Vulkan instance/device/provider,
presentation layer, SPIR-V translator path, volk, and VMA. The native PM4 stream compiled and
linked with D3D12 completely absent. The Vulkan backend now implements the proven ownership
contract and shares every PM4/control semantic with D3D12. D3D12 remains the first draw
implementation because it is the current Windows oracle; backend command submission may now
advance in Vulkan without duplicating the command processor.

Live D3D12 proof: nightly log `Fable2_656.log` selected `backend='d3d12-foundation'`, reported
device/queue/recording/clear/presentation capabilities, completed the guest scratch/interrupt
handshake, and presented the first 1280x720 diagnostic clear. D3D12 has since advanced to guarded
captured-draw submission; see P2 below.

Live Vulkan proof: nightly log `Fable2_659.log` selected `backend='vulkan-foundation'`, reported
device/queue/recording/clear/presentation capabilities, completed the same handshake, and presented
the first 1280x720 Vulkan diagnostic clear without Vulkan/GPU errors.

## P2 — native backend vertical slice

- [x] Scaffold `rexgpu-native` as a separately selectable validation plugin.
- [x] Define the initial backend-neutral draw/resolve/present interface and shared stream consumer.
- [x] Implement D3D12 device and direct command-queue foundation.
- [x] Implement D3D12 presenter ownership, descriptor allocation, and command recording.
- [x] Reproduce live guest control/memory side effects needed to reach `XE_SWAP`.
- [x] Feed normalized IR into the same validation backend from live ring input and replay.
- [x] Render and present a diagnostic clear; retain per-run A/B selection with xenos.
- [x] Feed one live captured Fable shader through the existing raw-microcode analyzer and DXBC
      translator (`Fable2_661.log`: 24-dword VS to 5,536-byte DXBC).
- [x] Add owned, collision-safe shader input caching shared by validation and host backends.
- [x] Build and cache a D3D12 root signature/graphics PSO from a live captured VS/PS pair
      (`Fable2_664.log`: VS `EBFF6B02D047092B`, PS `8391FFB8F4EA66F3`).
- [x] Bind translated constant buffers and the shared-memory SRV/UAV.
- [x] Submit one known auto-index frontend draw into a host render target.
- [x] Render one known frontend draw.
- [x] Expand Xenos rectangle-list primitives into a complete four-vertex triangle strip
      (`Fable2_679.log`: exactly 518,400 changed pixels over a `960x540` rectangle).
- [x] Retain and composite multiple guarded draws per swap without clearing between them
      (`Fable2_681.log`: 24 submitted draws in the first accumulated frame).
- [x] Snapshot, hash, and bind live vertex/pixel float constants plus shared bool/loop constants.
- [x] Snapshot DMA index buffers, preserve Xenos index endian conversion, and issue indexed draws.
- [x] Bind dimension-correct null texture SRVs and sampler tables as a guarded texture-path proof
      (`Fable2_685.log`: textured indexed sequences 104 and 106 retained).
- [x] Snapshot and upload the first real guest textures: linear, endian-none `k_8` YUV planes;
      bind R8 UNORM/SNORM views, preserve fetch constants/signs, and submit sequence 104
      (`Fable2_692.log`).
- [x] Audit Skate3Recomp's title-hook/native-scene architecture and capture the applicable
      guest-to-host resource, fallback, RHI, and cache-lifetime patterns in
      `NATIVE_PC_MEMORY.md`.
- [x] Promote one-shot texture resources into a persistent, byte-accounted D3D12 host store keyed
      by the raw descriptor plus visible-content fingerprint, with bounded LRU eviction
      (`Fable2_696.log`: three uploads followed by three cache hits and no repeat upload).
- [x] Track per-range guest dirty generations through physical-memory invalidation callbacks;
      take coherent generation-bracketed snapshots and skip guest copying/hashing when a ready
      cached resource is unchanged (`Fable2_698.log`: streamed writes reject reuse at `0/3`;
      `Fable2_699.log`: stable frame qualifies at `3/3`).
- [x] Defer transient upload/readback destruction by submission fence rather than waiting the
      whole queue solely for local resource lifetime (`Fable2_701.log`: three upload heaps,
      1,474,304 bytes, assigned to submission 103 and safely retired).
- [x] Rotate draw recording across three independently fenced allocator/list, constant-buffer,
      descriptor, vertex, and index resource slots; prevent eviction of textures referenced by an
      incomplete submission (`Fable2_702.log`: two native draws simultaneously in flight).
- [x] Coalesce adjacent sequence-contiguous draws with byte-for-byte identical captured GPU
      payloads into one command-list submission and one output copy/presenter refresh
      (`Fable2_704.log`: 24 candidates, one batch, exact textured-frame hash preserved).
- [x] Batch heterogeneous retained runs under one presenter refresh and final output copy; group
      up to three independently resourced command lists under one fence submission
      (`Fable2_706.log`: 26 logical draws, three runs, one copy and one signal).
- [x] Preserve full PM4 register indices in guest-visible side effects and lock down Fable's
      1,024-dword upload with a regression (`Fable2_733.log`: continuous unforced YUV motion).
- [x] Add Vulkan command recording and diagnostic clear parity.

Final proof: `Fable2_679.log` submits captured rectangle-list sequence 25 using 84 bytes of live
guest vertex data. Its geometry stage forwards `TEXCOORD0..15` plus `SV_Position`, chooses the
longest diagonal, and mirrors the correct opposite vertex. Readback finds exactly 518,400
non-clear pixels spanning `(320,180)..(1279,719)`, a complete `960x540` rectangle copied to and
presented by the native presenter without PSO, D3D12, or device-loss errors. The current guard
accepts zero/one-fetch auto-index or DMA-indexed point, line, triangle, and rectangle draws with
known constant state and fully known texture descriptors. `Fable2_681.log` advances this to a
bounded 32-candidate per-frame list and correctness-first sequential accumulation on one private
target. `Fable2_692.log` advances the retained set again: sequence 104 carries 1 VS and 4 PS float
vectors, six DMA indices, and three real linear `k_8` texture uploads (1280x720 Y plus two
640x360 chroma planes). A disabled-by-default eager proof mode bypasses the static frontend's
missing later swap and produces a full 1280x720 non-black readback using the original translated
YUV pixel shader. The first host image is the game's near-white intro-video frame. `Fable2_696.log`
replays that frame from three persistent host-cache entries using 1,507,328 actual D3D12 bytes,
with no second upload. Logs 698 and 699 prove that generation tracking distinguishes an actively
rewritten video source from a stable cached source without trusting guest address identity. Log
704 coalesces the first 24 identical retained draws into one command-list batch without changing
the known textured output. Log 706 groups the real three-run/26-draw frame under one presenter
refresh, output copy, and fence signal. Next, collapse those runs into one command list using
offset per-draw resource slices, retain subsequent video/game frames, broaden the guarded
format/layout set, and add title registration/rebind/destruction lifecycle hooks.

Acceptance: the logo/frontend vertical slice renders through our command processor and backend
without using xenos draw assembly.

## Later ownership

- [ ] P2 gameplay textures, render targets, resolves, EDRAM replacement, and presentation.
  - [x] Snapshot and compact multiple referenced vertex-fetch streams per draw; rewrite every
        fetch constant to the corresponding bounded host-buffer offset (`Fable2_949.log`).
  - [x] Add linear/tiled R16 gameplay texture upload and host sampling.
  - [x] Capture and hash `VGT_OUTPUT_PATH_CNTL`; prove the first one-vertex primitive-0x12
        gameplay boundary is a quad tessellation patch (`output_path=1` in `Fable2_950.log`).
  - [x] Integrate native D3D12 hull/domain shaders plus adaptive/continuous/discrete quad
        tessellation factor and index handling (`Fable2_954.log`, `Fable2_961.log`).
  - [x] Add six-face cube upload plus DXT3A, R8G8, tiled R8, and packed 1-5-5-5 gameplay formats
        (`Fable2_955.log`, `Fable2_957.log`).
  - [x] Apply guest reversed-Z state through a real D32 attachment; resolve depth as shader-readable
        R32 and round-trip persistent depth between prepass and color passes (`Fable2_961.log`).
  - [ ] Add mip-chain upload/sampling and remaining array/stacked texture dimensions.
  - [ ] Complete stencil, MSAA depth, and multiple simultaneous color-target semantics.
- [x] Turn one-shot guest texture snapshots into a persistent byte-budgeted host texture store
      keyed by descriptor plus content fingerprint.
- [ ] P3 owned Xenos shader translation or a deliberately trimmed, understood translator fork.
- [ ] P4 make `rexgpu-native` default and add PC-native resolution/MSAA/post-processing features.
- [ ] P5 replace PM4-emitting game functions with direct renderer calls as decomp coverage permits.
