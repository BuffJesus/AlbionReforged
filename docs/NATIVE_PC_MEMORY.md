# Native PC memory architecture

## Decision

Fable II needs two memory domains during the recomp-to-native transition:

1. **Guest compatibility arena** — the 32-bit Xbox address space used by still-translated PPC code,
   ABI-visible structures, guest pointers, and kernel/MMIO contracts.
2. **Host native resource domain** — ordinary 64-bit PC allocations for renderer resources, decoded
   textures and meshes, audio buffers, shader/PSO caches, streaming data, mod state, and every
   decompiled subsystem that no longer exposes raw guest pointers.

Trying to turn every guest allocation into a host pointer at once is not safe. Recompiled game code
still stores, compares, endian-swaps, serializes, and performs arithmetic on 32-bit pointers. A host
pointer cannot be substituted without changing every producer and consumer of that value.

The practical path to a true native port is to shrink the compatibility arena, not to enlarge it
forever.

## The bridge

Cross-domain objects use a checked handle table:

```text
translated Fable code          owned PC subsystem
---------------------          ------------------
guest address / asset ID  -->  lookup key + generation
                               native 64-bit object
                               native allocation / GPU resource
                               lifetime + byte accounting
```

Rules:

- Never expose a raw host pointer to untranslated code.
- Prefer an existing stable game identity — texture object, asset GUID, mesh context, sound-bank
  entry — as the lookup key.
- If a new 32-bit handle is unavoidable, use an explicitly tagged, generation-checked handle table.
  Do not claim an arbitrary guest virtual-address band without proving it cannot collide.
- Snapshot guest descriptors and payloads coherently, fingerprint mutable content, and retain the
  host copy until invalidation or budgeted eviction.
- Keep host budgets in bytes and use PC RAM/VRAM policy. The 512 MB guest physical count must not
  cap host caches.
- Resource destruction is deferred to the owning GPU/audio submission timeline, not tied blindly
  to guest heap reuse.

## What is already native

The live `rexgpu-native` textured path already follows this model:

- the guest contributes a six-dword texture descriptor and source bytes;
- our command processor snapshots them;
- host vectors, D3D12 upload buffers, textures, views, descriptors, PSOs, and readbacks live outside
  the 512 MB guest physical map;
- the original guest allocation remains only the compatibility-side source of truth.

As of `Fable2_696.log`, the default-heap textures are persistent rather than one-shot. Their key
contains the raw six-dword descriptor, exact visible byte length, and content fingerprint. The
first three-plane YUV upload consumes 1,507,328 actual D3D12 allocation bytes; an immediate replay
hits all three entries and performs no repeat upload. The store exposes live/peak bytes,
hits/misses, and evictions and uses a 256 MiB host-side LRU budget. This budget is renderer policy,
not guest physical memory. Current eviction is safe because the correctness-first draw path
retires prior submissions before reuse; deferred fence retirement replaces that global wait later.

The compatibility boundary now has a dirty-generation fast path. The native graphics system arms
physical-memory write tracking for coalesced texture ranges and records per-4 KiB generations.
Texture capture brackets its guest copy with generation reads and retries once if streaming races
the snapshot. When the descriptor, range generation, and ready host entry all match, the renderer
reuses the stored content key without copying or hashing guest bytes. A first write invalidates the
whole watched resource range, after which the next draw takes a fresh coherent snapshot.

The live streamed-video result is intentionally timing-dependent and therefore useful:
`Fable2_698.log` found all three YUV planes changed (`reusable=0/3`), while `Fable2_699.log` found a
later stable capture eligible (`reusable=3/3`). The former proves an address-only cache would have
served stale video; the latter proves the unchanged eligibility path. Fable
registration/rebind/destruction hooks remain valuable for stronger title-level identity and
lifecycle ownership, not for basic mutation correctness.

Transient upload and readback ownership is now submission-indexed as well. A backend-neutral
retirement queue holds move-only host resources and byte accounting until the D3D12 fence reaches
their submission. `Fable2_701.log` places the three YUV upload heaps (1,474,304 bytes) on submission
103 and retains the exact known output. Upload staging no longer requires a whole-queue wait merely
to keep a local resource alive. Diagnostic readback still waits for the data it explicitly
requests, and the current single command allocator/dynamic-buffer set still requires a wait before
reuse.

That final global reuse constraint is now removed as of `Fable2_702.log`. Three independently
fenced draw slots own allocator/list, constant, descriptor, vertex, and index resources. The live
run reached two overlapping draw submissions while preserving the exact known textured output.
Host texture eviction also checks each entry's last referencing submission, so reclaiming PC VRAM
cannot destroy a texture still visible to an in-flight command list. As of `Fable2_704.log`,
adjacent draws with identical complete captured payloads are coalesced into one submission and one
output copy/presenter refresh; the first 24-draw burst becomes one batch. `Fable2_706.log` advances
mixed-state frames too: three independently resourced command lists representing 26 logical draws
share one fence signal, presenter refresh, and final output copy. The lists remain separate until
one command list gains offset per-draw constant, descriptor, vertex, and index slices. That
remaining combination is an efficiency/architecture step rather than a resource-lifetime
prerequisite.

