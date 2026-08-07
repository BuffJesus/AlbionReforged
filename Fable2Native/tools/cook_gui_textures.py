#!/usr/bin/env python3
"""Cook Fable II frontend GUI textures into a native PC UI package.

The runtime NEVER ships a texture. This offline cooker reads the *user's own* base-game texture
bank (`data/art/gui/gui_textures.bnk`), decodes the frontend `.tex` entries to DDS via the project's
LhTex decoder (`f2native_cook_lh_tex.exe`), and writes them + a `ui_manifest.ini` keyed to the
runtime's NativeUiAsset keys (native_ui.cpp `asset_from_key`).

Format (docs/GUI_TEXTURE_COOK_PLAN.md, cracked 2026-08-07):
- Each `gui_textures.bnk` body is self-contained: inline mip table `{comp(BE u32)@0, offset@4=0x30,
  size@8}` then mip data at 0x30 — exactly what `f2native_cook_lh_tex.exe` reads. So no header/body
  assembly is needed.
- The 84-byte `gui_texture_headers.bnk` header (BE u32) gives width@0x10, height@0x14, and a
  **format code @0x18**: 0x23 = DXT1 (body mip comp=1, cook_lh_tex handles it) · 0x27 = raw/ARGB
  (body comp=7, NOT yet supported — needs lh_decode_variant_2_3_4). This cooker does the comp-1 set.
"""

from __future__ import annotations

import argparse
import struct
import subprocess
import sys
from pathlib import Path

_ASSET_BROWSER_ARCHIVE = (
    Path(__file__).resolve().parents[2] / "Fable2AssetBrowser" / "source" / "Archive"
)

LHTEX_COMP_DXT1 = (1, 11)  # custom-compressed DXT1 (cook_lh_tex decodes; dims from body)
LHTEX_COMP_TILED = 7       # Xbox-360 tiled BCn (cook_lh_tex needs --pf/--width/--height)
LHTEX_TILED_PF = (35, 39)  # supported comp-7 pixel formats: 35 -> DXT1, 39 -> DXT5

# Frontend texture set: bank entry (Art\GUI\...\name.tex) -> (output DDS name, [ui_manifest keys]).
# Keys must match native_ui.cpp asset_from_key(). Entries that are comp-7/raw or absent are skipped
# cleanly (logged) — see the plan doc for the follow-up set (fe_f2logo, ABXY atlas, icons, cards).
FRONTEND_TEXTURES = {
    r"Art\GUI\FrontEnd\Textures\PanoramicF.tex": ("PanoramicF.dds", ["title_background", "main_background"]),
    r"Art\GUI\Frames\frames_page_texture.tex": ("frames_page_texture.dds", ["frames_page_texture"]),
    r"Art\GUI\Frames\motifs.tex": ("motifs.dds", ["motifs"]),
    r"Art\GUI\Frames\frames_elements.tex": ("frames_elements.dds", ["frames_elements"]),
    r"Art\GUI\Frames\ability_elements.tex": ("ability_elements.dds", ["ability_elements"]),
    r"Art\GUI\Frames\frames_04.tex": ("frames_04.dds", ["frames_04"]),
    r"Art\GUI\Frames\CalibrationImage.tex": ("CalibrationImage.dds", ["calibration_image"]),
    r"Art\GUI\Frames\frame_new_left_top.tex": ("frame_new_left_top.dds", ["menu_frame_left_upper"]),
    r"Art\GUI\Frames\frame_new_left_bottom.tex": ("frame_new_left_bottom.dds", ["menu_frame_left_lower"]),
    r"Art\GUI\Frames\frame_new_right_top.tex": ("frame_new_right_top.dds", ["menu_frame_right_upper"]),
    r"Art\GUI\Frames\frame_new_right_bottom.tex": ("frame_new_right_bottom.dds", ["menu_frame_right_lower"]),
    # comp-7 (Xbox-360 tiled) — the footer gold coin (pf39 -> DXT5).
    r"Art\GUI\Controller\24x24\icon_gold_coin.tex": ("icon_gold_coin.dds", ["gold_coin"]),
}


