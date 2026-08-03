# Own Graphics Pipeline — replacing rexgpu-xenos with our own renderer (Vulkan/D3D12)

**Goal (user directive 2026-07-19):** stop relying on ReXGlue's Xenia-derived Xenos emulation for
graphics. Build *our own* pipeline: our command processor, our backend (Vulkan or D3D12), sized to
what Fable II actually uses — the graphics rung of the ownership ladder
([RUNTIME_OWNERSHIP.md](RUNTIME_OWNERSHIP.md) step 4).

This doc records the decomp session that mapped the game's 360 D3D layer (the foundation), the key
architectural finding that fixes the strategy, and the phased plan.

## Native PC architecture rule

The destination is a PC game running a PC-native renderer, not a permanent simulation of an Xbox 360 GPU.
PM4 packets, fetch constants, Xenos shaders, resolve commands, and EDRAM addresses still have to be decoded
because the unchanged game emits them, but they are a frontend ABI—not the host rendering architecture.

Normalize that input into a title-specific render graph backed by native D3D12/Vulkan textures, buffers,
descriptors, barriers, queues, fences, and unrestricted host heaps. Guest EDRAM aliases should become native
resource identities and generations; resolves should become host copies, blits, or render-graph edges; guest
RAM must not remain the canonical backing store for render targets. The finished port must not inherit the
360's 10 MiB EDRAM window, guest RAM ceiling, or avoidable render-target round trips.

The host-native resolve graph is now unconditional.
`REXGPU_NATIVE_RESOLVE_HEAP` and its compatibility gate have been removed.
Independent persistent color/depth resources are keyed by guest target
identity, while registry entries reference those actual draw resources rather
than duplicate snapshot textures. The former seed/store copies are gone.
Guest EDRAM bases therefore serve only as frontend identity and alias labels;
they do not select a fixed backing heap.

## Current live status (2026-08-02)

The native D3D12 path is operational through frontend/loading/UI, native HDR resolve, final
compositor, and present. The loaded Hero001 world is still flat blue-gray, but this is not an empty
or missing-present path: the compositor binds live finite HDR from `0x19C67000` (1120x720,
R16G16B16A16_FLOAT), with approximately peak `1.0605` and mean max `0.0811`. A ×16 pixel-output
diagnostic scales that measurement to approximately `16.97` and `1.297`, proving the native scene
writes real pixels that are materially under-scaled or computed incorrectly.

The 1D texture resource fix is retained and materially improved loading screens. Predicate-true,
always-pass-depth, and exposure=1 A/Bs did not restore the world. The next work is a valid per-material
shader probe, not more compositor/exposure guesses.

Important implementation detail: late-world draw diagnostics print translator cache keys, while the
shader translator target selector compares guest microcode hashes. The exact five late-world mappings
are documented in [SESSION_SNAPSHOT_2026-08-02_NATIVE_RENDERER.md](SESSION_SNAPSHOT_2026-08-02_NATIVE_RENDERER.md).

Screenshot helpers foreground the game by default; that can change title/menu/loading visibility and
must be treated as an active presentation intervention. Use passive capture only when the game is
already foreground.

## Previous live status (2026-07-29 late night)

The architecture above remains the target, but the present implementation is
not at gameplay parity. A clean native run renders frontend and loading
correctly, then produces a black 1120x720 world HDR surface with UI/glows
still visible. The final compositor does consume that native resolved surface.
A `LEQUAL` diagnostic proves color geometry is present but behind the stored
reverse-depth prepass; normal `GEQUAL` rejects nearly all of it. A small
positive bias did not help, and paired fingerprints rule out a random
cross-frame batch.

The same late scene repeatedly replays large retained populations: roughly
1.41 million `SubmitDrawProof` calls and 18,198 draw-slot waits were observed
in one bounded run. This is not the intended end-state render graph. Resolve
history must become persistent native pass/resource generations rather than
fresh draw replay.

The Lionhead planar-video sequence also flashes white between valid frames.
See
[SESSION_SNAPSHOT_2026-07-29_LATE.md](SESSION_SNAPSHOT_2026-07-29_LATE.md)
for captures, exact diagnostics, RenderDoc automation, and the resume order.

## Current visual milestone (2026-07-27)

- The title screen now reaches the blue sparkling `FABLE II` layer and the `Press     to start` text through
  the native D3D12 path. The opaque logo, scenic 3D background, and controller `A` glyph are still missing.
- The missing `A` draw is isolated exactly between the two text runs: `Press`, one generic six-index image
  quad, then `to start`.
- Its source is `Art/GUI/Controller/icon_button_a.tex` at guest base `0x0D367000`: 64x64, pitch 64,
  Xenos format 6 (`8_8_8_8`), tiled, endian 2, swizzle `0x60A`, with alpha blending.
- Geometry and texture decoding are verified. The remaining failure is downstream of upload—descriptor
  binding, pipeline execution, depth/composition order, or accumulated resolve/presentation state.
- Native texture component-swizzle composition, BC4 replication, and real sampler creation are implemented.
  The suite passes 463 assertions across 28 test cases, plus the native plugin smoke test.

The missing `Press A` prompt and save/main menus are now traced to their actual producer. At the
menu, the native frame already retains 45 supported textured draws and 74 textureless companions.
The final frontend pass samples `0x0C6E4000`, but that 560x360 resolved surface is all zero in
guest memory. PM4 resolve telemetry identifies the exact write to the same address. This rules
out the glyph atlas, packed BC mip tails, and generic heap/timing speculation as the immediate
blocker: the native renderer must own the offscreen render-target graph.

The host-only resolve graph retains per-draw color/depth target identities,
routes matching draws at resolve events, keeps resolved D3D12 resources keyed
by guest destination address, and binds/presents them without copying through
the emulated 360 RAM heap. Compatible resolved and persistent targets are
reused unless a retained draw still pins the older generation. Color and depth
attachments are activated independently, avoiding unrelated target switches
and clears. `Fable2_1082.log` crossed resolve 16,384 in late-world rendering
under D3D12 debug+DRED without a barrier/state error or device loss; selective
activation reduced checkpoint-2048 target switches from 23,557 to 14,352.

