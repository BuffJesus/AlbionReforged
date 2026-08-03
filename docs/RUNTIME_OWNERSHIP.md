# Owning the Runtime — from ReXGlue patches to our own engine

How the runtime patches become permanent, and the ladder from "patching ReXGlue" to "our own engine
tailored to our use case" (renderer, audio/video pipeline, memory, kernel, modding API baked in).
Companion to [MODDING_MASTER_PLAN.md](MODDING_MASTER_PLAN.md) §2 (ownership strategy).

## The key reframe: rexglue-src is ALREADY our fork

The runtime (`rexruntime.dll`) is built from **`rexglue-src/`, which is our in-repo source tree** — we
already modify it (page-protection recovery in `core/`, XMA-input translation in `audio/`,
`MmGetPhysicalAddress`, `_vsnprintf` clamp, ...). So the patches are *already permanent in our source*.
The only "re-adding" pain was a **build-staging artifact**: the nightly Fable2 build linked the
*prebuilt* (unpatched) runtime from the SDK on `CMAKE_PREFIX_PATH` and staged that over ours.

**Fixed (2026-07-17):** a Fable2 `CMakeLists` POST_BUILD step now auto-copies our patched
`rexglue-src` runtime over the SDK's after every link (verified). No manual step remains.

ReXGlue is Xenia/XenonRecomp-derived, so `rexglue-src/src/` is a **full engine runtime** we can own,
subsystem by subsystem:
`audio/` (XMA decode/mix) · `graphics/` (Xenos command processor → D3D12 plugin) · `core/` (guest
memory/VA) · `kernel/` (xboxkrnl/xam shims) · `filesystem/` (VFS) · `input/` · `system/` · `ui/`.

## The ownership ladder

1. **Fork + patch — DONE.** `rexglue-src` is ours; we patch it and rebuild the runtime.
2. **Build-from-source — CONSUMER INTERFACE FIXED (2026-07-17), makes "permanent" true at the build
   level.** Switch the Fable2 build to build `rexruntime` **from `rexglue-src/`** (`REXSDK_DIR=rexglue-src`
   → `add_subdirectory`) instead of consuming the prebuilt SDK on `CMAKE_PREFIX_PATH`. Then the build
   *produces* our runtime directly — there is no prebuilt to override, the POST_BUILD copy becomes
   unnecessary, and `rexglue-src` is a first-class, versioned in-repo component. This is the truest
   sense of "the patch is permanent": our source IS the runtime the build ships.
   The three consumer-interface gaps that used to block this are now fixed in `rexglue-src` (general,
   correct for any `add_subdirectory` consumer): (a) SSSE3/`-march=x86-64-v3` for `memory.cpp`; (b) the
   imgui include dir surfaced onto the host target in `rexglue_configure_target`; (c) the generated
   `<rex/version.h>` dir exported via `REXGLUE_GENERATED_INCLUDE_DIR`. The two host sources the SDK
   injects into the consumer (`rex_app.cpp`, `windowed_app_main_sdl.cpp`) now compile from source
   (verified). See memory `fable2-runtime-build-integration`.
3. **Rebrand + own the build.** Treat `rexglue-src` as *our* runtime (name it, own its CMake, drop the
   external-SDK path entirely). ReXGlue's codegen (`src/codegen/`) still translates the PPC exe, but the
   host runtime is ours.
