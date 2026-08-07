# Fable2Native frontend — session handoff (2026-08-07)

Resume-critical state. See also memory `fable2native-frontend-architecture` (has a RESUME-HERE block
at the top) and `docs/FRONTEND_ARCHITECTURE.md`.

## Headline: the D3D12 frontend app is now 100% ImGui-free
`Fable2Native/src/native_frontend_app.cpp` (the **default** backend) no longer uses ImGui at all.
Deleted this session: the ~1163-line ImGui fallback render block, `ImGui_ImplDX12/Win32` Init +
Shutdown + `NewFrame`/`Render`/`RenderDrawData`, the `ImGui_ImplWin32_WndProcHandler` hook, and the
`imgui*` includes. Every screen renders through the native `f2::render` scene now:

| Screen | Native renderer |
|---|---|
| Title | `render_native_title` — wordmark, **white** sparkle field (alpha_mask + additive), "Press ENTER to start", 3 legal lines |
| Main menu | `render_native_main_menu` — solid leather buttons (ability_elements mask + menu_surface leather), gold rims, native-font labels |
| Options | `render_native_options_chrome` + `render_native_options_page` — Options title pill, submenu tabs, Back/gold footer, settings panel (game/video/controls/audio) |
| Intro/attract video | `render_native_video` — full-screen quad of the decoded frame |
| Loading | `render_native_loading` — centered caption |

Build + `ctest` GREEN. Title + menu screenshot-verified. **Everything is UNCOMMITTED.**

## Fixes verified this session
- **Translucent menu buttons (the recurring bug): FIXED.** Real cause was NOT the material/UV — it was
  an *ungated* ImGui dark-veil `AddRectFilled` (~65% alpha) drawn over the native buttons every frame
  (ImGui rendered last). Deleting the ImGui path removed it. Buttons now opaque + use both correct
  textures. (Earlier UV/alpha_mask edits did nothing because they changed the native body *under* the
  veil — lesson: confirm which draw is actually on screen.)
- Title sparkles now **white** (`alpha_mask` shader mode: RGB from vertex color, coverage from texture
  alpha — turns the pink star sprites white; additive).
- **NativeFont** atlas (vendored public-domain stb_truetype at `third_party/stb_truetype.h`) replaced
  ImGui's font. Latin-1 range (© ® render), ascent-correct (top-left positioning like ImGui::AddText),
  plus an opaque-white block in the atlas corner for solid UI rects (`add_rect`). Unit-tested.
- Native **mouse hit-testing** replaced `ImGui::InvisibleButton` (menu/cards/title). Additive-blend PSO
  added to `native_ui_renderer` (`UiQuad.blend_mode`/`alpha_mask`).

## OPEN — needs attention next session
1. **Mouse UX (user-reported "erratic/too sensitive").** Hover-select thrashed the centered-wheel menu
   (selecting re-centers → rows scroll under the cursor → re-select). Shipped fix at session end =
   **click-only** (hover inert; a click selects+activates the item under the cursor; cards select on
   click; title advances on click). Compiles; **not yet user-verified** live.
2. **Selected-row prompt gone.** The "ENTER"/"LEFT CLICK" hint on the left was in the deleted ImGui
   block; native only draws the controller A-bezel. Re-add via `emit_text` if wanted.
3. **Options settings mouse-adjust dropped** (value +/- by click). Keyboard Left/Right still adjusts.
4. **Vulkan app still uses ImGui.** `src/native_frontend_vulkan_app.cpp` (~1840 lines) is a separate
   inline renderer — still ImGui + still the OLD translucent buttons. Default is D3D12 so users see the
   fixes, but `f2native_imgui` (CMake target) can't be deleted until Vulkan is migrated too (port the
   native paths, or make it consume the shared `f2::render::UiDrawList` scene).

## Videos — cooking + playback WORK (and are in the installer)
- `tools/cook_videos.py` converts the Bink `.bik` startup movies → `videos/*.mp4` (H.264/AAC) + a
  `manifest.json`. **Verified** it produces the 3 startup clips (microsoft_logo, lionhead_logo,
  middlewarelogos; `intro` is the big 110 MB one).
- Already wired into the installer: `src/native_cook.cpp` runs `cook_videos` when `options.cook_videos`.
- Playback: the frontend loads `<--video-root>/videos/<clip>.mp4` and decodes via **Media Foundation**
  (handles H.264). The intro was black in tests only because `--video-root` was empty. The clip sequence
  is hardcoded in `native_frontend.cpp` (`intro_videos_.set_sequence`).

## Screenshot recipe (for verification)
Kill stale `f2native_frontend`, then launch
`build/RelWithDebInfo/f2native_frontend.exe --skip-intro --start-menu --ui-root
ghidra_out/title_ui_re --game-dir Fable2Recomp/assets/game`, wait for `MainWindowHandle` (flaky ~5s,
use a retry loop), `PrintWindow(hwnd, hdc, 2)` (PW_RENDERFULLCONTENT for the flip-model swapchain).
Ready-made scripts are in the session scratchpad (`native_menu_shot.ps1`, `native_title_shot.ps1`,
`native_options_shot.ps1`).
