# Fable2Native frontend architecture — decoupled + modder-friendly

**Created 2026-08-07.** The refactor target for the native frontend: one executable, a clean
backend seam (D3D12/Vulkan behind one interface), and a **data-driven, moddable** UI. Tracks
`NATIVE_PORT_PLAN.md`'s `f2render` layer. Backend toggle is **restart-applied** (memory
`fable2native-frontend-architecture`).

## Layering (target)
```
FrontendController  (pure logic/state: slides, selection, options, stable IDs)   [f2core]
        │  produces
        ▼
UiScene = std::vector<f2::render::UiQuad>   (backend-neutral draw list: asset id + rect + uv + color)
        │  built by a shared scene builder from the controller state + a LAYOUT DATA description
        ▼
IRenderBackend   (f2::render — initialize/resize/create_texture/draw_ui/present/set_msaa)  [f2render]
     ├── D3D12Backend      (wraps the existing device/swapchain + NativeUiRenderer)
     ├── VulkanBackend     (same contract; reaches D3D12 UI parity via the SHARED scene builder)
     └── NullRenderBackend (headless; used by tests/CI — already shipped + unit-tested)
```
Nothing above `IRenderBackend` mentions D3D12 or Vulkan. Textures are opaque `TextureId`s the backend
resolves. One `FrontendApp` owns window + game + input + audio + video + the selected backend.

## Status
- ✅ **Contract shipped + tested (2026-08-07):** `include/f2/render/render_backend.h`
  (`TextureId`, backend-neutral `UiQuad`, `IRenderBackend`, `BackendCaps`) +
  `null_render_backend.h` (headless), exercised in `native_core_tests`.
- ✅ **One executable + restart-applied backend toggle** (`frontend_main.cpp`,
  `native_frontend_config.*`, Options "Renderer" row). Vulkan-only exe retired.
- ⏳ **Remaining migration (incremental, build-green each step):**
  1. `D3D12Backend : IRenderBackend` — wrap the current `native_frontend_app.cpp` device/swapchain +
     `NativeUiRenderer` (map `TextureId`→D3D12 descriptor via a texture registry).
  2. Extract the shared **scene builder** (the ~164 layout calls) → produces `UiQuad`s from the
     controller. Both backends consume it; Vulkan reaches parity for free.
  3. `VulkanBackend : IRenderBackend`; collapse the two apps into one `FrontendApp`.
  4. **MSAA/AA** → `IRenderBackend::set_msaa` (intermediate multisampled RT + resolve); the Options AA
     setting reads `BackendCaps` and disables itself when unsupported (never faked).

## Modder-friendly design (a first-class goal)
The frontend must be reskinnable/relayoutable **without recompiling**. The seams that deliver that:
1. **Assets are loose + manifest-driven, cooked from the user's own game, shipping nothing** — already
   true for audio (`native_audio/audio_manifest.ini`) and textures (`ui_manifest.ini`); see
   `docs/GUI_TEXTURE_COOK_PLAN.md`, memory `fable2-native-asset-cook-pipeline`. A mod drops a
   replacement DDS/WAV; a **loose-file override layer** wins over the base package (like Skyrim loose
   files / UE `.pak` overrides). No repack tool needed.
2. **UI layout as DATA, not code.** The scene builder should interpret a **layout description**
   (elements = asset key + anchor + rect + uv + color + z + animation ref) that lives in a data file,
   so modders relayout the menu/title/options by editing data + hot-reload — not the 164 hardcoded
   draw calls in C++ today. Migration step 2 above is where the hardcoded layout becomes data-driven.
   The retail truth for the initial data (UVs/anchors/z/animation) is already RE'd in
   `RETAIL_FRONTEND_DRAW_TRUTH.md` / `RETAIL_FRONTEND_SPEC.md`.
3. **Stable IDs, not visual indices.** Menu/options items are keyed by stable string IDs
   (`FrontendController::add_menu_item/remove_menu_item/select_menu_item`) so a mod can add/remove/omit
   entries without depending on draw order or an address. Keep this invariant in the scene layer.
4. **Event/behavior hooks.** The retail frontend fires named events (`SE_GUI_*` sounds, `OnPressA/B`,
   `OUTRO_FINISHED`) — expose these as a small, documented hook surface so mods can react (play a
   sound, swap a screen, add a slide) without touching the renderer. Mirrors the recomp's proven
   `register_native` Lua-hook model (memory `fable2-modapi-register-native`).
5. **Backend-neutral by construction.** Because mods target the `UiScene`/asset/ID layer — never the
   backend — a reskin/relayout works identically on D3D12 and Vulkan.

## Invariants to preserve during migration
- Never regress the working D3D12 frontend; migrate behind the interface with the build green at each
  step and `native_core_tests` passing.
- Never ship a copyrighted asset; the runtime consumes only user-cooked packages + loose overrides.
- Never fake a setting (AA): gate it on `BackendCaps`.
