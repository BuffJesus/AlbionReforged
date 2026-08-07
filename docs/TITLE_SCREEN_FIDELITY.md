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
  each named `Maiandra GD Fable` with 1,894 glyphs. A user-owned TTF export is
  now accepted by both native frontends; automatic authored wrapping and
  glyph-bound placement remain open.
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
  and an RGBA8 value at `17`. The original capture recorded only guest fetch
  addresses; the later live sidecar recovered the corresponding vertex/index
  memory, exact UV rectangles, positions, and per-sprite constants.
- Draw sequences `95396..95399` are four text draws using `0x1A93D000`; this
  confirms the earlier rejection of that texture as the blue reveal effect.

## 2026-08-04 live geometry evidence

The external Xenos probe now supports an opt-in shader-filtered sidecar dump
(`FABLE2_PM4_GEOMETRY_DUMP` plus
`FABLE2_PM4_GEOMETRY_DUMP_SHADER=6D15306961102F7D`). It records the draw
metadata, constants, vertex/index memory, and texture descriptors without
requiring the full PM4 prelude to fit in a replay capture. A local run recovered
39,273 matching indexed draws with successful vertex and index reads for every
draw.

The recovered pixel shader uses UV0 for `tf13`, the main 512x512 format-20
atlas at `0x0FBE7000`, and UV1 for `tf14`, the changing format-18 detail
texture. The representative draw's relevant constants are `c30=(1,1,1,1)`,
`c31=(1,0,0,0)`, `c46=(1,0,0,0)`, `c47=(1,0,0,0)`, and `c77=(0,0,0,0)`.
With those constants, the material's active color path reduces to
`out.rgb = detail.rgb * c47.x + detail.a * main.rgb` and
`out.a = main.a * detail.a`, followed by the verified source-alpha blend.
The native UI asset loader accepts an optional same-sized `ambient_detail`
export and folds this equation into the user-owned atlas before either backend
uploads it. This gives both native backends the recovered material result for
the supplied detail snapshot. A manifest-provided `ambient_detail_frames`
sequence now streams those composed atlas frames at 60 Hz; the exact retail
frame inventory remains dependent on user-exported source data.

The version-2 timed sidecar adds swap markers. In the live run the atlas block
starts at frame 792 with 18 baseline draws; 12 additional draws (three
four-draw atlas slices) appear at frame 794, move through frame 803, and the
30-draw block repeats unchanged through frame 912. The moving UV rectangles
are `0.740..0.865`, `0.871..0.873`, and `0.875..1.000` in U, all over
`0.250..0.500` in V. This closes the atlas-rectangle and live-geometry timing
recovery; the native path now consumes these slices when the user supplies the
optional atlas. A same-sized `ambient_detail` snapshot is combined with the
atlas using the recovered equation; `ambient_detail_frames` can provide the
changing detail sequence at the measured cadence.

## Current native implementation

The D3D12 and Vulkan frontends load only user-selected/cooked UI assets. When
`ambient_atlas` is supplied, the title ambient layer uses the recovered
three-slice atlas geometry and timing; D3D12 emits it through the native quad
renderer and Vulkan uses the shared geometry helper. The unverified procedural
sparkle scheduler is retained only before that measured block begins; otherwise
the title keeps the procedural fallback. The 18 static draws are a separate source set: the timed capture
binds 18 individual 512x512 format-18 textures for them, not additional
rectangles in `ambient_atlas`. The optional user-owned `ambient_baseline`
input provides a transparent precomposite for that static set and is drawn at
the measured 5.90-second boundary. The available press-A and long PM4 captures
contain the baseline draw descriptors but no memory records for the 0E* source
addresses, so their pixels are not recoverable from the checked-in captures.
The runtime intentionally does not assign the nearby repeated scene tiles to
the baseline by guess. No game art is shipped in the repository.
The manifest also accepts a user-converted `title_font` TTF/OTF; both native
frontends install it as the ImGui default when present. The SWF-to-vector/font
conversion itself remains an external user-data preparation step.

## Remaining gate items

## Claim status

“Asset-free” is currently satisfied for the tracked repository: game-art,
font, texture, and extracted retail binary assets are not checked in. Local
user-owned UI exports and research helpers remain outside that boundary.
“Parity” is not yet satisfied globally. The recovered ring binding, layer
order, title timing, and optional atlas path are evidence-backed, but full
pixel parity still requires the 18 baseline textures, authored legal wrapping,
and synchronized D3D12/Vulkan frame captures against the retail reference.

