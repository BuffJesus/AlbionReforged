# GUI texture cook plan — base-game-sourced UI textures for Fable2Native

> **STATUS 2026-08-07 — SHIPPED (comp-1 AND comp-7 frontend sets).** `tools/cook_gui_textures.py` +
> `native_cook.cpp` texture step + installer `--no-textures`. Cooks **12/12** frontend textures from
> the user's `gui_textures.bnk` → DDS + a MERGED `ui_manifest.ini`, via `f2native_installer` (build +
> ctest green). Branch is on the **body mip0 `comp`**: comp 1/11 → `cook_lh_tex` custom-DXT1;
> **comp 7 → Xbox-360 tiled BCn** — `cook_lh_tex` now untiles + endian-swaps (ported from the
> AssetBrowser decoder) and, using the header pixel-format, writes **pf35→DXT1 / pf39→DXT5**
> (`icon_gold_coin` = 32×32 DXT5, verified). comp-7 pf40 (BC5) still skipped (runtime loads DXT1/DXT5
> only). REMAINING: `fe_f2logo`/seasonal panorama + cards/sparkles/fonts (NOT in `gui_textures.bnk` —
> likely `gui_streaming.bnk`); the cooker MERGES `ui_manifest.ini` so those add incrementally.
> ⚠ Pixel-correctness of the untile is by verbatim port; confirm visually on a live run.

**Created 2026-08-07.** Goal: make the native frontend's UI textures **cooked from the user's base
game** (ship nothing), mirroring the shipped GUI-audio pipeline (`tools/cook_gui_audio.py`, spec §8),
and wire it into the installer cook step (`native_cook.cpp` → `cook_native_package`). This is the
foundation of frontend visual parity (docs/FRONTEND_FULL_PASS_NEXT_SESSION.md, RETAIL_FRONTEND_DRAW_TRUTH.md).
Today the `title_ui_re` UI textures are a mix of hand-produced PNG/DDS with unclear provenance — this
plan replaces that with a reproducible cooker.

## Format findings (verified 2026-08-07)
- **`data/art/gui/gui_textures.bnk`** — 1079 NAMED `.tex` entries (LhTex pixel/mip bodies), e.g.
  `Art\GUI\Frames\frames_page_texture.tex`, `...\motifs.tex`, `...\frames_elements.tex`.
  Read via AssetBrowser `Archive/bnk_reader.py` `BNKReader.extract_file(name,out)`.
- **`data/art/gui/gui_texture_headers.bnk`** — 1167 entries, **84 bytes each** = the per-texture
  metadata header. **FORMAT CRACKED 2026-08-07** (big-endian u32; verified across CalibrationImage,
  frames_page_texture, PanoramicF, motifs, icon_gold_coin):

  | off | u32 field | notes |
  |---|---|---|
  | 0x00 | magic `0xFFFFFFFE` | sentinel |
  | 0x04 | total body/data size | == the gui_textures.bnk body size |
  | 0x08 | reserved (0) | |
  | 0x0C | flags | `0x10` opaque DXT1 / `0xb0` alpha-or-raw |
  | 0x10 | **width** | 512, 32, … |
  | 0x14 | **height** | |
  | 0x18 | **format code** | **`0x23` = DXT1 (body mip comp=1)** · `0x27` = raw/ARGB (body comp=7, e.g. icons) |
  | 0x1C | depth/array = 1 | |
  | 0x20 | header size = `0x54` (84) | |
  | 0x50 | tail | 0 for fmt 0x23, 2 for fmt 0x27 |

- **★ KEY: the BODY is self-decodable — the header is NOT required to decode.** Each
  `gui_textures.bnk` body starts with an inline mip table `{comp(BE u32)@0, offset@4=0x30, size@8}`
  (up to 4 mips) then mip data at 0x30 — exactly what `cook_lh_tex.cpp` already reads (`comp@0`,
  `size@8`, `mip@48`). **VERIFIED: `f2native_cook_lh_tex.exe` decodes the CalibrationImage /
  frames_page_texture / PanoramicF / motifs bodies straight from the bank to correct-dimension
  512×512 DDS** (matching the header W/H). The header is only needed to (a) branch on format
  (comp1 vs comp7) without decoding, and (b) validate decoded dims. ⇒ the split header/body is NOT a
  blocker; no header/body "assembly" is needed for comp-1 textures.
- Body `comp` values seen: **1 = DXT1** (cook_lh_tex ✓), **3** (motifs mip1, small), **7 = raw/ARGB**
  (icons — `cook_lh_tex` rejects `comp=7`; needs `lh_decode_variant_2_3_4`). The frontend
  page/panorama/frame/motif textures are all **comp 1**, so they cook today.
- **Decoder available:** `Fable2AssetBrowser/source/src/textures/`:
  - `LhTexCodec.h`: `lh_decode_compressed_mip(body, size, &w, &h, out_bc1, err, comp11)` → BC1/DXT1
    (comp flag 1 or 11), returns width/height. `lh_decode_variant_2_3_4(...)` for other modes.
    Already wrapped standalone by `Fable2Native/tools/cook_lh_tex.cpp` (→ DDS) and built as
    `f2native_cook_lh_tex.exe`.
  - `textures/export/TextureExport.{h,cpp}`: `decode_tex_to_rgba(blob, rgba, w, h, &has_alpha, mip)`
    + `tex_export_begin_named(fmt, tex_name, preferred_bnk, mip)` = the full **named-tex-from-bnk →
    RGBA → PNG/DDS** path that ALREADY handles the header/body combine + bnk lookup. BUT it is woven
    into the ImGui app (global export queue `tex_export_drive`, app bnk state) — not a clean CLI.

