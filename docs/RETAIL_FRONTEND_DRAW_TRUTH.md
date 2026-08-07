# Retail front-end draw truth

This note records observations taken from the retail `frontendmainmenu.bgf`,
the retail GUI banks, and `title_geometry_dump_20260804_shader.txt`. It is an
evidence ledger for the first native front-end pass; it is not a renderer
speculation sheet.

For the complete next-session execution order, including generated-code/ReXGlue tracing,
full frontend state-machine coverage, backend-owned custom settings, and the acceptance loop,
see [`FRONTEND_FULL_PASS_NEXT_SESSION.md`](FRONTEND_FULL_PASS_NEXT_SESSION.md).

## Proven asset assignments

| Retail GPU base | Dimensions/format | Proven content | Assignment |
| --- | --- | --- | --- |
| `0FC47000` | 512x512, format 20 | rounded leather row, B/A prompt, stars, curved frame pieces | `frames_elements` |
| `0FE87000` | 512x512, format 20 | rings, white/green/brown controls, highlight pieces | `frames_04` |
| `0FD07000`/`0FD67000` | 512x512, format 20 | serialized upper/lower left side-panel pieces | left menu-frame border |
| `0FEE7000`/`0FF47000` | 512x512, format 20 | serialized upper/lower right side-panel pieces | right menu-frame border |
| `0FE27000` | 256x1024, format 20 | separate packed prompt/element atlas | not the main side border |

`frontendmainmenu.bgf` names `frames_elements`, `frames_04`, and
`ability_elements` in its `Background`, `Tab`, `InnerBrown`, `InnerRed`, and
border materials. `fe_elements` does not occur in that BGF. The same retail
`fe_elements.tex` body is byte-identical to the extracted local atlas, but the
only frontend BGF consumer found in the GUI script bank is `203_hud.bgf`, where
it is used by the suggested-expression HUD element. It is not a justified
main-menu source.

## Serialized row construction

The first menu-row draw block is sequences `151290`–`151297` in the shader
geometry dump. The row is not one stretched rectangle:

- left cap: `frames_elements` UV `(0.000, 0.004)`–`(0.125, 0.164)`;
- center span: UV `(0.125, 0.004)`–`(0.813, 0.164)`;
- right cap: UV `(0.820, 0.004)`–`(0.945, 0.164)`.

The first row's serialized object-space x positions are `-12.026063` through
`-7.567313`. The cap widths are `0.512500`; the center is `3.433750`. The
second/third row placements preserve those slices while changing z. The row
height is `0.672656`, the regular row step is `0.580000`, and the resulting
overlap is `0.092656`. That overlap is why independently drawn tabs should not
leave a visible gap.

The native paths now emit those three quads in the same left/center/right order,
with the inner brown layer first and the frames-elements rim second. This keeps
the serialized overlap at the texture-composition level instead of introducing
an inset gutter between independently stretched rectangles.

The corresponding inner brown layer is also three-sliced. Sequences
`151263`–`151274` bind the 512x512 ability-elements texture (`0FCA7000`) and
reuse the same horizontal UV bands: left `(0.000, 0.000)`–`(0.125, 0.164)`,
center `(0.125, 0.000)`–`(0.813, 0.164)`, and right `(0.820, 0.000)`–
`(0.945, 0.164)`. Four row bands are visible at z ranges approximately
`-0.445..0.195`, `0.135..0.775`, `0.715..1.355`, and `1.325..1.965`.
This is direct evidence for a paired outer/inner three-slice construction,
not a full-width inner image inset inside a full-width outer image.

## Selected prompt passes

In the same capture, sequences `151305` and `151306` use the same
`frames_elements` UV rectangle `(0.016, 0.721)`–`(0.125, 0.830)` at nearly
identical positions, separated by `0.004` object-space z. This is a two-pass
prompt/edge treatment, not a single glyph pasted over a row. The dump also
contains the `frames_04` tiny highlight rectangle `(0.408, 0.220)`–
`(0.409, 0.221)` with color `00A00000` in sequences `151317`–`151319` and
`151321`; those draws are separate from the button ring and must remain in the
layer order.

