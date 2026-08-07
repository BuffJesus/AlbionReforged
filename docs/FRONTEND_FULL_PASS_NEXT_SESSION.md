# Frontend full-pass handoff — exact retail shell with native additions

> ▶▶ **UPDATE 2026-08-06 — read [`RETAIL_FRONTEND_SPEC.md`](RETAIL_FRONTEND_SPEC.md) first.** A full
> evidence-driven decompile pass of the retail frontend (XEX/PPC primary + LuaQ) is now consolidated
> there: state machine (`SLIDE`/`TitleItem`, A/B/Up/Down), input model, movie native API (real player
> `0x823C0D90`), title reveal, menu layout/animation constants, options, save/load fan, sound API, and
> the reusable RE machinery. This doc's options-parity plan still stands; the spec is the broader map.
> Resume queue + exact commands are in [`HANDOFF.md`](HANDOFF.md) ▶▶ ACTIVE TRACK and memory
> `fable2-retail-frontend-re-workflow`.

**Created:** 2026-08-05  
**Scope:** Fable II native PC frontend, from process launch through title, main menu,
options, and frontend-to-game transition.

## Objective

Complete one evidence-driven pass over the entire frontend until it matches the retail
experience in appearance, state transitions, input behavior, animation, prompts, sounds,
and content. The native PC frontend may add small, clearly owned features such as
Resolution and Anti-Aliasing, but those additions must sit inside the retail structure and
must not replace or visually dilute the stock menu.

The rule for the next session is:

> Decompile and trace first. Reuse the retail data and renderer structure. Implement only
> what the evidence supports. Use screenshots for verification, never as a substitute for
> finding the source setup.

## Current situation

`Fable2Native` currently has a working visual prototype for the title/main/options shell,
but the options page is not yet a parity implementation. The active renderer still contains
approximate C++ layout and duplicated legacy options paths.
**Resolution — WIRED 2026-08-07 (D3D12):** `FrontendController::resolution_{width,height}()` map the
index to a concrete size (1280×720/1600×900/1920×1080); the D3D12 app's `apply_resolution_setting()`
resizes the window (`SetWindowPos`+`AdjustWindowRectEx`) and swapchain (`resize()`) when the value
changes, and the value persists in the controller across leaving/reopening Options (unit-tested).
**Still pending:** the same wiring in the **Vulkan** app; and **Anti-Aliasing** has NO backend yet —
a flip-model D3D12/Vulkan 2D frontend needs MSAA-via-intermediate-RT or supersampling; it is
deliberately NOT faked. Do not describe AA (or Vulkan-app resolution) as complete.

Known areas that must be rechecked rather than trusted:

- Options left-page rails and selected-A bezel over the scrolling panorama.
- Exact page/prompt ordering: retail reads `Cancel` then B and `Accept` then A.
- Footer capsule, B placement, gold coin, value placement, and clipping.
- Motif, slider, arrow, frame, page, and shadow UVs.
- Options page open/close transitions and whether selection remains solid while scrolling.
- Root options order and content order.
- Actual behavior of every custom setting, not only its displayed label.
- Legacy/dead options drawing branches in `native_frontend_app.cpp`.

The current C++ implementation is a reference for what has already been attempted, not a
visual authority. Preserve useful asset extraction and tests, but replace guessed paths and
duplicate render passes when the decompilation establishes the real path.

## Retail evidence already recovered

### Options scene and population

The retail options scene is proven from the extracted/decompiled assets:

- `guiscripts.bnk` entry **#161**: `optionsscreen.fac`
- `guiscripts.bnk` entry **#162**: `optionsscreen.bsg`
- `guiscripts.bnk` entry **#163**: `optionsscreen.bgf`
- `ghidra_out/title_ui_re/research_bgfs/163_optionsscreen.bgf` is byte-identical to the
  extracted retail BGF.
- The BGF hierarchy is `Layer -> Camera/HierarchyCam -> Foreground -> Book -> Pages`,
  with `Page1`, `Page2`, `Page3`, `Page4`, `RightFrame`, and
  `ExpandableMenu -> OptionsMenuY`.