1. The user-owned `Maiandra GD Fable` TTF export is now present at the local
   manifest's `title_font` path and is accepted by both frontends. Measured
   glyph bounds still need to drive prompt and legal wrapping automatically;
   the repository continues to ship no font or game art.
2. The `95346..95395` geometry, constants, atlas rectangles, and frame-relative
   timing are now recovered from the live sidecar and promoted as an optional
   user-owned three-slice atlas path in both native frontends. The recovered
   dual-texture equation is applied for `ambient_detail` or the streamed
   `ambient_detail_frames` sequence; the baseline texture set still requires
   its user-provided data.
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

## Effect RE 2026-08-07

This pass resolved the reveal geometry/animation model from the checked-in
sidecar dumps and corrected two prior framings. Evidence files:
`ghidra_out/title_ui_re/title_reveal_geometry_re.txt`,
`title_sparkle_logic.txt`, `title_reveal_frame_timeline.txt`,
`title_reveal_frame794_verts.txt`, `frontend_lua_full_disasm.txt`,
`ghidra_out/title_effect_render_re.txt` (PPC submit-path trace), and the
decoder `tools/decode_title_reveal_geometry.py`. Turnkey capture harness for
the missing reveal instant: `ghidra_out/title_capture/`.

1. **Reveal vertex data is closed (was listed as an open gap).** The reveal
   sprite VB *and* IB contents are present in
   `title_geometry_dump_20260804_shader.txt` (v1) and `..._timed2.txt` (v2,
   SWAP-frame timed). Per-vertex position, UV0 (atlas), UV1 (detail), passthrough
   float, and RGBA8 color are all recoverable per draw with the new decoder. The
   18-dword layout matches `vs_6D15306961102F7D_ucode.txt`; index buffer is the
   fixed quad `[1,0,2,2,3,1]`.

2. **Vertex-data source = procedural, per-frame, native.** Tracking one sprite
   across consecutive SWAP frames shows UV0/UV1 held constant while POSITION
   moves along a decelerating (ease-out) slide and then settles (~10 frames).
   The game regenerates a fresh vertex buffer each frame from the UI element's
   interpolated position — it is not a static VB and not a Lua particle emitter.

3. **`StarVel`/`Time` are declared-but-unused in the shipped `frontend.lua`.**
   `FrontEnd.Initialize` sets `Time/ButtonY/StarVel = 0`; across the whole
   script only `ButtonY` is ever read again (it drives the green-A indicator
   button's Y in `SelectTitleItem`). There is no per-frame sparkle update, no
   velocity/gravity/lifetime, and no other GUI script references `StarVel`. The
   earlier "Time/ButtonY/StarVel = star motion" framing is superseded: the
   procedural sparkle scheduler in the native frontend has no retail-script
   basis and remains unverified.

4. **The `pm4_title_geometry_20260804_*` timed sidecars are the MAIN MENU, not
   the black→panorama reveal.** In their frames 792–912 the atlas set is the
   full menu-frame set (`frames_elements`, `ability_elements`, side rails, and
   the 18 format-18 leather/parchment detail surfaces) with menu-tint vertex
   colors — i.e. the ExpandableMenu animating in. This revises the prior
   interpretation of "frames 792–912" as the reveal.

5. **Blue bubble-burst — still a GAP, but scoped.** The atlas `0x0FBE7000`
   (slice map in `title_reveal_geometry_re.txt`) contains grey/white shape
   masks: a **radial sphere** (`u[0.75..1.0] v[0.0..0.25]`) and a **radial
   gradient corner** (`u[0.5..1.0] v[0.5..1.0]`) that are the plausible burst
   sources — they would be tinted blue via RGBA8 vertex color, since the tf14
   detail textures are only leather/parchment surface fills. No blue-tinted
   draw and no dedicated blue texture appears in any checked-in capture, because
   none captures the reveal instant. Verdict: (a) most likely a grey atlas
   radial slice × a blue vertex color, not a separate texture — **UNRESOLVED**
   until a reveal-moment capture confirms the slice + color.

6. **What the user must capture to finish it:** a RenderDoc `.rdc` of the
   black→panorama reveal instant (the 1.1 s fade after the 5.9 s hold), under
   `--gpu_plugin xenos`. Turnkey harness + step-by-step:
   `ghidra_out/title_capture/README_CAPTURE_STEPS.md` and
   `extract_title_reveal.py` (dumps each reveal draw's VB/IB + bound textures,
   filtered by VS `6D15306961102F7D` / PS `4B61E208208F3F5B`).