The owned renderer now crosses the video-to-frontend boundary. Planar frames are submitted at
guest swaps (`Fable2_746.log`), unsupported companion draws no longer overwrite them, and frontend
composition takes over when supported guest textures arrive. The native texture path covers tiled
RGBA8, 10:10:10:2, RGBA16F, BC1, BC2, BC3, and BC4/DXT5A with endian conversion, untile, coherent
guest snapshots, persistent host caching, and base-mip fallback. Guest `RB_BLENDCONTROL0` and
color-write state are part of the D3D12 PSO key, so alpha-composited UI no longer appears as
opaque black rectangles.

`rexglue-src/out/native_753_frontend_oriented/frame_00.png` is the first clean, upright,
recognizable non-video frame: Fable II's Microsoft/Lionhead legal text rendered by translated
guest shaders through our command processor and D3D12 backend. The frontend Y convention is now
separate from the proven bottom-up planar-video convention. `Fable2_755.log` validates native 1D
R32F/RGBA/RGBA16F lookup textures as well, and all 28 focused PM4/native-plugin tests pass.

The owned D3D12 path now produces continuous, unforced Fable II boot-video motion. The blocker was
a native command-processor address-domain bug: PM4's full 15-bit register indices were converted
to byte offsets and truncated through the 16-bit MMIO aperture. Fable's Type-0 constant upload at
`0x4000..0x43FF` consequently aliased `0x41DC/0x41DD` onto
`SCRATCH_UMSK/SCRATCH_ADDR`, disabled scratch writeback, and stranded the swap mailbox. PM4
side effects now operate directly on full register indices while real MMIO retains its aperture
mask. A 1,024-dword regression locks down the exact failure shape.

Clean live D3D12 logs `Fable2_733.log`, `Fable2_738.log`, and `Fable2_740.log` translate the first
Fable shader, accept the first VS/PS pair into a host PSO, present a vertex-backed draw, and submit
changing three-plane YUV frames
without `REXGPU_NATIVE_FORCE_STALLED_WAIT`. It reaches frames 1, 2, 3, 4, 8, 16, 32, 64, 128,
and 256 with distinct content fingerprints and draw sequences through 7017. At 600 host-vblank
polls it had submitted 291 frames, with no malformed ring span, rejected PM4 packet, D3D12
submission error, or device loss.

This crosses the motion threshold with recognizable, correctly colored video. The old near-white
BMP was a deliberately one-shot capture of streaming frame 1, not the current live output. Timed
window captures in `rexglue-src/out/native_738_boot_sequence/` show a clean Microsoft Game Studios
frame (`frame_14.png`) and the Lionhead animation (`frame_20.png`). The first live capture also
revealed that the planar surfaces were vertically inverted; reversing the source rows of all three
planes during the streaming upload fixes orientation without disturbing the required D3D NDC
transform.

Planar streaming is now part of normal presenting D3D12 initialization. Final log
`Fable2_740.log` used no renderer proof variable, selected the real 4:2:0 draw automatically, and
reached frame 128 in eight seconds. Headless initialization remains unchanged, diagnostic artifact
dumping remains explicitly opt-in, and the former stream variable is only a diagnostic override.
A regression locks down order-independent 1280×720 + 640×360 + 640×360 classification and
bottom-up row mapping.

The first normal run exposed two presentation-ownership bugs. Incomplete companion proof batches
were presented over the planar frame, causing a flashing black rectangle, and independent
host-vblank polling sampled decoder surfaces between guest draw boundaries, causing bottom-edge
garbage during the post-logo transition. Planar video now takes exclusive output ownership and is
updated only by a completed guest planar draw. `Fable2_743.log` and
`rexglue-src/out/native_743_draw_paced_sequence/` prove a clean 50-frame boot capture whose
post-Lionhead frames 37-49 remain byte-stable.

The next visual target is the complete menu scene: add the depth/HDR postprocess inputs, retain the
complete frontend draw set, and route guest render targets/resolves rather than
compositing every supported draw directly into the proof target. Visual runs must specify both
`--gpu_plugin native --gpu_backend d3d12`; omitting the backend selects headless validation.

---

## 1. What the decomp found (2026-07-19 session, all labeled in Ghidra)

### The 360 D3D runtime inside the exe
Fable II statically links Microsoft's Xbox 360 D3D library. It was invisible until now because it
sat in the known over-merged blob `0x82B9F038–0x82BAF08C` — **carved this session into 119 real
functions via `.pdata`** (`tools/ghidra_label/CarvePdataRange.java`), plus satellite code around
`0x8219x–0x822Cx` and the XG utilities at `0x82B99xxx/0x821FBxxx/0x8225Fxxx`.

Core anchors (all named, `ghidra_out/labels_d3d_layer.tsv`, 65 labels applied):

| Function | Addr | Role |
|---|---|---|
| `D3DDevice_Swap` | 0x82BA34D8 | Present: fences, `VdSwap(frontbuffer…)`, display info |
| `D3D_SwapWorkerThread` | 0x82BA5D08 | dedicated swap thread (`XSetThreadProcessor`) |
| `D3D_DeviceInit` | 0x82BA6990 | `VdInitializeEngines`, interrupt cb `0x82BA26B0`, `VdGlobalDevice` |
| `D3D_InitRingBuffer` | 0x82BA2830 | ring alloc + `VdInitializeRingBuffer` + PM4 `ME_INIT` |
| `D3D_CmdBufReserveSpace` | 0x821E8EC0 | ★ THE kick/MakeSpace — every PM4 writer calls this |
| `D3D_Resolve_EmitCopyDraw` | 0x82206F30 | EDRAM resolve (copy-regs + rectangle draw) |
| `D3D_LoadShaderMicrocode` | 0x8222BBF8 | shader ucode into command stream (`IM_LOAD_IMMEDIATE`) |
| `D3D_GetGlobalDevice_TLS` | 0x82CA93F4/F8 | device getters (~2000 call sites) |

Device struct essentials: `dev+0x30` = PM4 write cursor, `dev+0x38` = limit (cursor past limit →
call kick), `dev+0x2A90` = system command buffer block, `dev+0x3A30..0x3A58` = ring segments.
Kernel boundary = the `Vd*` import thunks at `0x832BA7xx` (Swap/ring/interrupt/scaler/EDRAM).

