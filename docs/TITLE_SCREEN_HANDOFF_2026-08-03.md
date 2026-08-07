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
  1,894 glyphs. A user-owned export is now installed as `title_font.ttf` in
  the local UI root and used by both native frontends; prompt/legal metrics
  remain part of the visual fidelity gate.
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

## Completed evidence task

The live sidecar capture recovered the guest memory dependencies for the
`95346..95395` draws and decoded:

1. each six-index quad's position and atlas UVs;
2. its second UV/detail texture selection;
3. the pixel constants used by `4B61E208208F3F5B` (`c30`, `c31`, `c46`,
   `c47`, `c77` are the relevant shader inputs);
4. the atlas element and timing represented by each draw.

All four items agree with the capture, so the shared D3D12 and Vulkan effect
implementation now replaces the approximation when the user supplies the
recovered atlas input.

## Next task

Validate the user-exported `ambient_detail_frames` inventory and capture the
18 separate format-18 baseline textures with their memory dependencies. The
current local PM4 captures contain the baseline draw descriptors but no
matching memory records for the `0x0E367000..0x0E527000` texture set, so the
native runtime must continue using an optional user-owned `ambient_baseline`
precomposite until that evidence exists.

Research files:

- `ghidra_out/title_ui_re/pm4_press_a_probe11.txt`
- `ghidra_out/title_ui_re/frame95445_draws_summary.txt`
- `ghidra_out/title_ui_re/ps_4B61E208208F3F5B_ucode.txt`
- `ghidra_out/title_ui_re/vs_6D15306961102F7D_ucode.txt`
- `ghidra_out/title_ui_re/live_0FBE7000_0000000100000000_100000000.bin`
- `ghidra_out/title_ui_re/live_0FBE7000_0000000100000000_100000000_512x512_bc3.png`

The original PM4 capture command is documented in
`docs/TITLE_SCREEN_FIDELITY.md`. The later shader-filtered live sidecar added
the required vertex/index records and closed the geometry step.

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

## 2026-08-04 capture progress

The missing live-memory evidence was recovered with an opt-in Xenos sidecar
capture. The external ReXGlue GPU probe now accepts:

```text
FABLE2_PM4_GEOMETRY_DUMP=<local text path>
FABLE2_PM4_GEOMETRY_DUMP_SHADER=6D15306961102F7D
```

The sidecar records the decoded draw, VSC/PSC constant bitmaps and values,
vertex bytes, index bytes, and all texture descriptors for each matching draw.
The local 2026-08-04 run recorded 39,273 indexed draws; all 39,273 vertex and
index reads succeeded. This sidecar is research-only and is not part of the
source repository.

The shader ucode confirms the coordinate path: `tf13` samples the main atlas
from vertex UV0 (`r0.xy`), while `tf14` samples the changing detail texture
from vertex UV1 (`r0.zw`). In representative draw `162910`, the main atlas is
`0x0FBE7000` (512x512, format 20, tiled, endian 1), the detail texture is
`0x0E547000` (512x512, format 18, tiled, endian 1), and the four UV pairs are:

```text
UV0: (0.875, 0.5), (1.0, 0.5), (0.875, 0.25), (1.0, 0.25)
UV1: (0.08119202, 0.343429565), (0.140719, 0.343429565),
     (0.08119202, 0.231448323), (0.140719, 0.231448323)
```

For the same draw, the relevant pixel constants are known: `c30 = (1,1,1,1)`,
`c31 = (1,0,0,0)`, `c46 = (1,0,0,0)`, `c47 = (1,0,0,0)`, and
`c77 = (0,0,0,0)`. The vertex/index records use the known 18-dword format and
the six indices decode as `0,1,2,2,3,1`. The active shader path is therefore
`out.rgb = detail.rgb * c47.x + detail.a * main.rgb`,
`out.a = main.a * detail.a`, with source-alpha blending at RT0. The current
native atlas path intentionally stops before this dual-sampler material step.

