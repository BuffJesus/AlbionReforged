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

## Current native implementation

The D3D12 and Vulkan frontends load only user-selected/cooked UI assets. The
title ambient layer now reuses the extracted logo mask behind the white logo,
with independent pre-reveal burst and settled-halo timing. No game art is
shipped in the repository.

## Remaining gate items

1. Convert or render the `Maiandra GD Fable` SWF font and use its measured
   glyph bounds for prompt and legal wrapping.
2. Decode the captured title effect resources through the Xenos texture path
   and confirm whether they are additional ambient animation or transient
   particle data before adding them.
3. Keep the verified title-state timing: the startup reference holds the
   black/grey title card for about 5.9 seconds after title entry before the
   panorama fades in over about 1.1 seconds. Re-measure if the video/startup
   boundary changes.
4. Validate D3D12 and Vulkan with frame captures at the same reference times.