### ★ The architectural finding: the D3D API boundary is DISSOLVED by LTCG
The six standalone draw-packet writers (`D3D_EmitDrawIndx*`) have **zero references** — no `bl`,
no tail `b`, no address-takes, no data pointers (verified with `FindBranchTo.java` +
`FindValueInData.java`). Meanwhile the kick has **144 call sites smeared across game code**
(`0x8219x…0x82ABx`, `ghidra_out/d3d_api_callers.txt`). Conclusion: the 360 D3D library was
**LTCG-inlined into Fable's renderer** — PM4 packet writing is fused into game functions; the
standalone copies are unreferenced cold code.

**Consequence:** "intercept the D3D API calls and reimplement them natively" is NOT viable — there
is no call boundary to hook. The clean boundaries are:
1. **Below: the PM4 command stream** (ring buffer + kick + `VdSwap`) — narrow, complete, already
   what the GPU plugin consumes. ← *the boundary for owning the pipeline now*
2. **Above: Fable's own renderer classes** (mesh/material/scene draw) — the eventual decomp target
   (Track B / step 5), a much bigger lift.

### What Fable actually emits (the subset our renderer must support)
From decompiling all 88 command-writer functions (`ghidra_out/d3d_writers_decomp.txt`), the PM4
type-3 opcode set in use:

- **Draws:** `DRAW_INDX 0x22` (auto + indexed forms), `DRAW_INDX_2 0x36` (immediate, used by
  resolve/blits/XPS export)
- **State:** `SET_CONSTANT 0x2D` (register blocks), `LOAD_ALU_CONSTANT 0x2F` (shader constants),
  `IM_LOAD 0x27` / `IM_LOAD_IMMEDIATE 0x2B` (shader microcode), `INVALIDATE_STATE 0x3B`
- **Sync:** `EVENT_WRITE 0x46`, `EVENT_WRITE_SHD 0x58`, events `0x5A`, `WAIT_REG_MEM 0x3C`,
  `MEM_WRITE 0x3D`, `REG_RMW 0x21`
- **Control:** `NOP 0x00`, `ME_INIT 0x48`, `INDIRECT_BUFFER 0x3F` (secondary command buffers)
- **Tiling:** `SET_BIN_MASK/SELECT LO/HI 0x60–0x63` (predicated tiling — reset each Swap)

That is ~18 opcodes — dramatically smaller than a general Xenos emulator. Same for registers: the
writers hit a bounded register set (copy/resolve regs, RB/VGT/SQ state) enumerable from the
`SET_CONSTANT` payloads.

---

## 2. The plan: our own GPU plugin, then climb

The GPU plugin interface (`--gpu_plugin xenos` → `rexgpu-xenos.dll`) is already a clean seam: it
receives the ring buffer/`Vd*` traffic. We build **`rexgpu-native`** — our command processor + our
backend — behind the same seam, A/B-swappable with xenos at any time (`--gpu_plugin`).

**Backend choice:** D3D12 first, with Vulkan following each proven backend milestone.
D3D12 remains the fastest correctness oracle for the current Windows target and can initially use
the existing DXIL translation infrastructure while we own the CP and pipeline. Vulkan is already
technically viable: ReXGlue contains a Vulkan instance/device/provider, presentation path, SPIR-V
translator, volk, and VMA, and a `D3D12=OFF`, `Vulkan=ON` build now successfully links the native
plugin. The normalized PM4 contract is API-neutral, so Vulkan is a second backend rather than a
rewrite. The first live D3D12 `XE_SWAP`-driven diagnostic clear is now proven, and a selectable
`VulkanPm4Backend` owns the Vulkan provider/device/graphics queue, command pool/buffer, submission
tracking, and its own live diagnostic-clear path. D3D12 remains the first known-draw path; Vulkan
will follow the shared shader/pipeline/resource milestones.

