# Session snapshot — 2026-07-26

## Final checkpoint: UI producer isolated and native resolve heap staged

The `Press A` and menu problem is now localized. Menu-era frames retain 45 supported textured
draws and 74 textureless companions, but the final frontend shaders sample an all-zero 560x360
surface at `0x0C6E4000`. PM4 resolve telemetry names that same destination, proving the missing
content belongs to the offscreen render-target graph rather than an unknown font texture.

An opt-in `REXGPU_NATIVE_RESOLVE_HEAP=1` path now groups retained draws by their actual EDRAM
color/depth target base, executes them at resolve boundaries, stores the result as a D3D12 texture
keyed by guest destination address, and rebinds/presents it without a guest-RAM round trip. It is
stable across thousands of resolves but still produces a black selected surface; default mode
therefore stays on the proven swap composition while EDRAM persistence/partial-resolve seeding is
finished.

Default `Fable2_765.log` and
`rexglue-src/out/native_765_resolve_heap_gated/frame_04.png` re-verify upright legal text.
The focused suite passes 27 parser cases / 447 assertions plus the native plugin smoke test.
`rexgpu-native.dll` SHA-256 is
`C0B97405FDF2DE5E20CF72E0CAE2C6BD487293F5EADD7DA9639F3E0CD7A407F8`; runtime remains
`0659BCBABDC41F1EEAE1C469637B92BA2A79327A2712CEBB8E27868594BBD73B`.

## Final checkpoint: first upright native Fable II frontend frame

The owned D3D12 renderer now advances from continuously presented planar logo video into normal
guest-swap-paced frontend rendering. `Fable2_746.log` proves one coherent planar draw per swap
while 27 incomplete companion draws are deferred. `Fable2_747.log` inventories the first
post-video layouts. The guarded host texture path now supports tiled RGBA8, 10:10:10:2, RGBA16F,
BC1, BC2, BC3, and BC4/DXT5A, including guest endian conversion, untile, coherent snapshots,
persistent cache reuse, and base-level use for unpacked mipmapped descriptors.

Captured `RB_BLENDCONTROL0` and color-write state are included in native PSO identity and mapped
to D3D12 blend factors/operations. That turns the former opaque black UI rectangles into properly
alpha-composited glyphs. Frontend geometry uses a top-down Y transform while planar decoder draws
retain their already-proven bottom-up transform. The decisive visual is
`rexglue-src/out/native_753_frontend_oriented/frame_00.png`: upright, readable Microsoft/Lionhead
legal text rendered through our PM4 state, translated shaders, texture pipeline, and D3D12 backend.

The final staged build is covered by all 28 focused PM4/native-plugin Release tests.
`rexgpu-native.dll` SHA-256 is
`5A15EC1E226E851C6EEB58D5A3D2F1596C0AD53CF4E52F4FE2C77A292B877419`; runtime remains
`0659BCBABDC41F1EEAE1C469637B92BA2A79327A2712CEBB8E27868594BBD73B`.
`Fable2_755.log` additionally validates native 1D R32F/RGBA/RGBA16F lookup textures.
Next: implement the depth/HDR postprocess inputs, then replace proof-target
composition with complete guest render-target/resolve routing.

## Final checkpoint: unforced native D3D12 boot-video motion

The producer stall is fixed. PM4 Type-0 packets encode 15-bit register indices, but the native
guest-side-effect callback previously converted them to byte offsets and passed them through the
16-bit MMIO aperture. Fable's 1,024-register constant upload at `0x4000..0x43FF` therefore wrapped
`0x41DC/0x41DD` onto `SCRATCH_UMSK/SCRATCH_ADDR`, disabled scratch writeback, and prevented
`SCRATCH_REG1=1` from publishing the swap mailbox. PM4 reads/writes now use full indices directly;
only actual MMIO accesses are aperture-masked. The focused regression reproduces the exact
1,024-dword upload and verifies no low-register alias.

Clean unforced D3D12 logs `Fable2_733.log` and `Fable2_738.log` cross the boot-motion threshold.
They translate the first Fable VS, build the first native PSO, present a vertex-backed draw, and
continuously submit changing three-plane YUV frames through frame 256 / draw sequence 7017. At 600
host-vblank polls, 291 changed frames had been submitted with only five incoherent snapshots. No
malformed span, rejected PM4 packet, D3D12 submission failure, or device loss occurred. The
forced-wait variable was absent.

The live visual is recognizable and correctly colored. The apparent washout was a stale BMP that
is intentionally written only for streaming frame 1. Timed window captures in
`rexglue-src/out/native_738_boot_sequence/` show the actual Microsoft and Lionhead animations.
Those captures exposed a vertical inversion in the planar upload, now fixed by reversing each
Y/U/V plane's source rows while retaining the D3D NDC conversion.