## Implementation options
- **(A) Batch bnk-aware C++ cooker (recommended first cut).** Extend `cook_lh_tex.cpp` into
  `f2native_cook_gui_tex` that: loads `gui_textures.bnk` + `gui_texture_headers.bnk` (port the small
  bnk_reader logic to C++, or reuse via a py sidecar), parses the 84-byte header to get
  width/height/format/mip layout, assembles the body into what `lh_decode_compressed_mip` /
  `lh_decode_variant_2_3_4` expects, decodes the FRONTEND set to DDS/PNG, and writes `ui_manifest.ini`.
  Needs: **RE the 84-byte header format** (the one real unknown — dump several headers, correlate with
  known texture dims like the 1024×512 `fe_f2logo`).
- **(B) Carve `decode_tex_to_rgba` + its bnk load out of AssetBrowser into `libf2data`.** Cleaner
  long-term (the plan's format boundary) but a bigger refactor (decouple from app globals).
- **(C) Python sidecar** mirroring `cook_gui_audio.py`: `bnk_reader` extracts header+body, a Python
  LhTex decoder decodes. BLOCKER: no Python LhTex decoder exists (audio had `bnk_reader`+ffmpeg; tex
  decode is C++ only). Would require porting LhTexCodec to Python — more work than (A).

## Frontend texture set → NativeUiAsset key map (concrete)
Bank entries (all in `gui_textures.bnk`, `Art\GUI\...`) → `ui_manifest.ini` keys consumed by
`native_ui.cpp asset_from_key()` / `NativeUiAsset`:
| Bank entry (.tex) | ui_manifest key(s) | NativeUiAsset |
|---|---|---|
| `FrontEnd\Textures\fe_f2logo` | `logo` | Logo |
| `FrontEnd\Textures\PanoramicF` (+ seasonal) | `title_background`/`main_background` | TitleBackground/MainBackground |
| `Frames\frames_page_texture` | `frames_page_texture` | (options page parchment) |
| `Frames\frames_elements` | `frames_elements` | FrameElements |
| `Frames\ability_elements` | `ability_elements` | AbilityElements |
| `Frames\frames_04` | `frames_04` | Frames04 |
| `Frames\motifs` | `motifs` | MenuFrameOverlay/motif |
| `Frames\acceptancebox` | (accept box) | — |
| `Frames\CalibrationImage` | `calibration` | (options video page) |
| `Frames\frame_new_{left,right}_{top,bottom}` | page side frames | — |
| `Controller\...\icon_gold_coin` | `gold_coin` | (options footer coin) |
| `sliderframe` (guiscripts.bnk / gui banks) | `slider` | — |
| ABXY atlas `live_0D3F7000_..._100000000.dds` | `accept`/`back` | Accept/Back |
Confirm each against docs/FRONTEND_FULL_PASS_NEXT_SESSION.md "Proven texture/material sources" and
RETAIL_FRONTEND_DRAW_TRUTH.md (3-slice UV bands, `MenuHighlight` layers, side rails
`0FD07000/0FD67000` L + `0FEE7000/0FF47000` R).

## Wire into installer + runtime
- Add a `cook_gui_textures` step to `native_cook.cpp cook_native_package()` (alongside videos/audio),
  writing `<package>/*.dds|png` + `<package>/ui_manifest.ini`. Runtime already reads `ui_manifest.ini`
  from `--ui-root` (= the package).
- Ship nothing: repo carries only the cooker; the DDS/PNG come from the user's `gui_textures.bnk`.

## Verification
1. Decoded DDS dimensions match the `gui_texture_headers.bnk` header for each entry (structural).
2. Numeric compare rendered frontend states vs `resources/options/*.png` + title captures (visual).
3. Keyed-alpha entries (color-key, not DXT1 alpha) handled — some `_keyed` textures need the key color
   applied (see existing `ability_elements_keyed.png` in title_ui_re).

## Simplified implementation (post-crack — option A is now small)
The cooker mirrors `cook_gui_audio.py`:
1. `bnk_reader` extracts each frontend body by name from `gui_textures.bnk` (no header assembly needed).
2. Read the 84-byte header (same name in `gui_texture_headers.bnk`) → width/height + **format code**.
3. If `fmt == 0x23` (comp1/DXT1): invoke `f2native_cook_lh_tex.exe body out.dds` → DDS (verified path).
   If `fmt == 0x27` (comp7 raw): decode via `lh_decode_variant_2_3_4` (needs a small standalone wrapper) → PNG/DDS.
4. Validate decoded dims == header W/H; write `ui_manifest.ini` (key → file) into `<package>`.
5. Add a `cook_gui_textures` step to `native_cook.cpp cook_native_package()` (alongside videos/audio).

## Open unknowns to close first
1. ~~The 84-byte header format~~ **CLOSED 2026-08-07** (table above) — and the body is self-decodable,
   so this was not even a hard gate.
2. The `FrontEnd\Textures\` entries — `PanoramicF.tex` confirmed (512×512, comp1). `fe_f2logo` is NOT in
   `gui_textures.bnk`; find it (likely `gui_streaming.bnk` or a different name) + its seasonal panorama set.
3. Color-key vs DXT-alpha per frontend entry (flags `0x10` vs `0xb0`; `_keyed` entries need key-color applied).
4. A small `f2native_cook_gui_tex` wrapper exposing `lh_decode_variant_2_3_4` for comp-7 (icons/coin).