### Phases
- **P0 — Shadow tracer (no risk, high knowledge).** A sideline PM4 parser (ours, from scratch)
  that observes the same ring xenos consumes and logs/validates: opcode histogram, register
  writes, draws, resolves, shader loads. Run alongside xenos → prove our parser understands 100%
  of Fable's stream (count unknown packets = 0 across boot→town→combat). Deliverable:
  `rexglue-src/src/graphics/native/pm4_parser.{h,cpp}` + `FABLE2_PM4TRACE=1`.
  - ▶ **PARSER CORE SHIPPED + UNIT-VERIFIED (2026-07-20 resume).** `pm4_parser.{h,cpp}` written
    (dependency-free: type-0/1/2/3 header decode, Fable's ~18-opcode subset table, `Pm4Stats`
    histogram + draws/reg-writes/shader-loads/resolves/IB tallies, **unknown-opcode counter** =
    the P0 completeness metric, truncation-safe with a `malformed` guard, optional bounded
    INDIRECT_BUFFER recursion via an `IbResolver`). Compiled with the project clang + ran a
    standalone self-test (`scratchpad/pm4_test.cpp`): all assertions pass — every dword accounted
    for exactly once, unknown/malformed detection correct. `Pm4TraceEnabled()` reads
    `FABLE2_PM4TRACE`.
  - ▶ **P0 TAP WIRED + RUNTIME BUILT (2026-07-20 resume).** `command_processor.cpp`
    `ExecutePrimaryBuffer` now shadow-parses the SAME [read_index, write_index) span the CP is about
    to execute, gated by `FABLE2_PM4TRACE`: a `memory::RingBuffer` mirror reads+byte-swaps the window
    into a scratch vector (`ReadAndSwap`, wrap-safe), then `Pm4Parser::Parse()` walks it with an
    `IbResolver` (`Pm4IbResolve`) that follows `INDIRECT_BUFFER`/`_PFD` into secondary buffers
    (`TranslatePhysical(addr & 0x1FFFFFFF)` — GpuToCpu=id, CpuToGpu mask; list_length cap 0xFFFFF;
    stable-address `std::deque` storage during recursion). Cumulative `Pm4Stats` dumped via
    `REXGPU_INFO` every 512 primary buffers. Runs on the single GPU worker thread (no locking).
    Added `native/pm4_parser.cpp` to `src/graphics/CMakeLists.txt`; `build_runtime2.cmd` → exit 0;
    patched `rexruntime.dll` staged into the nightly exe dir. Opcode table corrected vs the runtime
    `xenos.h` IT_* enum (NOP=0x10 not 0x00; +INDIRECT_BUFFER_PFD 0x37; EVENT_WRITE_EXT 0x5A).
    ▶ Historical next step was a live completeness run; completed below.
  - ▶ **FIRST LIVE RUNS DONE (2026-07-20 resume, logs 596–600).** ⚠ The CP lives in the
    `rexgpu-xenos.dll` PLUGIN, NOT `rexruntime.dll` — build/stage the **`rexgpu-xenos`** target
    (`cmake --build … --target rexgpu-xenos` then copy `out/win-amd64/Release/rexgpu-xenos.dll` into
    the nightly run dir). The tap fires (`[pm4]` dumps every 512 primary buffers) and reads the LIVE
    stream: ~34–38k INDIRECT_BUFFERs, `SET_BIN_SELECT_LO 0x62` ~17k, real DRAW_INDX/DRAW_INDX_2 when
    the scene draws, heavy type-2 NOP + zero padding. **Framing bugs found + fixed by comparing to the
    CP:** (1) `header==0` = 1-dword skip (was mis-read as type-0 → 155M phantom packets); (2) type-1 =
    FIXED 2-dword body (was count-based → desync); (3) completed the opcode table from the runtime
    `xenos.h` PM4_* enum (SET_SHADER_CONSTANTS is 0x56 not 0x2E; +BIN/WAIT/MEM_WRITE/DRAW_INDX_BIN)
    → UNKNOWN 15327→2645. **Historical blocker: ~30% of primary buffers desynced
    (`malformed`≈30k) though the CP
    logs ZERO packet errors** → one remaining per-packet size mismatch (the low-count bogus opcodes
    0x13/0x08/0x01 are debris from it). Under investigation (workflow pm4-desync-hunt: type-3 handler
    advancement / ring wrap / IB window / partial-tail).
  - ▶ **P0 COMPLETE (2026-07-25, logs 619–620).** Root cause was not another packet-size rule:
    the tracer scanned the whole primary buffer and recursively snapshotted IB memory *before*
    the real CP executed it. Fable dynamically populates/reuses some IBs during depth-first
    execution, so the snapshots were stale data that happened to resemble packets. The tap now
    observes exactly one packet inside `ExecutePacket`, immediately before the real CP executes it.
    Nested IBs are therefore observed at their real execution time and in their real order; the
    shadow parser no longer speculatively recurses in production. Two controlled live runs:
    log 619 = 2,155,378 packets / 144,924 draws / 18,425 IBs, `UNKNOWN=0`, zero CP packet errors;
    log 620 = 1,724,994 packets / 116,558 draws / 14,883 IBs,
    **`UNKNOWN=0 malformed=0(ib=0)`**, zero CP packet errors. The bogus opcode debris vanished.
    Dedicated `pm4_parser_tests` target adds 31 assertions across framing, padding, unknown and
    truncated packets, persistent predication, stable recursive IB parsing, and dynamic
    execution-order IB observation. **Next = P1 register-state shadow + normalized draw events.**
  - ▶ **P1 STATE BASELINE SHIPPED (2026-07-25, logs 621–626).**
    `native/pm4_state.{h,cpp}` is a dependency-free 16-bit register-space shadow with inline and
    memory-backed constant loads, type-0/type-1 writes, REG_RMW, bin predication, extended
    registers, gamma-ramp side effects, draw-register assembly, and normalized `Pm4DrawEvent`
    records. It runs beside xenos and compares its known standard registers against the real CP
    every 512 primary buffers. Live convergence: log 623 found two control-side-effect gaps
    (`COHER_STATUS_HOST` clear + `DC_LUT_RW_INDEX` auto-increment); after modeling/classifying them,
    log 626 processed 128,237 packets and 14,363 normalized draws with
    **2,466 known registers compared, zero mismatches**, `invalid=0`, `oor=0`, `UNKNOWN=0`,
    `malformed=0`, and zero real-CP packet errors. Focused target now passes 75 assertions in
    10 cases. Remaining P1 work is tracked in [OWN_RENDERER_TASKS.md](OWN_RENDERER_TASKS.md).
  - ▶ **P1.1 DETERMINISTIC CAPTURE/REPLAY SHIPPED (2026-07-25, log 627).**
    Added a versioned `F2PM4CAP` little-endian format, bounded env-gated live writer
    (`FABLE2_PM4_CAPTURE`, optional `FABLE2_PM4_CAPTURE_MAX_MB`), dependency records for
    `LOAD_ALU_CONSTANT`, pointer-based `IM_LOAD`, and memory-polled `COND_WRITE`, strict reader,
    replay library, and standalone
    `pm4_replay` CLI. The test target now passes 99 assertions in 12 cases, including deterministic
    round-trip, clean cap behavior, and truncated-record rejection. Live 8 MiB capture:
    175,764 executed packet records, 1,487 memory records / 39,108 dependency dwords, zero missing
    dependencies. Two offline replays were identical: 116,269 non-padding packets, 13,263 draws,
    `unknown=0 malformed=0 invalid=0`, packet hash `03F8E1006C0E7534`, normalized draw hash
    `8C29776C0270E4DD`, register hash `A5C658D6DCD87A30` after applying the P1.2/P1.3 semantics.
    This is the deterministic corpus/harness for
    finishing P1 without repeatedly launching the game.
  - ▶ **P1.2 SEMANTIC COVERAGE BASELINE SHIPPED (2026-07-25, log 629).**
    The state shadow now models the observable register effects of `EVENT_WRITE*`, `VIZ_QUERY`,
    and register/memory-polled `COND_WRITE`, and explicitly separates state-semantic packets,
    understood control packets, and unclassified semantics. `XE_SWAP` now emits a normalized frame
    event and hashes the complete known-register snapshot; production compares both the fast
    register file and shadow-known sparse extended registers at every executed swap. The focused
    target passes 125 assertions in 14 cases. The saved boot corpus replays 481 swaps with identical
    snapshot-stream hash `2559219BD700E6D7`, alongside 19,746 state-semantic + 15,698 control
    packets and `unclassified=0`. Controlled live log 630 reached 958,950 packets, 68,049 draws,
    and 656 swaps with **`unclassified=0 invalid=0 oor=0 swap_mismatches=0`**. Sampled swaps matched
    2,473 known standard registers plus the exercised sparse extended register. Only the longer
    loaded-world/combat/effects scenario remains for full P1.2 acceptance.
  - ▶ **P1.3 DRAW ASSEMBLY STARTED (2026-07-25, log 631).**
    `Pm4DrawEvent` now resolves DMA index buffers into an aligned guest address, index format and
    element size, endian mode, available element count, and byte length without depending on xenos
    command-processor types. A small standalone microcode scanner also tracks active vertex/pixel
    shader hashes, extracts the vertex shader's 96-bit fetch mask, and joins only referenced fetch
    constants to the draw as slot/type/endian/address/size descriptors. These fields participate in
    the deterministic draw hash (`DDF847AA0D4DAF98` for the saved corpus); 3,441 captured shader
    loads analyze with zero failures. The focused target passes 141 assertions in 14 cases. Live
    log 632 checked the independent fetch mask against the runtime shader analyzer on 64,969 draws:
    **zero mask mismatches**, zero shader-analysis failures, and 18,769 resolved active descriptors.
    Index classification covered 5,265 DMA draws (16-bit, 8-in-16 endian) plus 63,352 auto-index
    draws with no immediate/reserved sources, understanding gaps, or parity errors. Next is render
    target state and translator/cache metadata.
  - ▶ **P1.4 SHADER INVENTORY BASELINE SHIPPED (2026-07-26, log 633).**
    Executed `IM_LOAD` and `IM_LOAD_IMMEDIATE` payloads are hashed and deduplicated by stage, with
    bytecode size, load count, and vertex-fetch mask retained in a deterministic inventory.
    `pm4_replay --inventory <capture>` prints the report without launching the game. The saved
    8 MiB corpus contains 3,441 loads but only 19 unique shaders (7 vertex + 12 pixel). Live log 633
    reached 30,423 loads and 27 unique shaders (10 vertex + 17 pixel), with zero shader-analysis
    failures, fetch-mask mismatches, semantic gaps, or parity errors. The focused target now passes
    148 assertions in 14 cases. Remaining P1.4 work is broader constant/fetch-usage metadata,
    translator cache keys, and saturation across the representative gameplay scene suite.
  - **P1.3/P1.5 RENDER-TARGET AND RESOLVE IR SHIPPED (2026-07-26, log 636).**
    Every normalized draw now snapshots backend-neutral EDRAM pitch/MSAA/mode, four color
    base/format/exponent-bias/write-mask descriptors, depth base/format/control, explicit register
    knownness, and a compact attachment-transition mask. Copy-mode draws additionally emit
    `Pm4ResolveEvent` records with decoded source selection, copy/sample/clear control and complete
    destination base/pitch/height/array/slice/format/endian/bias/swap state. The saved corpus has
    13,263 draws, 1,962 attachment transitions, and 497/497 complete resolves (496 color, 1 depth);
    repeated replay produced draw hash `CB0E7E75AEC07B26` and resolve hash `FF3915A416DED5CC`.
    The focused target passes 212 assertions in 15 cases. Live log 636 reached 219,370 draws,
    29,836 transitions, 9,997/9,997 complete resolves, and 1,079 swaps with
    `UNKNOWN=0`, `malformed=0`, `unclassified=0`, `invalid=0`, `oor=0`, zero shader/fetch-mask
    failures, and zero state/swap parity mismatches.
    Ghidra constant scans and focused decompilation (`renderer_resolve_signatures_20260726.log`,
    `renderer_copy_emitters_20260726.log`) confirmed that the cold resolve helper has no direct
    references after LTCG and that generic draw/load packet signatures are shared by export and
    frontbuffer-blit emitters. Runtime `RB_MODECONTROL=kCopy` plus `RB_COPY_*` state is consequently
    the reliable resolve boundary.
  - **P1.4/P1.5 TEXTURE-FETCH IR SHIPPED (2026-07-26, log 637).**
    The independent shader scanner now retains texture-fetch masks for both stages. Every referenced
    slot is joined to its six fetch-constant registers as a backend-neutral descriptor containing
    explicit per-dword knownness, stage ownership, base/mip addresses, dimensions, format/endian,
    signs, swizzle, filters, LOD, and packed-mip state. Unknown or invalid descriptors remain visible
    in the IR rather than being dropped. Descriptor content participates in the deterministic draw
    hash and shader inventory rows now report texture masks. The saved corpus contains 2,857/2,857
    complete descriptors over 967 draws; repeated replay is identical with draw hash
    `0EE9EC8C218CA3EA`. The focused target passes 278 assertions in 17 cases. Live log 637 reached
    181,136/181,136 complete descriptors and 516,243 per-stage comparisons with the existing shader
    analyzer with **zero texture-mask mismatches**, alongside zero vertex-mask, shader-analysis,
    semantic, packet, state-parity, or swap-parity errors. Next is float/bool/loop constant usage
    and deterministic translator modification/cache keys.
  - **P1.4 CONSTANT USAGE + TRANSLATOR KEYS SHIPPED (2026-07-26, logs 638/641).**
    The dependency-free scanner now decodes ALU constant sources and control-flow bool/loop
    references directly from raw microcode, including the all-256 float mask required by dynamic
    a0/aL addressing. Draw records retain each active stage's usage metadata. Inventory rows expose
    float/bool/loop masks, counts, dynamic-addressing state, and a versioned 64-bit translator key
    formed from stage, bytecode identity, and normalized usage. Live uniform and fetch values are
    deliberately excluded because they are runtime inputs, not host-shader variants. Repeated saved
    replay is identical with draw hash `8EECEFDE3D074DC3`; the focused suite passes 299 assertions
    in 18 cases. Log 638 compared 250,910 stage records with `Shader::ConstantRegisterMap` with zero
    mismatches. The final staged key-bearing build reached 135,816 zero-mismatch comparisons in log
    641, with all prior packet/fetch/register parity invariants still clean. P1.4 metadata is now
    complete; the active implementation target moves to the separate `rexgpu-native` validation
    plugin/backend seam.
  - **NATIVE VALIDATION PLUGIN SEAM SHIPPED (2026-07-26).** `rexgpu-native` is now a separate,
    installable ABI-1 plugin with an owned headless MMIO/ring worker. Normalized draw, resolve, and
    present events cross the backend-neutral `Pm4Backend` interface; the live worker and saved
    replay both use `Pm4Stream` and `Pm4ValidationBackend`. The saved capture retains all aggregate
    hashes and adds 481 deterministic frame records. The focused suite passes 306 assertions and a
    DLL load/factory/setup/shutdown smoke test. This is a validation seam only: D3D12 presentation
    and the remaining guest-visible command side effects are the next slice.
  - **D3D12 FOUNDATION + VULKAN BUILD PROOF SHIPPED (2026-07-26).** Backend construction is now
    injected behind API-neutral identity/capability/diagnostic fields. `--gpu_backend=d3d12`
    selects `D3D12Pm4Backend`, which owns a real device and direct queue while still delegating
    events to deterministic validation until draw submission exists. Nightly log `Fable2_645.log`
    confirms device/queue creation and a bounded live host smoke. A separate Windows configuration
    with D3D12 disabled and Vulkan enabled builds `rexruntime` and `rexgpu-native`; that proof found
    and fixed a common-presenter DXGI link dependency that had been incorrectly gated on D3D12.
  - **LIVE PM4 SIDE EFFECTS + FIRST HOST SUBMISSION SHIPPED (2026-07-26, log 656).**
    `Pm4SideEffects` now performs endian-correct `MEM_WRITE`, `REG_TO_MEM`, `COND_WRITE`,
    `EVENT_WRITE_SHD`, blocking `WAIT_REG_MEM`, Type-0/Type-1 register writes, and packet
    interrupts. `NativeGraphicsSystem` mirrors scratch registers to guest memory and dispatches
    PM4 interrupts on a guest-capable host thread. This completes Fable's scratch/interrupt
    semaphore handshake and reaches `XE_SWAP`; `D3D12Pm4Backend` records, submits, and presents a
    1280x720 diagnostic clear with no rejected packets or malformed spans. The focused suite passes
    342 assertions in 20 cases and all saved replay hashes are unchanged. A selectable
    `VulkanPm4Backend` now owns the Vulkan provider/device/graphics queue behind the same contract.
  - **VULKAN COMMAND RECORDING + LIVE CLEAR SHIPPED (2026-07-26, log 659).**
    The Vulkan backend now owns a graphics-family command pool/buffer and submission tracker,
    performs the presenter mailbox image's first-use/internal-layout transitions, and submits on
    the externally synchronized graphics queue. The live official host reports
    `command_recording=true` and `clear_submission=true`, completes the same interrupt/wait
    handshake, and submits/presents its first 1280x720 diagnostic clear without Vulkan/GPU errors.
    Vulkan is therefore viable now; D3D12 still leads the first real draw, with Vulkan following the
    shared shader/pipeline/resource slice.
  - **OWNED SHADER PAYLOAD CACHE + FIRST LIVE DXBC SHIPPED (2026-07-26, log 661).**
    `Pm4StateShadow` retains exact host-endian `IM_LOAD*` words, and `Pm4ShaderCache` owns and
    collision-checks translation input. The saved boot replay turns 26,526 VS/PS prepare requests
    into 19 unique shaders with 26,507 hits and zero rejections while preserving every established
    deterministic hash. `D3D12Pm4Backend` now embeds the existing ReXGlue raw-microcode analyzer
    and DXBC translator; the official live host translated captured VS `EBFF6B02D047092B` from 24
    dwords to 5,536 bytes of DXBC and continued presenting. The focused suite passes 361 assertions
    in 21 cases. `XenosRecomp` remains useful design reference, but is not required for the first
    native draw.
  - **FIRST CAPTURED-SHADER D3D12 PSO SHIPPED (2026-07-26, final log 664).**
    Active vertex/pixel translator keys now form a deterministic ordered pair key.
    `D3D12Pm4Backend` builds and caches the translator-compatible bindful root signature and a
    graphics PSO for each encountered pair/topology. The official live host translated and paired VS
    `EBFF6B02D047092B` with PS `8391FFB8F4EA66F3`; D3D12 accepted the 5,536/5,520-byte DXBC blobs
    with six root parameters, and the process continued presenting the diagnostic clear. The
    focused suite passes 364 assertions in 21 cases. Next is binding constant/shared-memory
    resources and issuing one known auto-index draw.
  - **FIRST NATIVE FABLE GEOMETRY SHIPPED (2026-07-26, logs 670 and 679).**
    The D3D12 backend now binds translator constant buffers, a compact guest-memory SRV, a private
    host render target, topology-aware PSOs, and issues guarded `DrawInstanced` calls. The first
    resource-free point changed one center pixel (`Fable2_668.log`). The next frame's captured
    three-vertex rectangle consumed 84 bytes of live guest vertex data and changed 145,799 pixels
    over `(322,180)..(1279,483)` before being copied to the presenter. This is real Fable shader
    and vertex output through our pipeline, with no xenos draw assembly. `Fable2_679.log` closes
    the rectangle gap: a translator-compatible geometry stage forwards 16 interpolators, selects
    the longest diagonal, mirrors the correct opposite vertex, and produces exactly 518,400 pixels
    over the complete `(320,180)..(1279,719)` `960x540` rectangle.
  - **FIRST GUARDED MULTI-DRAW FRAME SHIPPED (2026-07-26, final log 681).**
    The backend retains up to 32 eligible draws per swap and submits them sequentially against the
    same private render target, clearing only before the first successful draw. The first live swap
    accumulated 24 draws; the next retained 26 candidates including two vertex-backed rectangles.
    Its final aggregate readback stayed at 518,400 pixels because all currently eligible boot
    shaders overlap in black, but later draws are no longer discarded. Float/bool/loop constants,
    multiple vertex fetches, and textures are next. The sequential fence-per-draw proof should be
    converted to batched recording after per-draw constant/descriptor slices exist.
  - **CONSTANT + INDEX + TEXTURE-TABLE BRIDGE SHIPPED (2026-07-26, final log 685).**
    Normalized draws now own tightly packed live vertex/pixel float values and the fixed 40-dword
    bool/loop payload; deterministic validation hashes include those values. The D3D12 proof
    buffer reserves the full 256-float4 range per stage. DMA index buffers are snapshotted and
    bound with the guest endian mode applied by the translated vertex shader. Bindful texture and
    sampler root tables receive dimension-correct null descriptors, allowing the first real
    textured pair to pass every remaining guard. Sequence 104 now retains 1 VS float vector,
    4 PS float vectors, 6 DMA indices, and 3 pixel textures; sequence 106 also enters the frame.
    The source textures are format `k_8`, linear, endian-none 1280x720 and two 640x360 surfaces.
    The bounded run saw no later swap, so they were retained but not submitted. Next is real
    linear `R8` snapshot/upload and a forced frontend redraw.
  - **FIRST REAL TEXTURED FRAME SHIPPED (2026-07-26, final log 692).**
    Sequence 104 snapshots 1,474,560 bytes from the game's three linear, endian-none `k_8`
    YUV planes, uploads them to R8 typeless D3D12 textures, binds matching UNORM/SNORM SRVs and
    the original fetch/sign constants, then executes the original translated Xenos YUV shader.
    The diagnostic eager-present switch is off by default and exists only because the static
    frontend emits no later swap. Its readback is non-black across all 921,600 pixels
    (`hash EA1BD6D13ECFB239`), and the BMP shows the expected near-white intro-video frame.
    DXBC inspection caught the last blocker: neutral `color_exp_bias` must be `(1,1,1,1)`;
    zero initialization erased the otherwise-correct shader output.
  - **PERSISTENT HOST TEXTURE STORE SHIPPED (2026-07-26, final log 696).**
    The guarded D3D12 path no longer creates and destroys default-heap textures for every
    presentation. A backend-neutral key combines the exact six-dword guest descriptor, visible
    byte length, and payload fingerprint. Cache entries own persistent GPU resources and actual
    D3D12 allocation-byte accounting under a 256 MiB correctness-first LRU budget; upload staging
    remains transient. Live proof pass one allocated/uploaded the three YUV planes using
    1,507,328 bytes, while an immediate second presentation hit all three entries and submitted
    without another upload. The known readback remained exactly
    `EA1BD6D13ECFB239` with no D3D12/device error.
  - **GUEST DIRTY-GENERATION TEXTURE FAST PATH SHIPPED (2026-07-26, final log 699).**
    `NativeGraphicsSystem` now uses physical-memory invalidation callbacks to assign generations to
    watched 4 KiB texture pages. The first guest write expands invalidation to the complete
    coalesced watched resource range and removes that watch, so a streaming update incurs one
    protection fault rather than one per page. Texture capture double-reads the range generation,
    retries once if the producer races the copy, and refuses an incoherent snapshot. A matching
    ready cache entry with an unchanged generation skips the guest copy and content hash entirely;
    changed generations fall back to a fresh coherent snapshot and fingerprint.

    Live coverage hit both correctness branches: log 698 found the three video planes rewritten
    after capture and reported `reusable=0/3`; log 699 refused an actively changing sequence-104
    snapshot, then uploaded/replayed stable sequence 106 and reported `reusable=3/3`. The latter
    replay issued no second upload and had no GPU/D3D12/device-loss error.
  - **FENCE-DEFERRED TRANSIENT RESOURCE RETIREMENT SHIPPED (2026-07-26, final log 701).**
    A backend-neutral, move-only-safe `SubmissionRetirementQueue<T>` now owns upload heaps and
    diagnostic readbacks by D3D12 submission index. It accounts live/peak bytes and releases a
    batch only when the submission tracker reports its fence complete. Host texture entries become
    queue-ready after submission, which is sufficient for ordered work on the same direct queue;
    the upload heap itself remains alive independently. Failed signal/readback-wait paths retain
    their resources until a later completed submission or shutdown instead of dropping a live
    `ComPtr`.

    Live log 701 deferred three YUV upload heaps totaling 1,474,304 bytes to submission 103 and
    exercised fence-completed transient retirement. The cache replay still issued no second upload
    and produced the exact known full-frame hash `EA1BD6D13ECFB239`, with no GPU, D3D12, or
    device-loss error. Diagnostic readback still waits for its own result by design; ordinary
    upload submission no longer forces a whole-queue wait merely for staging lifetime. The
    remaining pre-draw wait protected the single reused command allocator, constant buffers, and
    descriptor tables.
  - **THREE-SLOT IN-FLIGHT DRAW RING SHIPPED (2026-07-26, final log 702).**
    The D3D12 draw path now rotates across three independently owned submission slots. Each slot
    contains its own command allocator/list, shader-visible view and sampler descriptor heaps,
    constant buffer, compact vertex memory, and index memory. Reuse waits only for that slot's
    previous submission rather than draining the whole direct queue. Host texture entries track
    their last referencing submission, and LRU eviction refuses an entry until its fence has
    completed.

    Live log 702 queued submission 4 while two draw submissions were simultaneously in flight.
    It then uploaded and cache-replayed the three YUV planes, preserving the exact full-frame
    `EA1BD6D13ECFB239` hash with no GPU, D3D12, or device-loss error. This is real overlapping
    native draw submission, but not yet one frame batch: each retained draw still closes and
    submits its own command list and performs its own presenter refresh/output copy. Next is to
    record a retained frame into fewer submissions with one final output copy/present, followed by
    broader texture formats/layouts and title-level lifecycle hooks.
  - **IDENTICAL DRAW-RUN COALESCING SHIPPED (2026-07-26, final log 704).**
    Adjacent retained draws now share a submission when their complete captured GPU payloads are
    equal and their sequences are contiguous. The comparison includes shaders, primitive and
    index state/data, vertex fetch/data, constants, texture signs, descriptors, and captured
    texture bytes. Each logical repetition is still emitted as a draw call, but the run uses one
    command list, output copy, and presenter refresh. Contiguous sequences prevent coalescing
    across a rejected or unsupported intervening draw.

    Live log 704 reduced the 24-draw boot burst to one submission batch (`candidates=24`,
    `batches=1`, `repetitions=24`). The three-slot ring still reached two submissions in flight,
    and the YUV upload/cache replay produced the exact known full-frame hash
    `EA1BD6D13ECFB239` without GPU, D3D12, or device-loss errors. The next batching slice is
    heterogeneous: allocate per-draw constant/descriptor/vertex/index slices, record mixed-state
    draws in one frame command list, and copy/present only the final accumulated output.
  - **HETEROGENEOUS FRAME REFRESH/FENCE GROUPING SHIPPED (2026-07-26, final log 706).**
    Mixed retained runs now share one presenter refresh and one final guest-output copy. The
    private accumulation target clears only for the first run; intermediate command lists leave
    guest output untouched. Frames of up to three runs use the existing three independently owned
    resource slots but advance the submission fence only after the final command list. Failure
    paths signal any earlier queued work before returning, preserving allocator, upload, readback,
    and texture-cache lifetime.

    Live log 706 batches a real 26-draw frame into three runs: 24 identical point draws plus two
    distinct vertex-backed draws. It reports one presenter refresh, one output copy, and one fence
    signal, saving two copies and two signals. The accumulated rectangle remains exactly
    518,400 pixels with hash `091707070CB32225`; the later YUV result remains the full 921,600-pixel
    `EA1BD6D13ECFB239` frame, with no GPU, D3D12, or device-loss errors. The remaining command-list
    merge requires offset constant/descriptor/vertex/index slices so switching heterogeneous
    payloads cannot overwrite resources referenced earlier in the same list.