Planar streaming is now enabled automatically for the normal presenting D3D12 path. Logs
`Fable2_739.log` and final-binary `Fable2_740.log` used no proof variables; log 740 reached frame
128 in eight seconds. `native_739_normal_sequence/frame_12.png` is the clean live visual proof.
Headless mode remains unchanged, diagnostic artifacts remain opt-in, and a focused regression
covers planar layout classification plus bottom-up row mapping. Continue by replacing eager
per-frame presentation with normal swap/pacing and advancing into frontend composition. Visual
runs require `--gpu_plugin native --gpu_backend d3d12`; omitting the backend selects headless
validation.

The first user-visible normal run then exposed presentation overlap rather than PM4 corruption.
`Fable2_741.log` showed an incomplete companion batch following each valid planar frame. The
stream now takes exclusive presentation ownership and suppresses those generic batches. A
50-frame follow-up also isolated post-logo bottom-edge corruption to the host-vblank memory
poller sampling decoder surfaces outside guest draw boundaries. Host-vblank polling is disabled;
completed guest planar draws provide the authoritative ~30 fps frame boundary.
`Fable2_743.log` confirms both policies, and
`rexglue-src/out/native_743_draw_paced_sequence/frame_37.png` through `frame_49.png` are clean and
byte-stable.

All 28 focused PM4/native-plugin Release tests pass. Full CTest also reports nine unrelated
migration/template resource failures and the known unbuilt PPC target. Clean source
Release hashes: native `4DF669BCFA14FE821643C476068E6C8B7FB9DD3DE1EE26053320B4DA663FB264`,
runtime `0659BCBABDC41F1EEAE1C469637B92BA2A79327A2712CEBB8E27868594BBD73B`.

## Newest checkpoint: authentic native video frame, remaining stall isolated

The owned D3D12 renderer now recognizes Fable II's genuine three-plane linear R8 4:2:0 video draw
and presents the decoded Microsoft-logo frame. With `REXGPU_NATIVE_STREAM_TEXTURE_PROOF=1`, it
polls coherent plane snapshots on host vblank, directly fingerprints their contents, suppresses
duplicates, and submits changed frames through the three-slot ring. `Fable2_710.log` performed 600
polls in ten seconds but observed one frame and 600 duplicates with no dirty-generation changes,
proving the static image is produced by unchanged guest surfaces rather than a stale host cache.

The same executable/runtime reaches the Fable II title in about 12 seconds with `rexgpu-xenos`
(`out/xenos_window_711.png`). Native tracing ends normally after sequence 106 at a frame-boundary
`WAIT_REG_MEM` for address `0x1FC83006`; no later command buffer arrives. Guest-visible
`VGT_EVENT_INITIATOR`, EXT event output, and `REG_RMW` parity are now implemented and restored
several seconds of nonzero native audio, but the next video frame remains blocked.

`Fable2_716.log` dispatches PM4 interrupts immediately on the guest-aware host thread rather than
waiting for vblank. It delivered all nine, with 7 µs final latency and no pending mask. The terminal
wait still observed 0 versus reference 1 at encoded address `0x1FC83006` after 6,000 iterations
(ring read 43 / write 55, GPU counter 735). Continue by finding the missing guest/CPU write to
aligned semaphore word `0x1FC83004` and the completion/read-pointer behavior expected to trigger
it, rather than doing more texture or presenter work. Debug/Release pass 434 assertions in 26 cases
plus both plugin smokes. Current source Release hashes are native
`D6C5A66FD4A6D2E84F2092B8300F9E39E5B0482D46D85C6FF2C81E5DB24E9D79` and runtime
`6D1B3A158A6AF223E53E330B9ABE58AB62FC0EC8DDE124337B1EC60F2B455C17`. Ghidra was not used.

The next diagnostic crossed the motion threshold. With disabled-by-default
`REXGPU_NATIVE_FORCE_STALLED_WAIT=1`, `Fable2_717.log` submitted four YUV frames with distinct
content hashes at sequences 106, 133, and 187. This proves changing boot-video frames traverse the
owned renderer, though only after slow forced mailbox releases. `Fable2_718.log` maps the chain to
the `0x1FC83000` D3D swap-ring callback mailbox and known frame-end pointers. Moving native GPU
commands to an `XHostThread` and synchronously dispatching interrupts matches Xenos thread context,
but `Fable2_719.log` still stalls unforced after 7/7 callbacks. Next recover the exact protocol in
`Function_821F6050` and `D3D_GraphicsInterruptCallback` (`0x82BA26B0`), including the
`EVENT_WRITE_SHD` completion block at `0x1FC84000`.

Final Debug/Release result is 435 assertions in 26 cases plus both plugin smokes. Current source
Release hashes are native `C91F862C47EBA940E81632257AB7C82100904936FE6CEA60DA65335852E58294`
and runtime `068496CFBEAF71B40DD081493E5ED701AD89BCF7032F0266EDF8C020CC907D50`.

## Resume here