4. **Rewrite subsystems to spec — the real work, tailored to PC + our goals.** The runtime is modular;
   replace Xenia-derived implementations with ours, one at a time:
   - **Graphics / render pipeline:** the Xenos command processor → currently a D3D12/xenos plugin. Own
     it → our renderer (D3D12 or Vulkan), arbitrary resolution/FPS, better shaders, fix the RTT/resolve
     issues (the adult-hero black-skin bug), modern post-processing, PC-native lighting. The first
     live textured frame and persistent byte-accounted D3D12 texture store now exist in
     `rexgpu-native`; `Fable2_696.log` proves three native allocations are reused without repeat
     upload. Logs 698-699 add guest dirty-generation tracking: actively rewritten video is
     invalidated, while stable cached texture ranges can skip repeated guest copying and hashing.
     Log 701 adds submission-indexed transient-resource retirement, so upload heaps no longer force
     an immediate whole-queue wait solely to remain alive. Log 702 replaces the reused per-draw
     resources with a three-slot fence-owned ring and proves two native draws in flight. Log 704
     safely coalesces the first 24 identical captured draws into one command-list batch and one
     output copy/presenter refresh while preserving the exact known textured frame. Log 706 groups
     a three-run, 26-draw heterogeneous frame under one fence signal, presenter refresh, and final
     output copy without changing the accumulated rectangle or textured-frame hashes.
   - **Audio pipeline:** XMA decode/mix → our audio engine (higher-quality mixing, custom SFX/music
     injection, modern codecs) — builds on the XMA fixes already in `audio/`.
   - **Video pipeline:** Bink cutscene playback → our video path (replaceable cutscenes, modern codec).
   - **Memory / kernel — the deepest 360-ism, and where "360-game-on-PC" becomes "PC game."**
     Today the recomp *emulates* the 360 memory model: a guest virtual address space (membase
     `0x100000000`, guest addrs `0x82…`) mapped to host memory, a fixed guest heap + 256 MB CRT heap
     (`rexglue-src/src/kernel/crt/heap.cpp`, o1heap), and `total_physical_pages = 0x20000` (512 MB)
     reported to the game. The migration to a PC-native model, subsystem by subsystem:
     1. **Runtime bridge (now, we own rexglue-src):** keep the guest address space ABI-correct for
        translated code, but move host-owned copies — GPU resources, decoded assets, audio/video
        buffers, shader caches, mod data — into ordinary 64-bit PC allocations. A raw host pointer
        cannot replace a 32-bit guest pointer while untranslated code still stores and performs
        arithmetic on it. Use stable resource IDs or generation-checked handles at explicit
        boundaries. See [NATIVE_PC_MEMORY.md](NATIVE_PC_MEMORY.md).
     2. **Decomp (the real shift):** as we decompile the game's memory subsystems, reroute their
        allocators (the game's new/delete, asset heaps, pools) to a **PC-native heap** — no guest-VA
        constraint — and rewrite the **hardcoded 360 budgets** (asset/streaming/entity/pool caps) to
        scale with PC RAM (GBs, dynamic). Decompiled code uses native pointers, not the membase mapping.
     3. **Endgame:** the decompiled game is native x64 using PC memory management (dynamic heaps, PC RAM,
        PC-scaled budgets) with no 360 emulation layer — a genuine PC game; the 360 was the *source*, not
        a constraint. rexglue translates only what isn't decompiled yet, shrinking to nothing.
     **Principle:** don't perpetuate 360 constraints — as we decompile, *replace* the emulated 360
     memory model with PC-native systems. The heap is the foundation of that shift.
   - **Modding API baked in:** `register_native`, `GetText`, asset-load, input, UI hooks become
     first-class runtime features (see NATIVE_MODDING_API.md), not env-gated patches.
5. **Decompile the game code (Track B).** Replace ReXGlue's PPC→C++ *translation* with our own
   understood C++ subsystem by subsystem (verified against the recomp as an oracle).
6. **End state.** Our owned engine = our runtime (step 4) + our decompiled game (step 5). ReXGlue is
   reduced to the initial translation of whatever we haven't decompiled — and eventually gone. Our
   audio/video/render pipeline, memory model, and modding API are *ours*, tailored to the PC port + the
   Creation-Kit/native-modding goals.

## Sequencing (non-blocking)
- **Now:** patches permanent at build level (POST_BUILD ✓); optionally take **step 2** (build-from-source)
  to make it structural.
- **Near:** own the build (step 3); bake the modding API into the runtime as we build it (register_native).
- **Ongoing:** rewrite subsystems (step 4) where our goals demand it — render pipeline (res/shaders/RTT
  fix) and audio/video are the highest-value for a PC port; memory/limits next.
- **Underneath:** decomp (step 5) continuously converts translated blobs into owned code.

Nothing here blocks modding or gameplay work — the runtime keeps running while we progressively own it.
# Native-port update

The end state is a standalone `Fable2Native` C++23 runtime. `rexglue-src` remains reproducible
for oracle runs and evidence capture, but new product systems should not be added there unless
they directly improve parity data.