- **P1 — Command processor parity.** Our CP consumes the stream for real: state shadow (register
  file), draw-call assembly (vertex fetch constants, index buffers), resolve/tiling handling —
  still calling into a *borrowed* backend (thin D3D12 layer, may crib xenos pieces knowingly).
  Milestone: logo + frontend render through OUR CP.
- **P2 — Own the backend.** Replace borrowed pieces: our swapchain/present (from `VdSwap`
  semantics), our EDRAM strategy (render-target memory as host RTs, resolve = copy/blit — we know
  the resolve path: `D3D_Resolve_EmitCopyDraw`), our texture upload (fetch constants → host
  textures; ties into the dual-domain plan in
  [NATIVE_PC_MEMORY.md](NATIVE_PC_MEMORY.md)). Milestone: gameplay renders with no
  xenos code in the frame path.
- **P3 — Shader ownership.** Xenos microcode → DXIL/SPIR-V translator of our own (or a trimmed,
  understood fork). Fable's shader population is finite and dumpable via the P0 tracer
  (`IM_LOAD*` payloads) → we can pre-translate/cached-compile the whole set offline, a luxury a
  general emulator doesn't have.
- **P4 — Retire xenos; features.** `rexgpu-native` becomes default; now add the PC wins cleanly:
  arbitrary resolution (no draw_resolution_scale hacks), real MSAA, modern post, uncapped texture
  memory, fixing the RTT/streaming classes of bugs at the source.