The own-renderer track now reaches live D3D12 and Vulkan host submissions through an independent
plugin/backend seam. `rexgpu-native` owns its MMIO/ring worker, guest vblank/interrupt dispatch,
PM4 memory/control side effects, presenter, and per-API device/queue/command recording. It does not
compile or call the xenos command processor or draw assembly. `Pm4Stream` sends normalized
draw/resolve/present IR through the API-neutral `Pm4Backend` interface, while `Pm4SideEffects`
performs the guest-visible semaphore, scratch-writeback, interrupt, and memory semantics needed to
keep Fable's ring moving. D3D12 has now advanced beyond its diagnostic clear: live log
`Fable2_681.log` retains, accumulates, and presents multiple guarded captured Fable draws using
guest vertex data through the native pipeline. Vulkan remains at its proven diagnostic-clear
milestone (`Fable2_659.log`).

Next implementation target:

1. Capture live float/bool/loop constant values alongside their already-normalized usage masks.
2. Generalize the compact guest-memory upload from one vertex fetch to multiple fetch ranges.
3. Add the first texture upload and sampler descriptor path.
4. Batch accumulated draws using per-draw constants/descriptors instead of a fence wait per draw.
5. Mirror the shared
   shader/pipeline/resource slice in Vulkan.
6. Capture a representative loaded-world/combat/effects suite.

Do not restart the PM4 framing/state work; it is complete and validated. Preserve all unrelated
dirty worktree changes.

## D3D12 foundation and Vulkan viability

- `NativeGraphicsSystem` now receives an injected `Pm4Backend`; common PM4 headers contain no host
  API handles.
- `Pm4Backend` reports API-neutral identity, capabilities, and deterministic diagnostics.
- `D3D12Pm4Backend` owns ReXGlue's D3D12 provider, device, direct queue, command allocator/list,
  descriptor heap, submission fence, and presenter.
- ReXApp exposes `--gpu_backend=any|d3d12|vulkan|validation` and forwards it through the existing
  plugin ABI.
- Nightly log `Fable2_656.log` proves the official host selected `d3d12-foundation`, created the
  device/queue/recording resources and presenter, executed the guest scratch/interrupt handshake,
  reached the first live `XE_SWAP`, and submitted/presented a 1280x720 diagnostic clear. No PM4
  packet was rejected and no primary span was malformed in that run.
- A separate Windows build configured with `REXGLUE_USE_D3D12=OFF` and
  `REXGLUE_USE_VULKAN=ON` successfully built `rexruntime` and `rexgpu-native`. The first attempt
  exposed a common-presenter `CreateDXGIFactory1` link dependency; DXGI is now linked as a Windows
  platform dependency instead of only under the D3D12 option.
- `VulkanPm4Backend` now owns a graphics-family command pool/buffer and submission tracker. It
  performs the presenter's required first-use/internal-layout transitions, clears the mailbox image,
  returns it to shader-read layout, and submits on the externally synchronized graphics queue.
- Nightly log `Fable2_659.log` proves the official host selected `vulkan-foundation`, created an
  AMD Radeon RX 9060 XT Vulkan device and presenter, reported `command_recording=true` and
  `clear_submission=true`, completed the PM4 interrupt/`WAIT_REG_MEM` handshake, and submitted and
  presented the first 1280x720 Vulkan diagnostic clear. There were no Vulkan/GPU submission errors.

Conclusion: Vulkan is viable now, not premature. Device, presentation, command recording,
synchronization, and a real live submission are proven. D3D12 remains the shortest path to the
first actual draw; Vulkan should consume the same normalized shader/pipeline/resource layer at each
backend milestone rather than fork any PM4 semantics.

Current staged Release hashes:

```text
rexruntime.dll    B04B0E823892DF0FFEF165D0E5473AC0CB152C54568320876F57812509F2963B
rexgpu-native.dll C9CAF88EE658E9D443EEE6BBE03DAAFB57B6E3992CFD713A5529A5A700C598FF
```

## Constant-usage and translator-key milestone

The independent raw-microcode scanner now records:

- 256 float-constant bits plus count and dynamic-addressing state.
- 256 bool-constant bits.
- 32 loop-constant bits.
- Vertex and texture fetch masks.
- A versioned 64-bit translator key based on stage, bytecode identity, and normalized usage.

Live constant/fetch values are excluded from the translator key because they are runtime inputs,
not reasons to compile another host shader. The key is attached to shader inventory and active draw
records and participates in deterministic draw hashing.

Focused result: **361 assertions in 21 test cases**, all passing.

Saved replay remained byte-identical across repeated runs:

```text
packet_hash=03F8E1006C0E7534
draw_hash=8EECEFDE3D074DC3
resolve_hash=FF3915A416DED5CC
snapshot_hash=2559219BD700E6D7
register_hash=A5C658D6DCD87A30
```

## Native validation-plugin milestone

- `rexgpu-native.dll` is an installable/exported CMake target and is staged in the nightly output.
- DLL smokes verify load, ABI 1 exports, factory creation, headless setup, shutdown, and unload.
  The Vulkan-only smoke additionally creates the real Vulkan provider/device/presenter.