The main-menu selected-A presentation is distinct from the title-screen
`Press A` prompt. The title prompt is the standalone
`Art/GUI/Controller/icon_button_a.tex` (`0x0D367000`) and is correctly green
without a gold surround. The settled menu reference shows the green A seated
inside a separate metallic/gold ring. It is not the packed `Done`/`Cancel`
button at the bottom of the `0x0FE27000` / `side_rail_atlas` export, and it is
not the PM4 `dynamic_ring` buffer.

The ring binding is now recovered from the `MenuHighlight` component. Draw
`151304` is `rim_and_red`, using `frames_elements` UV
`(0.000,0.171)`-`(0.250,0.687)` and object-space bounds
`x=-8.662528..-7.638528`, `z=0.544000..2.656000`. The crop contains the
metallic bezel and a red center with transparent margins. Draws `151305` and
`151306` are `green_top` and `green_top_translucent`, both using UV
`(0.016,0.721)`-`(0.125,0.830)` at the centered 46-pixel prompt bounds. The
native D3D12 and Vulkan paths now submit that exact bezel crop first, followed
by the two captured green passes; the red center is therefore covered by the
green A as in retail. This proves the source binding and layer order, not full
pixel parity; backend frame captures against the settled retail screenshot are
still required.

## Side-panel serialization

At the settled menu position, sequences `155724` and `155725` bind the two
left-panel textures. Their projected 1280x720 bounds are approximately
`x=-3..272`, with the upper piece covering `y=-4..514` and the lower piece
covering `y=514..724`. The matching right-panel pair is `0FEE7000` and
`0FF47000`. These are the sprite sources for the side rails. The native path
now submits the four serialized crops directly instead of relying on the older
flattened `menu_frame_overlay.png`; the 256x1024 `0FE27000` atlas is not the
main menu side rail and must not be stretched over the side panels.

## Title ambient baseline boundary

The timed title capture identifies the static reveal as 18 individual 512x512
format-18 textures, followed by the separate atlas/detail pass. The capture
contains the PM4 draw descriptors for those textures, but neither the press-A
capture nor the long capture contains memory records for the 0E367000–0E5A7000
baseline addresses. Their pixels therefore cannot be reconstructed from these
captures alone. The native runtime deliberately keeps `ambient_baseline` as an
optional user-exported precomposite instead of assigning the nearby repeated
0E* scene tiles by guess. A future capture must include the texture memory
dependencies before those 18 layers can be promoted to individual manifest
entries.

## Retail animation constants

The extracted LuaQ scripts provide the timing and slot data independently of
the GPU capture. `expandablemenuformatting.lua` defines `HIGHLIGHT_INDEX=4`,
`NUM_VIS_SLOTS=12`, and `ANIMATE_TIME=0.15`. Its four visible scale/opacity
profiles are `(0.65,0)`, `(0.75,50)`, `(0.85,75)`, and `(1.0,100)`; title sizes
are 18 and truncation widths are 200, 210, 240, and 305. The position table is
the recovered curved rail: x offsets `0,22,42,56,56,56,56,56,50,42,22,0`
and y offsets `85,69,15,-45,-106,-164,-222,-280,-338,-396,-454,-475`.

`expandablemenuanimation.lua` animates position, scale, opacity, title size,
and truncation linearly over the same `0.15 s` interval. Menu initialization
uses `ANIMATE_IN_TIME=0.08` and `ANIMATE_OUT_TIME=0.08`. The native controller
now interpolates the previous and target slot properties over that proven
selection interval on both D3D12 and Vulkan.

The startup BGF separately names `PressStart`, `Legal`, `maiandra gd`, and
`TEXT_SPLASH_SCREEN_LEGAL`; its behaviors emit `CAN_PRESS_A` and `END_LEGAL` on
deactivation. Those identifiers belong to the startup screen and are not
interchangeable with the main-menu `ExpandableMenu` assets.

## Reproducible probe

`tools/retail_frontend_draw_probe.py` prints the serialized base, dimensions,
position, UV rectangle, and color for any draw range. Example:

```text
python tools/retail_frontend_draw_probe.py \
  ghidra_out/title_ui_re/title_geometry_dump_20260804_shader.txt \
  --first 151290 --last 151321
```

No runtime source was changed while establishing these assignments.

## Selected-A ring investigation — 2026-08-05 evidence result