The sidecar now writes `SWAP` markers (format version 2), which makes the
timing measurable without a full PM4 replay. In the 2026-08-04 timed run,
`0x0FBE7000` appears on frames `792..912`; each frame also has 18 baseline
draws bound to separate format-18 textures. Three additional four-draw slice
groups first appear on frame 794:

```text
UV0 0.740..0.865 x 0.250..0.500   x bounds -12.04..-4.30
UV0 0.871..0.873 x 0.250..0.500   x bounds -11.81..-0.47
UV0 0.875..1.000 x 0.250..0.500   x bounds  -7.98..-0.24
```

Those 12 animated draws settle by frame 803 and the complete 30-draw block
then repeats through frame 912. At a 60 Hz presentation cadence, the live
geometric slide is approximately 10 frames (167 ms); this is distinct from
the higher-level source-video interval of roughly 1.1 seconds for the full
panorama fade. The atlas preview identifies the surrounding static FX set
(plus/cluster, starburst, ring, radial sphere, clover, and border regions).

The geometry, detail-UV, constant, atlas-rectangle, and live timing evidence
is now recovered, and the measured three-slice atlas sequence is promoted
through the shared D3D12/Vulkan path when `ambient_atlas` is present in the
user's manifest. The native loader now applies the recovered dual-texture
detail combine for an optional same-sized `ambient_detail` snapshot or a
manifest-provided `ambient_detail_frames` sequence on both backends. The
remaining parity work is validating the user-exported frame inventory and the
baseline ambient draws. A follow-up audit of the timed sidecar makes
the baseline boundary explicit: those 18 draws bind separate 512x512 format-18
textures, so they cannot be reproduced from the single main-atlas input alone.
The native frontends now accept an optional user-owned `ambient_baseline`
transparent composite for that separate set and schedule it at the measured
5.90-second boundary.
The D3D12 native quad path and Vulkan path both emit the recovered 12-slice
atlas block; the unverified procedural sparkle scheduler is suppressed once
that measured block begins, so it cannot contaminate the evidence-backed pass.
The recovered `frames_elements` atlas is also used by the native main-menu
composition for its rounded leather rows and inner rails. Its local keyed PNG
and the converted font remain ignored user-owned UI-root data.
Preserve the asset-free repository boundary and the independently verified
title-card timing.

## Selected menu A ring — evidence binding recovered 2026-08-05

The selected-menu A ring's source binding is now recovered. The current native code
does not claim parity by using the side-rail `Done`/`Cancel` crop or by
compositing the wrong controller asset. The exact retail screenshot reference
is `C:\\Users\\Cornelio\\AppData\\Local\\Temp\\f2_ref_a_menu2.png`.

The bank search found `Art\\GUI\\Frames\\large_ring.tex` (header 1064,
body 746), but decoding proved it is only a thin copper ring and therefore
not the target. The stronger lead is the main-menu BGF’s `MenuHighlight`
component: `rim_and_red`, `green_top`, and `green_top_translucent` all use the
`frames_elements` material. Draw `151304` is `rim_and_red`, using UV
`(0.000,0.171)`-`(0.250,0.687)` and object-space bounds
`x=-8.662528..-7.638528`, `z=0.544000..2.656000`; draws `151305` and `151306`
are `green_top` and `green_top_translucent`, both using UV
`(0.016,0.721)`-`(0.125,0.830)`. The native D3D12 and Vulkan implementations
now emit those three layers in that order. This establishes evidence-backed
source/layer parity for the ring, but full visual parity still requires frame
captures of both native backends against the retail reference. The title prompt’s
standalone `icon_button_a` remains a separate, already-proven asset.

Do not delete the extracted research files under
`ghidra_out/title_ui_re/`; they include the bank headers/bodies and decoded
candidate previews needed for the comparison. The temporary decoder source
is `Fable2Native/tools/decode_large_ring.cpp`.