- The rebuilt nightly was launched with `--gpu_plugin=native`; it remained alive for the 15-second
  smoke window, and module inspection confirmed the official host loaded the staged
  `rexgpu-native.dll`. No Fable process was left running.
- `Fable2Recomp/CMakeLists.txt` now restages the worktree Release plugin after every nightly link.
- `Pm4Backend` is the backend-neutral draw/resolve/present contract; `Pm4ValidationBackend`
  implements deterministic aggregate and per-frame event hashing.
- `Pm4Stream` is shared by the native ring worker and `pm4_replay`.
- The saved boot capture produces **481 deterministic frame records**. First frame:
  `43C005B0672282ED`; last frame: `5961A01968E3E875`.
- Aggregate replay hashes remain unchanged from the P1.4 baseline above.
- Focused result is now **364 assertions in 21 cases**, plus the D3D12 and Vulkan native-plugin
  smokes.
- Final staged Release DLL SHA-256:
  `C9CAF88EE658E9D443EEE6BBE03DAAFB57B6E3992CFD713A5529A5A700C598FF`.

Current limitation: D3D12 submits only a guarded proof subset (auto-index point/line/triangle draws,
zero or one vertex fetch, no textures, and no referenced float/bool/loop constants). Unsupported
draws remain validation-only, and each swap presents only the selected proof draw on a fresh target.
Vulkan still presents its diagnostic clear.

## Owned shader payload/cache and first DXBC translation

- `Pm4StateShadow` now retains the exact canonical host-endian microcode words from every
  `IM_LOAD*`, rather than discarding the payload after usage scanning.
- Shader deduplication verifies stage, hash, dword count, and the full microcode payload.
  `Pm4ShaderCache` owns translation input memory and rejects translator-key collisions rather than
  silently aliasing them.
- Before every normalized draw, `Pm4Stream` resolves the active VS and PS records and prepares them
  through the API-neutral backend contract.
- The saved boot capture issues 26,526 stage prepare requests, which collapse to exactly 19 unique
  owned shaders (7 VS + 12 PS), 26,507 cache hits, and zero rejections.
- The captured replay hashes remain unchanged:
  packet `03F8E1006C0E7534`, draw `8EECEFDE3D074DC3`, resolve `FF3915A416DED5CC`,
  snapshot `2559219BD700E6D7`, register `A5C658D6DCD87A30`.
- `D3D12Pm4Backend` now embeds ReXGlue's existing raw-microcode analyzer and DXBC translator.
  Translation is performed once per cache key while draw submission remains disabled.
- Live nightly log `Fable2_661.log` proves the official D3D12 backend translated captured Fable VS
  `EBFF6B02D047092B` (24 input dwords) into a 5,536-byte DXBC blob, then submitted/presented the
  diagnostic clear and stayed alive for the full 30-second smoke. No Xenos-to-DXBC translation
  failure was logged.
- Source Release proof hashes are `rexgpu-native.dll`
  `913A92806295EDD67D0DF7B296EE9E59A28F81B0D3243B9A435E8442A29924F7` and matched
  `rexruntime.dll` `D73C618E8D7553D29B8946EA3F86EB6AC8E33B5003818591FEE6D04849BCCBD6`.
  The normal nightly staging was restored to the earlier hashes recorded above after the smoke.

This makes `XenosRecomp` unnecessary for the first draw. It remains useful as an offline-cache and
PSO-layout reference, but ReXGlue's existing translators already accept the raw `IM_LOAD*` payload
we capture and have now translated one live Fable shader successfully.

## First captured-shader D3D12 PSO

- Active vertex/pixel translator keys now form a deterministic ordered shader-pair key. The key is
  stable for identical pairs and distinguishes VS/PS order.
- `D3D12Pm4Backend` caches a root signature and graphics PSO for every live shader pair/topology.
- The root signature implements the translator's bindful contract: fetch, per-stage float, system,
  and bool/loop root CBVs; the shared-memory SRV/UAV table; and optional per-stage
  texture/sampler tables.
- The first live attempt exposed one fixed-function descriptor error: RTV formats beyond
  `NumRenderTargets` must remain `DXGI_FORMAT_UNKNOWN`. Restricting initialization to the four
  declared targets fixed D3D12 `E_INVALIDARG`.
- Nightly log `Fable2_663.log` proves D3D12 accepted captured VS `EBFF6B02D047092B` and PS
  `8391FFB8F4EA66F3` as a real graphics pipeline. The root signature has six parameters; the DXBC
  blobs are 5,536 and 5,520 bytes. The live host continued through the PM4 handshake, submitted the
  diagnostic clear, and stayed alive for the bounded smoke.
- The cache uses the complete ordered `(VS translator key, PS translator key, topology type)` for
  equality; the 64-bit digest is only the hash/diagnostic value, so a digest collision cannot alias
  a PSO.
  Final collision-hardened live confirmation is `Fable2_664.log`, with source Release hashes
  `rexgpu-native.dll` `354155CC9D112594C808571D9DD6266CF9C6FCC5E53B21E94EF8265F53A12760`
  and `rexruntime.dll` `EE2FF2E18E8323DCAA2073EED53975E4DDA190CB706EB4656AD0860F6F3E0F71`.