- Retail page order is **Game, Video, Controls, Audio**.
- Retail page data is:
  - Game: Subtitles, BreadcrumbSize, TutorialBoxes, MultiplayerOrbs, Auto_joinable.
  - Video: Gamma, CalibrationImage, `GUI_SCREEN_OPTIONS_GAMMA_GUIDE`.
  - Controls: InvertAim.
  - Audio: Sounds, Music, Voice, Speakers.

The supplied retail captures are in `resources/options/`:

- `GameSubMenu.png`
- `VideoSubMenu.png`
- `ControlsSubMenu.png`
- `AudioSubMenu.png`

These are the visual acceptance references. The retail captures show the purple left page;
the native build is intentionally allowed to retain the requested scrolling panorama behind
that page, but the rails, anchors, selected A ring, opacity, and typography must remain
retail-derived.

### Proven texture/material sources

Use the exact bank entries and their matching headers. Do not redraw or bitmap-edit them:

- `frames_page_texture` — parchment page.
- `frames_elements` — rails, controller elements, arrows, frame pieces.
- `motifs` — engraved page rules and motifs.
- `sliderframe` — slider frame/material.
- `CalibrationImage` — exact retail calibration image; the populated image occupies the
  upper 75% of the 512x512 payload and the black tail is unused texture data.
- `icon_gold_coin` — exact footer coin.
- Existing ABXY atlas `live_0D3F7000_0000000100000000_100000000.dds` — exact A/B prompt cells.
- `frame_new_left_top`, `frame_new_left_bottom`, `frame_new_right_top`,
  `frame_new_right_bottom` — page side frame pieces.

The relevant source banks are:

- `Fable2Recomp/assets/game/data/guiscripts.bnk`
- `Fable2Recomp/assets/game/data/art/gui/gui_textures.bnk`
- `Fable2Recomp/assets/game/data/art/gui/gui_texture_headers.bnk`
- `Fable2Recomp/assets/game/data/art/gui/gui_models.bnk`
- `Fable2Recomp/assets/game/data/art/gui/gui_streaming.bnk`

Use `Fable2AssetBrowser/source/build/f2tool.exe` to list/extract/verify bank entries. Record
the entry index, body hash, header hash, cooker command, and output hash for every new asset.

## Required investigation order

### 1. Freeze a clean baseline

Before changing frontend code:

1. Record `git status --short`; do not reset or discard existing work.
2. Build the current `Fable2Native` target and run the core tests.
3. Capture deterministic screens for Boot, Title, Main Menu, Options root, and all four
   option pages at the same window size as the retail references.
4. Record every known mismatch in a small table with source evidence and target behavior.
5. Identify and label the active render path. Remove ambiguity between the D3D12 app,
   Vulkan app, native UI renderer, and legacy/dead branches before editing.

### 2. Trace the real frontend execution path

Use all three evidence layers together:

1. **Retail data:** FAC/BSG/BGF, GUI texture/header/model/streaming banks, and LuaQ
   population/input scripts.
2. **Decompilation:** Ghidra output, generated `Fable2Recomp` C++, and address-level call
   flow for frontend state changes, GUI setup, event dispatch, animations, sounds, and
   page population.
3. **ReXGlue/PPC conversion:** use the recompiled PPC path as the behavioral oracle and
   trace the bridge points where XEX/PPC calls reach GUI, input, audio, texture loading,
   and presentation. ReXGlue is useful for answering “what does retail do?”; the shipping
   native frontend still needs a clean PC-facing implementation.

Do not hand-edit generated C++ as a permanent solution. If a generated function must be
observed or adapted, use weak overrides, a native adapter, or a documented hook in `src/`.
After any generated-code regeneration, run the repository’s dangling-goto fixer and verify
the Ghidra/codegen version before trusting addresses.

### 3. Rebuild the frontend state machine from evidence

Document and test this complete route:

`Boot -> IntroVideo -> Title -> MainMenu -> OptionsRoot -> OptionsPage -> OptionsRoot`

Also cover:

- Back from every state.
- Accept from every selectable item.
- Up/Down bounds and selection animation.
- Left/Right value changes.
- Mouse/keyboard/controller parity where supported.
- Prompt changes and controller glyph persistence during scrolling.
- Sound events for move, accept, back, and value change.
- Return from Options to Main Menu without stale page/selection state.
- Loading transition from the main menu and any frontend cancellation path.

The options model must be data-driven by stable IDs, not visual indices. Retail content keeps
its exact IDs/order. Native additions get explicit IDs and a documented ownership boundary.

### 4. Reconstruct the renderer in retail layer order

Treat the BGF/component hierarchy and PM4/decomp draw order as the renderer specification.
For each visible component record its source asset/material, UV rectangle and atlas
orientation, destination anchor at 1280x720/reference scale, alpha/blend/keying behavior,
z/layer order, animation timing, and whether it belongs to the retail page or the requested
panorama replacement.

The renderer must have one authoritative options-page path. Delete or quarantine duplicate
legacy purple/parchment paths after the active path is proven. No duplicated glyph, prompt,
rail, or frame pass may cover a retail pass and change its appearance.

### 5. Add the native PC twist without changing the stock shell

Keep the four retail pages and their existing fields intact. Add native settings in the least
disruptive place established by the layout pass, preferably as a compact Video display
section with the same label/value/arrow grammar:

- Resolution: enumerate actual supported PC modes.
- Anti-Aliasing: enumerate actual supported renderer modes.
- Optional future additions: window mode, VSync, frame cap, texture quality, or HDR only
  after the renderer backend has real implementations for them.

Every displayed setting must have a real backend owner. Resolution must update the native
window/swapchain safely and reflow the UI. Anti-Aliasing must select a supported render
configuration or be disabled from the menu if the backend cannot provide it. Do not leave
settings as memory-only labels.

### 6. Validate in a repeatable loop

For every page and state:

1. Launch from a fresh process with fixed window size and controller prompt mode.
2. Navigate using scripted input, recording state, selected ID, page ID, and setting values.
3. Capture the screen without accidentally changing focus unless the test explicitly covers
   focus behavior.
4. Compare against the matching retail reference at the same scale.
5. Use image measurements for anchors, bounds, alpha, and pixel deltas; use visual review
   only after numeric checks.
6. Patch one mismatch class at a time.
7. Rebuild, run tests, and repeat from a fresh process.

Do not accept “close enough” while any mismatch is unexplained. If a difference is
intentional (for example, the scrolling panorama or a new native setting), record it beside
the retail comparison and explain why it does not alter stock geometry or behavior.

## Completion checklist

The frontend pass is complete only when all of these are true:

- Retail Boot, Title, Main Menu, Options root, and all option pages have a documented source
  path and a deterministic capture.
- Every visible asset comes from a verified retail bank entry or an explicitly documented
  native addition.
- No guessed UVs, duplicate active render paths, stale legacy branches, or unexplained
  clipping remain.
- Selected A ring, menu rails, page motif, arrows, slider, calibration image, prompts, Back
  capsule, gold coin, text, and transitions match the retail reference at target scale.
- Options page order/content and all input transitions match the decompiled behavior.
- Custom Resolution and Anti-Aliasing settings are actually applied by the PC backend and
  survive leaving/reopening Options.
- Frontend audio/input behavior is verified, not merely drawn.
- `cmake --build Fable2Native/build --config RelWithDebInfo --parallel 4` succeeds.
- `ctest --test-dir Fable2Native/build -C RelWithDebInfo --output-on-failure` succeeds.
- `git diff --check` succeeds.
- This document is updated with final evidence, hashes, limitations, and the exact final
  deterministic smoke-test command.

## Files to read first next session

1. This document.
2. `docs/RETAIL_FRONTEND_DRAW_TRUTH.md`.
3. `docs/TITLE_SCREEN_HANDOFF_2026-08-03.md`.
4. `docs/MENU_SYSTEM_RE.md`.
5. The active paths in `Fable2Native/src/native_frontend_app.cpp` and
   `Fable2Native/src/native_frontend.cpp`.
6. `Fable2Recomp/assets/game/data/guiscripts.bnk` and the extracted/decompiled OptionsMenu
   population/input scripts.