- **P5 — Climb the boundary (with Track B decomp).** As Fable's renderer functions get decompiled
  (the 144 inlined-PM4 sites are the map of *where the renderer lives*), replace PM4-emission with
  direct calls into our renderer — dissolving the guest command stream entirely. Endgame: a native
  renderer fed by decompiled game code; the PM4 layer shrinks to nothing.

### Skate3Recomp audit (2026-07-26)

Skate3Recomp is a strong architectural reference, not a drop-in renderer. It hooks title-specific
mesh submission, texture registration, and guest swap functions; publishes an owned immutable
frame scene; mirrors mutable guest mesh/texture content into fingerprinted host caches; and draws
through one small D3D12/Vulkan RHI. It can suppress emulated draw work while its native frame is
active and yield to emulated output for unsupported/menu/editor paths. That is confirmation of our
P2-to-P5 direction: keep fallback as an oracle, climb above PM4 at verified Fable boundaries, and
move large resources into host-owned stores without first rewriting every guest pointer.

The highest-value pieces to reproduce are the per-frame native-output callback, coherent streamed
descriptor reads, content-fingerprint invalidation, immutable scene publication, deferred GPU
retirement, and explicit byte-budgeted caches. The Skate-specific shader/material code is not
portable to Fable, and its root repository currently has no license file, so it remains design
reference rather than copied source.