- The focused suite passes **364 assertions in 21 cases**. Release native-plugin load/factory
  smoke also passes. Normal nightly DLLs were restored after both temporary live runs.

That log was the PSO-creation proof. The following live milestone closes the resource-binding and
first-draw boundary.

## First captured Fable pixels and vertex-backed geometry

- `D3D12Pm4Backend` now owns a 4 KiB translated-constant upload buffer, null/shared-memory
  descriptors, a topology-aware PSO cache, a private `R10G10B10A2` render target, copy-to-presenter
  barriers, and guarded `DrawInstanced` submission.
- Log `Fable2_668.log` proves the resource-free point draw from VS `EBFF6B02D047092B` and PS
  `8391FFB8F4EA66F3` changed exactly one pixel at `(639,359)` to black on the 1280x720 target.
- The backend now receives the PM4 stream's guest-memory reader, snapshots one referenced vertex
  fetch, restores raw guest byte order, remaps its fetch constant to a compact upload buffer, and
  binds that buffer as the translator's shared-memory SRV.
- Final live proof `Fable2_670.log` submits rectangle-list sequence 25 using VS
  `DCB0A10386B81456`, PS `8391FFB8F4EA66F3`, and 84 bytes of guest vertex data from
  `0x1F4C703C`. GPU readback reports **145,799 changed pixels**, with bounds
  `(322,180)..(1279,483)`, followed by successful presentation. No D3D12 rejection, device removal,
  or GPU submission failure was logged.
- The changed-pixel count is almost exactly half of the `958x304` bounding box. That was expected
  from the temporary `RectangleList -> TriangleList` mapping and proved the three captured
  vertices were correct. The following rectangle-expansion milestone supersedes this limitation.
- Source Release SHA-256 for this proof:
  `rexgpu-native.dll` `493EDA3DD06E79F9E0A12E93299FDCB3F39405DC5BCCC1C2581BB83F97DC53E5`;
  `rexruntime.dll` `DE31C8A22A93C7E5249F435B36F525ECC3C79C1478F6B9FA10FD7E2127D77778`.
  The normal nightly DLLs were restored afterward.

## Complete Xenos rectangle expansion

- `D3D12Pm4Backend` now keys rectangle-list PSOs separately and binds a geometry shader that
  forwards the translator's `TEXCOORD0..15` registers plus register-16 `SV_Position`.
- D3D12 validation identified the original position-register and missing-interpolator linkage
  errors; the backend now reports stored info-queue details on any future PSO rejection.
- The captured vertices showed why fixed `-v0 + v1 + v2` synthesis was wrong for this draw. The
  geometry stage selects the longest diagonal, emits the corresponding strip order, and mirrors
  that order's opposite vertex and interpolators.
- Final live proof `Fable2_679.log` changes exactly **518,400 pixels**, filling the complete
  `(320,180)..(1279,719)` `960x540` rectangle. The earlier direct triangle changed 145,799 pixels.
  No PSO validation, D3D12 submission, or device-loss error is present.
- Source Release SHA-256:
  `rexgpu-native.dll` `B0EB25CBE7907CFD6FA295542AE07E5F2A988AE10FC910C52E9C90AA8FCC2066`;
  `rexruntime.dll` `338E8895860BE5D29A3E7DF61D456FFC2EECD9EB89E0F8272D86336C905998AF`.
  The normal nightly pair was restored afterward.
- Validation remains **364 assertions in 21 cases**, with Debug/Release D3D12 plugin smokes and
  the Vulkan-only RelWithDebInfo smoke passing.

## First guarded multi-draw frame

- A bounded 32-entry per-frame list replaces the single preferred pending draw. Extra candidates
  remain validation-only rather than growing memory without limit.
- The correctness-first path submits retained draws sequentially against one private target, clears
  only before the first successful draw, and waits between submissions so the existing shared
  descriptor and upload resources can be reused safely.
- `Fable2_681.log` submits **24 draws** in the first accumulated swap. The next guarded frame
  retains 26 candidates, including both vertex-backed rectangles and 24 bootstrap points.
- The final accumulated readback is **518,400 pixels** over `(320,180)..(1279,719)`, identical to
  the complete rectangle. This is expected: the eligible boot shaders currently overlap and write
  the same black value. No later draw is discarded, but broader constants/resources are needed for
  more distinct geometry and color.
- Source Release SHA-256:
  `rexgpu-native.dll` `6058FBDCFB47122DAC1FE968B3CDA1E2004F5B1084CA86B9204AAA90308296FA`;
  `rexruntime.dll` `A2C017453E262C79A75F01F8177572567A29AB4FAE1855867ACFC998EC70E38E`.
  The normal nightly pair was restored afterward.
