# Title-screen research handoff — 2026-08-03

This is the starting point for the next session. The goal remains an asset-free,
native PC title screen that matches Fable II's startup presentation while loading
all user-owned data from the selected game source.

## What is proven

- `frontendstartupscreen.bgf` contains `FXGUI_Logomain`,
  `FXGUI_Logomain_Fadein`, and `FXGUI_Logomain_Ambient`. All three reference
  `Art\\GUI\\FrontEnd\\Textures\\fe_f2logo.tex`.
- `fe_f2logo` is a 1024x512 DXT1 atlas. Its lower-half silhouette is the source
  mask used by the native logo export.
- `fonts_en.swf` contains two `Maiandra GD Fable` DefineFont3 records with
  1,894 glyphs. The fallback title/menu font is still not acceptable for the
  fidelity gate.
- In the PM4 title frame, sequences `95346..95395` are the reveal's indexed
  sprite block: VS `6D15306961102F7D`, PS `4B61E208208F3F5B`, main atlas
  `0x0FBE7000` (512x512, format 20), and changing 256/512 detail textures.
- RT0 blend `0x07060706` is source-alpha / inverse-source-alpha for color and
  alpha. The reveal is not proven to use additive blending.
- Sequences `95396..95399` use `0x1A93D000` and are text/glyph draws. This is
  not the blue sphere/cloud effect.
- The sprite vertex format is 18 dwords per vertex: position `0..2`, UVs
  `12..13` and `14..15`, float `16`, RGBA8 `17`.
- The reference title card holds the black/grey startup state for about 5.9
  seconds after title entry, then reveals the panorama over about 1.1 seconds.
  Reference: [Fable II startup video](https://www.youtube.com/watch?v=PvYnezs6Qi8).

## What is deliberately not claimed

The current blue logo-mask passes and procedural sparkle placement are a native
approximation only. The radial sphere crop from the frontend atlas rendered as
a hard dark disk through the current normal-alpha path, so it remains
research-only. No guessed sphere, additive pass, particle layout, or font
replacement should be promoted to the runtime.

## Exact next task

Capture one title-reveal frame with the guest memory dependencies for the
`95346..95395` draws, then decode:

1. each six-index quad's position and atlas UVs;
2. its second UV/detail texture selection;
3. the pixel constants used by `4B61E208208F3F5B` (`c30`, `c31`, `c46`,
   `c47`, `c77` are the relevant shader inputs);
4. the atlas element and timing represented by each draw.

Only after those four items agree with the capture should the shared D3D12 and
Vulkan effect implementation replace the approximation.

Research files:

- `ghidra_out/title_ui_re/pm4_press_a_probe11.txt`
- `ghidra_out/title_ui_re/frame95445_draws_summary.txt`
- `ghidra_out/title_ui_re/ps_4B61E208208F3F5B_ucode.txt`
- `ghidra_out/title_ui_re/vs_6D15306961102F7D_ucode.txt`
- `ghidra_out/title_ui_re/live_0FBE7000_0000000100000000_100000000.bin`
- `ghidra_out/title_ui_re/live_0FBE7000_0000000100000000_100000000_512x512_bc3.png`

The existing PM4 capture command is documented in
`docs/TITLE_SCREEN_FIDELITY.md`. The capture currently identifies the fetch
addresses but does not include the required vertex/index memory records, which
is why the geometry step is still open.

## Current source state

The last two commits are:

- `9778db1` — match title panorama reveal timing;
- `8c7c4a9` — document the rejected title sphere candidate.

The native source builds cleanly and its single test passes. Resume with:

```powershell
cmake --build Fable2Native/build --config RelWithDebInfo --parallel 4
ctest --test-dir Fable2Native/build -C RelWithDebInfo --output-on-failure
```

The repository must remain free of shipped game assets. Runtime assets continue
to come from the user's selected/cooked source root.
