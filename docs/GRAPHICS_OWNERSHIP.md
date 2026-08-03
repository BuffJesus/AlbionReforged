# Graphics Ownership — our own rendering pipeline (Vulkan / modern API)

> **⚠ SUPERSEDED (same night, 2026-07-19) by [OWN_RENDERER.md](OWN_RENDERER.md).** A decomp session
> proved the premise of Phases G2/G3 below WRONG: the 360 D3D API is **LTCG-inlined into the game**
> (draw-writer functions have zero references; the PM4 kick has 144 call sites fused into game
> code), so there is no callable D3D boundary to hook. The viable seam is the **PM4 ring** (already
> the GPU-plugin interface) — see OWN_RENDERER.md P0-P5 for the plan of record. Phase G1 (claim the
> plugin fork) and the API-choice reasoning (D3D12-first, thin RHI for Vulkan) still stand and are
> folded into that plan.

User directive (2026-07-19): "figure out how to get our own graphics pipeline integrated. Vulkan,
etc." This is the graphics rung of the recomp → decomp → **own** ladder in
[RUNTIME_OWNERSHIP.md](RUNTIME_OWNERSHIP.md). Three phases, each shippable on its own.

## Where rendering happens today (the stack we must own)

```
Fable II guest code (PPC)                      ← ours already (recomp + Ghidra decomp)
  └─ Xbox 360 D3D ("d3d9-360") — STATIC LIB INSIDE THE XEX   ← ★ the ownership boundary
       └─ PM4 command ring + Xenos GPU registers (emulated)
            └─ rexgpu-xenos.dll plugin (Xenia-derived): PM4 parser, EDRAM model, resolves,
               Xenos shader microcode → DXIL, D3D12 backend
                 └─ D3D12 on the host GPU
```

Key facts:
- The runtime moved graphics behind a **plugin interface** (`rex::system::LoadGpuPlugin` →
  `IGraphicsSystem`, nightly SDK). Backends are swappable DLLs — this is our integration point.
- We ALREADY patch rexgpu-xenos (morph-skip, readback cvars, resolve diags) and build it from
  `rexglue-src` — it is de-facto forked; we just haven't claimed it.
- Xbox 360 titles statically link the 360 D3D library, so **every D3D call the game makes is guest
  code inside our XEX** — nameable in Ghidra, overridable with weak-override host hooks exactly like
  any other guest function. This is what makes true ownership possible: we can intercept the game at
  the *API* level (draw calls, state sets, texture binds) instead of the *GPU command* level
  (PM4/EDRAM), and once we do, the entire Xenos emulation layer becomes unnecessary.

## Phase G1 — claim the plugin (days)
Formally own the fork we already have: move/rename rexgpu-xenos into our tree as the project's
renderer, wire its build into the Fable2 CMake flow (same pattern as the patched-runtime POST_BUILD
staging), delete dead band-aids (command_processor 0x1B1xx morph-skip). Zero behavior change; ends
the "patch a vendored blob" workflow. Deliverable: `rexgpu-fable2.dll` built from our source.

## Phase G2 — map the game's D3D-360 API surface (the decomp step, weeks, parallelizable)
In Ghidra, identify and label the statically-linked 360 D3D layer:
- Entry points: Draw*/SetRenderState/SetTexture/SetVertexShader/Present/BeginTiling etc. — found by
  tracing who writes the PM4 ring + known 360-D3D patterns (Xenia's docs + XeSDK headers name the
  whole surface).
- Catalog per call: guest address, signature, frequency, which engine systems call it (we already
  know the engine sites: DeviceState_FrameDispatch 0x82388380, the texture streaming stack, the
  CustomAtlas compositor, Bink player).
- Deliverable: `ghidra_out/d3d360_api_catalog.tsv` + a spec doc. This is pure RE — safe to run as
  background decomp sessions, and it also demystifies every remaining texture/streaming bug (the
  black-skin class lives exactly one layer above this API).

## Phase G3 — native renderer behind that API (the ownership step, the big one)
Write our own `IGraphicsSystem` plugin that does NOT emulate Xenos: weak-override the G2 catalog
(the game's D3D-360 entry points) and translate calls directly to a modern API. Start hybrid:
- **Stage A (hybrid):** override a first vertical slice (Clear/Present/simple draws) natively while
  everything else still routes through the Xenos plugin — both backends alive during bring-up,
  per-call switchable. This mirrors how we took over the runtime (function-by-function
  weak-override) and gives pixels on screen early.
- **Stage B:** move the big systems over — vertex/pixel shaders (the game's shaders are 360 shader
  bytecode; keep Xenia's microcode translator initially, then optionally recompile the game's
  ~shader set offline once and ship native shaders), render targets (kill the EDRAM model — use
  plain host RTs; this single change erases the entire resolve/readback bug class, including what
  made black-skin diagnosis so painful), textures (host-native pool — unblocks the GPU-texture
  reroute P2 that is paused on write-watch design, and the 512MB physical wall).
- **Stage C:** retire rexgpu-xenos. The game renders through our renderer only. Free wins at this
  point: real ultrawide/resolution/FOV, modern AA/upscalers, render hooks for the modding API
  (Fable II Studio overlays, custom shaders for mods).

### API choice: Vulkan vs D3D12
- The current plugin + all our diagnostics/tools are D3D12; Xenia also has a Vulkan backend to crib
  from. **Recommendation: keep D3D12 for Stage A** (shortest path — we can reuse the existing
  plugin's device/swapchain/shader-translator plumbing while swapping the top half), and make the
  Stage B renderer API-agnostic behind a thin RHI so a **Vulkan backend** is a backend, not a
  rewrite. Vulkan buys Linux/Steam-Deck later; it costs nothing to keep the door open and a lot to
  walk through it first.

## Sequencing vs current work
G1 anytime (mechanical). G2 can start now as background decomp (same loop as the Lua/GDB/terrain
tracks). G3 Stage A only after the texture-delivery fix stabilizes (don't own a layer while
actively debugging the layer above it). The delivery/pop-in work is not wasted — G2/G3 need
exactly that texture-streaming understanding, and Stage B is what deletes the bug class for good.