- Validation remains **364 assertions in 21 cases**, and Debug/Release native-plugin smokes pass.

The next boundary is supplying live float/bool/loop constants, multiple vertex fetches, and the
first texture/sampler path so a recognizable frontend frame can enter the submitted set. Once
per-draw resource slices exist, replace the sequential fence-per-draw proof with batched command
recording.

## Constant, DMA-index, and bindful texture-table bridge

- `Pm4DrawEvent` now snapshots used vertex/pixel float constants in the exact compact order
  expected by the DXBC translator. It also captures the shared 8 packed boolean and 32 loop
  registers, tracks whether every referenced register is known, and includes the values in the
  deterministic draw hash.
- The D3D12 proof constant resource now reserves the full 256 float4 registers for each stage and
  uploads float, bool, and loop payloads before each fenced draw.
- DMA index draws now snapshot their guest index bytes, create an upload index buffer, select
  `R16_UINT` or `R32_UINT`, normalize the unusual 16-bit endian modes like the existing primitive
  processor, and pass the guest endian mode to the translated vertex shader before
  `DrawIndexedInstanced`.
- Bindful PS/VS texture and sampler root tables now receive shader-visible, dimension-correct null
  descriptors. This is intentionally a binding/command proof, not real texture content yet.
- `Fable2_685.log` retains the formerly rejected sequence 104 with 1 VS float vector, 4 PS float
  vectors, 6 DMA indices, and 3 fully known pixel textures. Sequence 106 also enters the guarded
  frame. The source descriptors are:
  - slot 0: linear, endian-none `k_8`, 1280x720, base `1F3B6000`;
  - slot 1: linear, endian-none `k_8`, 640x360, base `1F372000`;
  - slot 2: linear, endian-none `k_8`, 640x360, base `1F32E000`.
- The static frontend emitted no later swap in the bounded run, so this newly expanded frame was
  retained but not submitted. Trigger a redraw when validating the real texture upload.
- Focused validation is **382 assertions in 22 cases**. Debug and Release native plugin smokes
  pass. Source Release SHA-256:
  `rexgpu-native.dll` `A56EDD3CE43E87E6C89DD2E12F0A2544EE43693F0C1F05B3F07B74AE28B42031`;
  `rexruntime.dll` `A8057B423587C4D035FC7C2A9DA59646E416286E210E4365B56B7B8972842EB0`.
  The normal nightly DLLs were restored afterward.

## First real linear-R8 textured output

- `Pm4TextureFetchDescriptor` retains the original six fetch-constant dwords. Draw snapshots also
  carry the 192-dword fetch buffer and packed post-swizzle texture signs expected by DXBC.
- The guarded D3D12 subset accepts fully known 2D `k_8`, linear, endian-none, non-stacked,
  single-mip surfaces. It snapshots pitch-aware guest bytes, creates R8 typeless default textures
  plus placed upload footprints, copies visible rows, transitions them for shader reads, and binds
  matching R8 UNORM/SNORM array SRVs.
- `Fable2_686.log` proves all three live snapshots: slot 0 is 1280x720/pitch 1280; slots 1 and 2
  are 640x360/pitch 768; total payload is 1,474,560 bytes.
- Because the static frontend emits no later swap, `REXGPU_NATIVE_EAGER_TEXTURE_PROOF=1` enables a
  disabled-by-default diagnostic submission and BMP dump. `Fable2_692.log` uploads and submits
  sequence 104 with no D3D12/device errors.
- DXBC/ucode dumps prove this pair is a three-plane YUV conversion. The initial black result was
  not missing texture data: `SystemConstants::color_exp_bias` was zero. Setting its neutral value
  to `(1,1,1,1)` produces a full 1280x720 non-black readback
  (`first=FE7F87E6`, `hash=EA1BD6D13ECFB239`) and the expected near-white intro-video frame in
  `rexgpu_native_texture_proof.bmp`.
- Focused validation is **383 assertions in 22 cases**. Final source Release SHA-256:
  `rexgpu-native.dll` `4A786011F16955728D29EDAD9640457F221DC46FE097466D01C445E99ED47FAA`;
  `rexruntime.dll` `AE4D560983F22BEC92D4C18C3AD33CA3D3F0010FBD22ED51126AFDE47682FB00`.
  The official nightly DLLs were restored afterward.

Next: retain/substitute subsequent video or frontend draws so the normal swap path presents them,
then broaden guarded texture formats/layouts and capture the first recognizable non-video scene.

## Upstream review and tool allocation

- Local ReXGlue is at `29eaa8a`; upstream `development` is 12 commits ahead at `3eb9b511`.
  Those 12 commits touch audio, codegen, hooks, DLL dispatch, UI/XAM, input, build/versioning, and
  tests, but no graphics/backend files. The useful items for Fable are the XMA loop/flush fixes and,
  if manual switch tables are added later, the block-discovery fix. Do not merge the range into the
  dirty renderer tree during the current milestone; review and integrate it as a separate update.