This was the stopping point for the missing gold ring. The native
renderer must not use the packed side-rail button as a substitute. The
following sources are explicitly rejected for the settled main-menu A:

- `0x0FE27000` / the side-rail atlas: its bottom button is the retail
  `Done`/`Cancel` control, not the selected-A bezel.
- `title_pid6208_dynamic_ring.bin`: this is a PM4/ring-buffer capture, not a
  directly viewable BC texture. The generated BC4/ATI1 previews are decode
  noise and must not be shipped as UI art.
- `Art\\GUI\\Frames\\large_ring.tex`: a real bank asset at GUI-header entry
  `1064` and GUI-body entry `746`; its header is 512x512, PixelFormat39/BC3.
  Decoded pixels are a thin copper circular ring. It does not match the thick
  white-highlight/brown-bezel/gold-crescent ring in the retail main-menu
  screenshot, and it is referenced by item/shop screens, not
  `frontendmainmenu.bgf`.

The important positive lead in `frontendmainmenu.bgf` is now confirmed:
`MenuHighlight` contains the groups `rim_and_red`, `green_top`, and
`green_top_translucent`; all three name the `frames_elements` material. Draws
`151304..151306` match that group and its layer order. The same BGF separately
names `AButton`, `ButtonText`, `icon_button_a2`, and `motifs` for other
contexts. The retail screenshot's green A is visually distinct from the
title-screen standalone `icon_button_a` (`0x0D367000`).

Useful bank indices and decoded checks:

| asset | GUI header | GUI body | result |
|---|---:|---:|---|
| `frames_elements.tex` | 112 | 361 | 512x512 atlas; bare green A and B/rim artwork |
| `frames_04.tex` | 1059 | 902 | 512x512 atlas; grey ring/radial FX |
| `frames_02.tex` | 119 | 585 | 256x256 frame/panel atlas |
| `motifs.tex` | 168 | 769 | 512x512 motif atlas; no matching bezel found |
| `received_item_circle_v2.tex` | 129 | 150 | dark item circle; not the menu bezel |
| `received_item_circle.tex` | 166 | 1073 | dark item circle; not the menu bezel |
| `large_ring.tex` | 1064 | 746 | thin copper ring; rejected above |

The code is now evidence-backed for the selected-A bezel: both D3D12 and
Vulkan submit the recovered `rim_and_red` crop followed by the paired
`frames_elements` green-A passes. The side rail remains a panel fallback only,
and `large_ring` remains rejected.

The original first probe was:

```powershell
$p = 'ghidra_out/title_ui_re/title_geometry_dump_20260804_shader.txt'
$a = Get-Content -LiteralPath $p
foreach ($n in 151300..151323) {
  $hit = $a | Select-String -SimpleMatch "DRAW seq=$n "
  if ($hit) { $a[($hit.LineNumber-1)..($hit.LineNumber+18)] }
}
```

That probe produced the recovered mapping above. For each `frames_elements`
draw, the first vertex's VFDATA UV pair and the group's object-space bounds
are sufficient to identify the layer; the exact `rim_and_red` pass precedes
the two green-top passes in the native renderer.

## Menu body material + panorama clip (2026-08-07)

Full evidence dossier: `ghidra_out/title_capture/menu_body_material_RE.txt`.

### Row body material — the solid brown interior

The menu row draws (`title_geometry_dump_20260804_shader.txt` seq `151263..151306`)
all use the **same** shader as the title reveal: VS `6D15306961102F7D`, PS
`4B61E208208F3F5B`. The inner-brown draws bind `tf13` (MAIN) = `0FCA7000`
(`ability_elements`) and `tf14`/`tf15` (DETAIL) = a **1x1 placeholder** at `1FC40000`
(fmt 6) — not a real detail atlas. Decoded PSC constants for those draws (dump line
103414): `c30=(1,1,1,1) c31=(0,0,0,0) c46=(1,0,0,0) c47=(1,0,0,0) c77=(0,0,0,0)
c255=(1,0,0,0)` (identical to the reveal).

Tracing `ps_4B61E208208F3F5B_ucode.txt` with those constants: `c31=0` makes the
predicate `p0=false`, so instruction `2.0 (!p0) jmp L7` **skips the entire `tf14`
detail-combine block**. Therefore the retail menu-row material reduces to:

- `out.rgb = ability_elements.rgb` (folded by the interpolated `r2` vertex tint);
- `out.a  = ability_elements.a * vertexColor.a`;
- RT0 blend `0x07060706` (SrcAlpha / InvSrcAlpha).

**The solid interior comes from the ability_elements texture itself carrying two
vertically-stacked row features** (measured on `live_0FCA7000..._bc3.png`):

- `v 0.000-0.164` = the pill **RIM** only — two thin opaque strips with a **transparent
  interior** (interior alpha ~6, 86% below 20; interior RGB is leather brown ~78,65,42);
- `v 0.188-0.297` = a **SOLID WHITE OPAQUE MASK** (RGB 255,255,255; alpha 255 in the
  core `v 0.20-0.28`). This band is the row **fill/shape mask**.

So the row is: inner-brown/fill layer (ability_elements MAIN sampled at the **opaque
mask band `v≈0.188-0.297`**, giving a solid body) drawn first, then the
`frames_elements` (`0FC47000`) rim second — consistent with the "inner brown first,
rim second" order already recorded above.

**Native translucent-button root cause:** `native_frontend_app.cpp add_body_three_slice`
already samples the mask band (`v 96/512=0.1875 .. 152/512=0.2969`) and does
`out.a = color.a * sampled.a` — the intent is right. But that exact band straddles the
mask's alpha **ramp edges** (`v0.1875` still ramping from 0; `v0.290`=154; `v0.297`≈0),
so linear filtering across the short row pulls transparent top/bottom edges into the
whole body → translucent. **Fix (spec only):** inset to the fully-opaque core
`v0 = 104/512 (0.203)`, `v1 = 143/512 (0.279)` (measured alpha 255). RGB source
(`menu_surface` leather via detail, or `ability_elements.rgb`) is fine; only the sample
band needs the inset. `menu_surface.png` is fully opaque leather (alpha 255 everywhere).

### Panorama clip / occlusion — no scissor, panel-alpha occlusion

There is **no scissor anywhere** — `grep -ci scissor|PA_SC` = 0 in both
`title_geometry_dump_20260804_shader.txt` and `title_capture/reveal_exact_dump.txt`.
The panorama is a **full-width tiled strip** of many 512x512 tiles (the `0E36_7000..
0E5A_7000` series, each ~5.1 obj-units wide, laid edge-to-edge across x — seq
`155629/155639/155646/155653/155643/155650...`), drawn first. It is contained purely by
**alpha occlusion**: the side panels (seq `155724`=`0FD67000` L-lower, `155725`=
`0FD07000` L-upper; right mirror `0FEE7000`/`0FF47000`) are drawn on top, opaque from
the screen edge inward to a **curved inner edge**. Measured opaque inner edge (alpha>128)
on the panel textures: L-upper opaque `u 0.000 → ~0.445 (top) / ~0.482 (mid) / ~0.477
(bottom)`; R-upper mirror `u ~0.03/0.09 → 0.529`. Mapping through the draw-truth panel
projection (L panel screen `x=-3..272`, width 275): the panorama's visible inner window
is roughly `x ∈ [~128, ~1152]` at mid-height, **curving inward toward top and bottom** —
a barrel window, not a straight rect.

**Fix (spec only):** draw the panorama full-frame, then composite the four serialized
panel crops (`0FD07000/0FD67000` L, `0FEE7000/0FF47000` R) OVER it with their **real
curved alpha** — that crops the panorama exactly as retail, no scissor. The overflow bug
means the panels are not being drawn over the panorama with their authored alpha (or the
panorama is drawn wider than they cover).

### GAPs
- The 2026-08-04 capture caught the menu mid-fade (row vertex-alpha `0x00` on nearly all
  draws), so it has no fully-opaque settled-menu frame; the "solid body" result is
  derived from the material equation + the ability_elements mask band, not an opaque-frame
  screenshot. A settled-menu capture (rows at opacity 100 → vertex alpha `0xFF`) would
  visually confirm. Not required to implement either fix.
- Curved panel inner-edge screen-x values assume linear-u across the panel quad; for
  sub-pixel border fidelity read the seq `155724/155725` VFDATA UV0 rect directly.