### Why this is tractable for us (vs "writing a GPU emulator")
- Single title: ~18 PM4 opcodes, one register subset, one finite shader set (offline-translatable).
- The stream's semantics are already labeled in *our* Ghidra project (draws, resolves, blits,
  fences — we know what each packet pattern *means to Fable*, e.g. the swap-blit vs resolve draws).
- The plugin seam gives per-run A/B against xenos, and the P0 tracer gives ground truth before any
  rendering code is written.

## 3. Assets from this session
- Labels: `ghidra_out/labels_d3d_layer.tsv` (65 applied; project saved).
- Decomp dumps: `ghidra_out/d3d_core_decomp.txt` (Swap/ring/device-init),
  `d3d_writers_decomp.txt` (all 88 writers), `d3d_api_callers.txt` (kick + getter call sites),
  `d3d_kick_funcs_uniq.txt`, `d3d_funcs_range.txt` (the carved blob).
- New reusable scripts: `tools/ghidra_label/CarvePdataRange.java` (pdata-carve any blob),
  `FindBranchTo.java` (bl + tail-b + lis/ori address-takes), `ContainingFuncs.java`.
- Next decomp targets: the Swap callers `0x82B6EB9C/0x82B6F408/0x82B6FB00` (Fable's frame-end /
  present layer — the top of the game's renderer), and enumerating the register subset from the
  `SET_CONSTANT` payloads (feeds P0's validator).
