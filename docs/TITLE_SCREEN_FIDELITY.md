# Title-screen fidelity gate

The native title screen is not considered complete until its presentation is
matched against the user's local Fable II capture and the extracted UI scene.
The runtime may remain a native PC implementation, but visual decisions must
come from these sources rather than hand-tuned substitutes.

## Confirmed evidence

- `ghidra_out/title_ui_re/frontendstartupscreen.bgf` names the three logo
  components `FXGUI_Logomain`, `FXGUI_Logomain_Fadein`, and
  `FXGUI_Logomain_Ambient`. All reference `Art\\GUI\\FrontEnd\\Textures\\fe_f2logo.tex`.
- The locally decoded `fe_f2logo` texture is a 1024x512 DXT1 atlas. Its lower
  half is the white Fable II silhouette used by the native `f2_logo.png`
  export; this is the source mask for the ambient pass as well as the title
  logo.
- `ghidra_out/title_ui_re/frontendstartupscreen.fac` provides the authored
  text anchors and UV records. The legal block is a fixed-width, wrapped
  group, not three independently guessed left-aligned strings.
- `fonts_en.swf` is a user-owned CWS SWF containing two DefineFont3 records,
  each named `Maiandra GD Fable` with 1,894 glyphs. The current fallback font
  remains a known fidelity gap until that vector font is converted or rendered
  through a runtime SWF font path.
- PM4 capture records identify the title's 512x256 format-59 (`DXT5A`) texture
  at `0x1A93D000`, tiled with Xenos endian mode 1. Raw memory must be Xenos
  detiled and endian-corrected before it is accepted as a title asset.
- In the `pm4_press_a_2026-07-26.f2pm4` title frame, draw sequences
  `95346..95395` are indexed sprite quads using vertex shader
  `6D15306961102F7D`, pixel shader `4B61E208208F3F5B`, and the 512x512
  format-20 atlas at `0x0FBE7000`. Their RT0 blend word is `0x07060706`;
  the Xenos register layout decodes that as source-alpha / inverse-source-alpha
  color and alpha blending. This is the strongest confirmed lead for the
  particle/cloud and sparkle portion of the reveal.
- The sprite vertex declaration is also known: 18 dwords per vertex, with
  position at offsets `0..2`, UVs at `12..13` and `14..15`, a float at `16`,
  and an RGBA8 value at `17`. The capture records the guest fetch addresses,
  but this capture does not contain the corresponding memory dependency
  records, so the exact UV rectangles, positions, and per-sprite constants
  are not yet recoverable from this file alone.
- Draw sequences `95396..95399` are four text draws using `0x1A93D000`; this
  confirms the earlier rejection of that texture as the blue reveal effect.

## Current native implementation

The D3D12 and Vulkan frontends load only user-selected/cooked UI assets. The
title ambient layer currently reuses the extracted logo mask behind the white
logo, with independent pre-reveal burst and settled-halo timing. That is a
temporary PC-native approximation; it is not being treated as the original
`FXGUI_Logomain_Ambient` material. No game art is shipped in the repository.

## Remaining gate items

1. Convert or render the `Maiandra GD Fable` SWF font and use its measured
   glyph bounds for prompt and legal wrapping.
2. Recover the `95346..95395` sprite geometry and constants from a capture that
   includes the guest memory dependencies (or add a one-frame live capture at
   the title reveal). Then map each UV rectangle in `0x0FBE7000` to its atlas
   element and reproduce the source-alpha blend in the shared D3D12/Vulkan
   effect path.
   The first PM4 candidate tested was rejected: `1A93D000` decodes to the
   Maiandra/prompt glyph atlas, not the blue reveal. A separate frontend atlas
   contains a radial sphere-shaped element, but rendering that crop through
   the current normal-alpha UI path produces a hard dark disk; its original
   material/blend path is not yet identified, so it remains research-only.
3. Keep the verified title-state timing: the startup reference holds the
   black/grey title card for about 5.9 seconds after title entry before the
   panorama fades in over about 1.1 seconds. Re-measure if the video/startup
   boundary changes.
4. Validate D3D12 and Vulkan with frame captures at the same reference times.

## Evidence commands

The reproducible research inputs are:

```powershell
& ghidra_out/title_ui_re/pm4_texture_probe_new.exe `
  Fable2Recomp/out/build/win-amd64-nightly/pm4_captures/pm4_press_a_2026-07-26.f2pm4
```

Use `ghidra_out/title_ui_re/pm4_press_a_probe11.txt` for the full draw block,
`frame95445_draws_summary.txt` for the compact sequence summary, and
`live_0FBE7000_0000000100000000_100000000.bin` plus its decoded PNG for the
user-owned atlas preview. Do not promote an atlas crop into runtime until its
draw sequence, UVs, material constants, and blend behavior are all tied to the
same capture.