`include/rex/graphics/host_texture_heap.h` is an earlier Phase-1 prototype. It proves host-backed
allocation and accounting, but its `0xD8000000` token band is explicitly provisional and it is not
wired into the live fetch path. Do not ship that band as if it were a safe general heap. The live
renderer cache should first key host resources by observed texture identity plus descriptor/content
fingerprint; tokenized allocation belongs at a verified game allocator boundary later.

## Migration ladder

### M0 — Measure compatibility memory

Keep the guest memory map correct and add useful accounting: committed/reserved bytes by heap,
allocation failures, and high-water marks. Allocation *probes* must be distinguished from final
failure.

### M1 — Move heavyweight copies to host ownership

Renderer uploads, decoded texture/mip chains, mesh buffers, shader caches, audio decode/mix queues,
video frames, and mod assets live in native allocations. Guest memory keeps only descriptors,
mailboxes, and compatibility payloads still read by translated code.

This is the current stage.

### M2 — Title-specific resource bridges

Hook Fable resource registration, streaming rebind, and destruction boundaries. Build host stores:

- `TextureObject/asset ID -> HostTexture`
- `Mesh/resource ID -> HostMesh`
- `Sound bank/voice ID -> HostAudioResource`
- `Movie/frame ID -> HostVideoFrame`

Each store owns native memory, generation/lifetime, fingerprints, asynchronous upload, and eviction.
The renderer and audio engine consume these stores directly rather than repeatedly decoding guest
memory.

### M3 — Replace subsystem allocators

After all users of a Fable subsystem are understood, decompile its allocator-facing API and return
native handles or native C++ objects. Rewrite its hardcoded budgets to scale with host RAM/VRAM.
Good early candidates are texture streaming, decoded audio, and renderer scene packets because
their boundaries are already observable.

### M4 — Native game data model

Convert decompiled renderer, streaming, world, entity, quest, and UI systems to native layouts and
64-bit pointers. Translate at the remaining boundary when those systems call unreplaced PPC code.

### M5 — Remove the compatibility arena

When no translated code or Xbox ABI consumer needs a guest pointer, delete the guest memory/kernel
bridge. At that point the executable is a conventional native x64 PC game.

## Skate3Recomp lesson

Skate3Recomp validates this incremental design. Its title hooks capture mesh submissions and a
guest swap boundary, publish an owned `FrameScene`, and render through a backend-neutral D3D12 /
Vulkan RHI. Its host caches are ordinary vectors/maps keyed by guest object identities and content
fingerprints. It can suppress emulated draws and yield to the emulated frame on an unsupported
case. It did not replace the entire guest heap first.

Useful patterns for Fable:

- hook high-value title boundaries above raw PM4 where they are known;
- publish immutable per-frame native scene data;
- use coherent double-reads and payload fingerprints for concurrently streamed guest resources;
- keep native/emulated per-frame fallback while coverage is incomplete;
- expose host resource byte budgets, LRU retirement, and deferred GPU destruction;
- support D3D12 and Vulkan behind one small RHI contract.

The Skate-specific renderer source has no top-level license file, so treat it as architectural
reference unless permission/licensing is clarified. Its ReXGlue fork retains the BSD-licensed
runtime files; the audio cadence work adapted here came from that compatible runtime lineage.

## Audio is a separate clocked pipeline

The current choppy audio is not evidence of heap exhaustion:

- `Fable2_692.log` submits valid PCM at about 187 frames/second, exactly `48000 / 256`, with a
  steady queue depth of 7–8.
- The 3,856 `BaseHeap::AllocFixed` collision messages occur in a single roughly 30 ms startup scan.
  There is no out-of-memory result in that run.
- An instrumented build showed startup silence while the queue filled, then two consecutive
  five-second windows at 187.4 frames/second with zero inserted silence.

The fix is clock/queue ownership: prioritize the audio worker, briefly wait for replacement chunks
after returning guest credits, pace credits to wall time, and record underrun/callback-gap stats.
This work belongs to the future native audio engine regardless of which memory domain owns its
buffers.

## Immediate next steps

1. Keep the audio cadence patch and validate it during a normal audible user run.
2. Broaden the persistent renderer store beyond the guarded linear `k_8` subset.
3. Locate Fable’s texture registration/rebind/destruction hooks so host resources gain explicit
   title-level identity and lifetime beyond the now-shipped dirty-generation bridge.
4. Collapse the grouped heterogeneous frame into one command list using offset per-draw constant,
   descriptor, vertex, and index slices.
5. Only then decide whether a token handle is necessary; prefer stable Fable resource identities.