def header_format(hdr: bytes) -> tuple[int, int, int] | None:
    """Return (format_code, width, height) from an 84-byte gui_texture_headers.bnk entry."""
    if len(hdr) < 0x1C or struct.unpack_from(">I", hdr, 0)[0] != 0xFFFFFFFE:
        return None
    width = struct.unpack_from(">I", hdr, 0x10)[0]
    height = struct.unpack_from(">I", hdr, 0x14)[0]
    fmt = struct.unpack_from(">I", hdr, 0x18)[0]
    return fmt, width, height


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("game_dir", type=Path,
                        help="extracted Fable II game dir (or its 'data' dir)")
    parser.add_argument("output_root", type=Path, help="UI package dir receiving *.dds + ui_manifest.ini")
    parser.add_argument("--cooker", type=Path, required=True,
                        help="path to f2native_cook_lh_tex.exe (LhTex .tex -> DDS)")
    parser.add_argument("--archive", type=Path, default=_ASSET_BROWSER_ARCHIVE)
    args = parser.parse_args()

    gui_dir = None
    for rel in (Path("data") / "art" / "gui", Path("art") / "gui"):
        if (args.game_dir / rel / "gui_textures.bnk").is_file():
            gui_dir = args.game_dir / rel
            break
    if gui_dir is None:
        parser.error(f"missing base-game gui_textures.bnk under {args.game_dir}")
    if not args.cooker.is_file():
        parser.error(f"LhTex cooker not found: {args.cooker}")

    sys.path.insert(0, str(args.archive))
    try:
        import bnk_reader  # noqa: E402
    except ImportError as exc:
        parser.error(f"could not import bnk_reader from {args.archive}: {exc}")

    args.output_root.mkdir(parents=True, exist_ok=True)
    tex_reader = bnk_reader.BNKReader(str(gui_dir / "gui_textures.bnk"))
    hdr_reader = bnk_reader.BNKReader(str(gui_dir / "gui_texture_headers.bnk"))
    tex_map = {e.name: e.name for e in tex_reader.file_entries}
    hdr_map = {e.name: e.name for e in hdr_reader.file_entries}

    tmp_body = args.output_root / "_tmp.tex"
    tmp_hdr = args.output_root / "_tmp.hdr"
    manifest: list[tuple[str, str]] = []
    cooked = 0
    for entry, (out_name, keys) in FRONTEND_TEXTURES.items():
        if entry not in tex_map:
            print(f"  SKIP {out_name}: '{entry}' not in gui_textures.bnk")
            continue
        tex_reader.extract_file(tex_map[entry], str(tmp_body))
        body = tmp_body.read_bytes()
        # Decodability is the BODY mip0 comp value, not the 84-byte header's overall format code:
        # e.g. motifs has header pf 0x27 but mip0 comp=1 (DXT1). The header pf is only needed to pick
        # the block codec for comp-7 (Xbox-360 tiled) and for dimension validation.
        comp = struct.unpack_from(">I", body, 0)[0] if len(body) >= 4 else -1
        pf, wh = None, None
        if entry in hdr_map:
            hdr_reader.extract_file(hdr_map[entry], str(tmp_hdr))
            info = header_format(tmp_hdr.read_bytes())
            if info:
                pf, wh = info[0], (info[1], info[2])

        cmd = [str(args.cooker), str(tmp_body), str(args.output_root / out_name)]
        if comp in LHTEX_COMP_DXT1:
            pass  # dims come from the body
        elif comp == LHTEX_COMP_TILED:
            if pf not in LHTEX_TILED_PF or not wh:
                print(f"  SKIP {out_name}: comp-7 pf={pf} unsupported (need header pf 35/39)")
                continue
            cmd += ["--pf", str(pf), "--width", str(wh[0]), "--height", str(wh[1])]
        else:
            print(f"  SKIP {out_name}: body mip0 comp={comp} unsupported")
            continue
        out_path = args.output_root / out_name
        result = subprocess.run(cmd, capture_output=True, text=True)
        if result.returncode != 0 or not out_path.is_file():
            print(f"  SKIP {out_name}: cook_lh_tex failed: {result.stdout.strip()} {result.stderr.strip()}")
            continue
        dims = result.stdout.strip().split(" ")[0]  # "512x512"
        if wh and dims != f"{wh[0]}x{wh[1]}":
            print(f"  WARN {out_name}: decoded {dims} != header {wh[0]}x{wh[1]}")
        for key in keys:
            manifest.append((key, out_name))
        cooked += 1
        print(f"  OK   {out_name} <- gui_textures.bnk:{Path(entry).name} ({dims}) keys={','.join(keys)}")
    tmp_body.unlink(missing_ok=True)
    tmp_hdr.unlink(missing_ok=True)
    tex_reader.close()
    hdr_reader.close()

    # ui_manifest.ini is SHARED with non-texture keys (logo, cards, fonts, ambient frames). MERGE:
    # preserve any existing keys, overlay the texture keys we cooked. Idempotent + non-destructive.
    manifest_path = args.output_root / "ui_manifest.ini"
    merged: dict[str, str] = {}
    if manifest_path.is_file():
        for line in manifest_path.read_text(encoding="utf-8", errors="ignore").splitlines():
            s = line.strip()
            if not s or s[0] in "#;" or "=" not in s:
                continue
            k, v = s.split("=", 1)
            merged[k.strip()] = v.strip()
    for key, name in manifest:
        merged[key] = name
    lines = ["# Textures cooked from the user's own data/art/gui/gui_textures.bnk by cook_gui_textures.py.",
             "# key = NativeUiAsset key (native_ui.cpp asset_from_key); value = cooked dds. No shipped assets."]
    for key, name in merged.items():
        lines.append(f"{key} = {name}")
    manifest_path.write_text("\n".join(lines) + "\n", encoding="utf-8")

    print(f"cooked {cooked}/{len(FRONTEND_TEXTURES)} frontend GUI textures into {args.output_root}")
    return 0 if cooked else 1


if __name__ == "__main__":
    raise SystemExit(main())