- UnleashedRecomp validates the architecture we were already converging on: hash Xbox shader
  binaries, pretranslate a finite shader population to DXIL/SPIR-V, normalize and hash pipeline
  state, and precompile known PSOs. Its `XenosRecomp` and `plume` dependencies are MIT-licensed.
- `XenosRecomp` is the most useful concrete donor, but not drop-in. It expects Xbox shader
  containers with reflection/vertex-declaration metadata, while our live `IM_LOAD*` inventory is raw
  microcode. It also documents incomplete integer constants, dynamic register indexing, mini vertex
  fetch, memory export, point size, and multiple Sonic-specific vertex/semantic assumptions. Start
  with one Fable shader-container-to-captured-microcode match and trim those assumptions behind our
  existing translator key.
- Do not replace the current ReXGlue D3D12/Vulkan backends with `plume` now. Its API shape is a
  useful reference for shared resources, barriers, and PSO descriptions, but switching RHIs would
  discard already-proven presenter integration and add another unstable dependency.
- Ghidra is not needed for the next renderer milestones. The other agent may use it. Bring it back
  only for a concrete Fable-specific ambiguity that capture/replay cannot answer (for example an
  unknown vertex-declaration mapping, register meaning, resolve quirk, or an eventual direct game
  renderer hook).

Constant oracle validation:

- Log 638: 250,910 stage checks, zero mismatches.
- Final staged key-bearing build log 641: 135,816 constant checks, zero mismatches.
- Log 641 also has zero vertex/texture-mask mismatches, shader failures, unclassified/invalid
  packets, out-of-range writes, swap mismatches, or register-parity mismatches.

## Texture-fetch milestone

Primary implementation:

- `rexglue-src/src/graphics/native/pm4_state.h`
- `rexglue-src/src/graphics/native/pm4_state.cpp`
- `rexglue-src/src/graphics/native/pm4_replay.cpp`
- `rexglue-src/src/graphics/command_processor.cpp`
- `rexglue-src/tests/unit/graphics/pm4_parser_test.cpp`
- `rexglue-src/tests/unit/graphics/pm4_replay_main.cpp`

The shader scanner retains stage-specific 32-bit texture masks. Each referenced slot produces a
descriptor even when its register state is incomplete; descriptors contain a six-bit dword
knownness mask plus decoded addresses, dimensions, format/endian/sign/swizzle, filters, LOD, and
mip state. The replay draw hash covers every normalized field, and inventory output reports texture
masks.

Saved-corpus replay:

```text
records(packet=175764 memory=1487) packets=116269 draws=13263 rt_transitions=1962
resolves=497 complete=497 color=496 depth=1 swaps=481 vfetch_draws=1479 vfetches=1479
shaders=3441 unique_vs=7 unique_ps=12 tfetch_draws=967 tfetches=2857
tfetch_complete=2857 tfetch_invalid=480 shader_fail=0 regw=1257029 unique=4000
external=512 state_sem=23187 control=12257 unclassified=0 invalid=0 unknown=0 malformed=0
packet_hash=03F8E1006C0E7534
draw_hash=0EE9EC8C218CA3EA
resolve_hash=FF3915A416DED5CC
snapshot_hash=2559219BD700E6D7
register_hash=A5C658D6DCD87A30
```

Repeated replay output was byte-identical. Focused tests pass **278 assertions in 17 test cases**.

Bounded live validation: `Fable2Recomp/out/build/win-amd64-nightly/logs/Fable2_637.log`.

```text
packets=3755424 draws=263948 rt_transitions=35663
resolves(total=11653,complete=11653,color=10894,depth=759) swaps=1240
tfetch(draws=69582,descriptors=181136,complete=181136,invalid=5727)
shaders=123792 unique_shader(vs=10,ps=18) shader_fail=0
unclassified=0 invalid=0 oor=0 swap_mismatches=0
vfetch_mask(checks=252295,mismatches=0)
tfetch_mask(checks=516243,mismatches=0)
parity known=2467 mismatches=0
```

The `tfetch_invalid` count is diagnostic, not an IR loss: active shaders may retain references while
the corresponding fetch group currently contains a vertex/invalid type (notably non-color/copy
draws). Those descriptors remain explicit and fully known; stage-mask oracle parity is zero.

## What landed

Primary implementation:

- `rexglue-src/src/graphics/native/pm4_state.h`
- `rexglue-src/src/graphics/native/pm4_state.cpp`
- `rexglue-src/src/graphics/native/pm4_replay.h`
- `rexglue-src/src/graphics/native/pm4_replay.cpp`
- `rexglue-src/src/graphics/command_processor.cpp`
- `rexglue-src/tests/unit/graphics/pm4_parser_test.cpp`

Documentation:

- `docs/OWN_RENDERER.md`
- `docs/OWN_RENDERER_TASKS.md`

New draw state includes explicit knownness plus:

- EDRAM surface pitch, MSAA mode, and EDRAM mode.
- Four color targets: tile base, format, signed exponent bias, and component write mask.
- Depth target: tile base, format, and depth/stencil control.
- Seven-bit resource transition mask: color targets 0-3, depth, pitch/MSAA, and EDRAM mode.

`Pm4ResolveEvent` includes:

- Surface pitch and MSAA.
- Color/depth source selection, source tile base and format.
- Sample selection, copy command, and color/depth clear enables.
- Destination base, pitch, height, array/slice, format, number format, endian, exponent bias, and
  channel swap.
- Explicit knownness for every register group.

## Deterministic replay baseline

Capture:

`Fable2Recomp/out/build/win-amd64-nightly/pm4_captures/pm4_boot_2026-07-25.f2pm4`

Latest replay:

```text
records(packet=175764 memory=1487) packets=116269 draws=13263 rt_transitions=1962
resolves=497 complete=497 color=496 depth=1 swaps=481 vfetch_draws=1479 vfetches=1479
shaders=3441 unique_vs=7 unique_ps=12 tfetch_draws=967 tfetches=2857
tfetch_complete=2857 tfetch_invalid=480 shader_fail=0 regw=1257029 unique=4000 external=512
state_sem=23187 control=12257 unclassified=0 invalid=0 unknown=0 malformed=0
packet_hash=03F8E1006C0E7534
draw_hash=8EECEFDE3D074DC3
resolve_hash=FF3915A416DED5CC
snapshot_hash=2559219BD700E6D7
register_hash=A5C658D6DCD87A30
```

Focused test result: **299 assertions in 18 test cases**, all passing.

## Live validation

Authoritative final-build live run:
`Fable2Recomp/out/build/win-amd64-nightly/logs/Fable2_641.log`.

Last cumulative sample:

```text
packets=1022576
draws=69741
rt_transitions=10935
resolves(total=3666,complete=3666,color=3488,depth=178)
swaps=657
tfetch(draws=15467,descriptors=35825,complete=35825,invalid=2410)
shader_fail=0
unclassified=0 invalid=0 oor=0
swap_mismatches=0
vfetch_mask(checks=66075,mismatches=0)
tfetch_mask(checks=135816,mismatches=0)
constant_usage(checks=135816,mismatches=0)
parity known=2473 mismatches=0
```

No Fable, rexglue, Ghidra, or Java process was left running.

## Ghidra result

Reports:

- `ghidra_out/renderer_resolve_refs_20260726.log`
- `ghidra_out/renderer_resolve_signatures_20260726.log`
- `ghidra_out/renderer_copy_emitters_20260726.log`

The known cold `D3D_Resolve_EmitCopyDraw @ 0x82206F30` has zero direct references because the D3D
static library was LTCG-inlined into game code. Signature scanning found generic load/draw packet
constants in export, swap-blit, and frontbuffer-blit emitters too, so those constants are not a
safe resolve classifier. The executed-state boundary is authoritative:

`RB_MODECONTROL.edram_mode == kCopy`, decoded with the active `RB_COPY_*` registers.

Do not spend the next session searching for direct callers of `0x82206F30`.

## Build and staging state

Release source and staged plugin matched at shutdown:

```text
rexglue-src/out/win-amd64/Release/rexgpu-xenos.dll
Fable2Recomp/out/build/win-amd64-nightly/rexgpu-xenos.dll
SHA256 EB973F72B5D6DDC3FAAED69D58E7D77C18537EDB540797307015FB7A05AE8422
```

The focused Debug test/replay targets and Release graphics plugin build succeeded. The existing
compiler warnings are pre-existing and were not treated as new failures.

## Useful restart commands

Run from `rexglue-src` in PowerShell.

Focused build:

```powershell
& cmd.exe /d /s /c 'set VSINSTALLDIR=&& set VCINSTALLDIR=&& set VCToolsInstallDir=&& set INCLUDE=&& set LIB=&& set LIBPATH=&& call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 && cmake --build out\build\win-amd64-tests --config Debug --target pm4_parser_tests pm4_replay'
```

Tests and deterministic replay:

```powershell
& '.\out\win-amd64\Debug\pm4_parser_tests.exe'
& '.\out\win-amd64\Debug\pm4_replay.exe' '..\Fable2Recomp\out\build\win-amd64-nightly\pm4_captures\pm4_boot_2026-07-25.f2pm4'
```

Release plugin:

```powershell
& cmd.exe /d /s /c 'set VSINSTALLDIR=&& set VCINSTALLDIR=&& set VCToolsInstallDir=&& set INCLUDE=&& set LIB=&& set LIBPATH=&& call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 && cmake --build out\build\win-amd64 --config Release --target rexgpu-xenos'
```

## Worktree warning

`rexglue-src` was already heavily dirty before this renderer work. The native PM4 directory and
focused graphics test directory are currently untracked as a whole, while many unrelated tracked
files also contain user/other-agent edits. Do not reset, clean, stage everything, or commit broad
paths. Inspect exact hunks and preserve unrelated changes.
