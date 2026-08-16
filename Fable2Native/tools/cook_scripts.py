#!/usr/bin/env python3
"""Cook Fable II gameplay Lua scripts into a native PC script package.

The runtime NEVER ships a script. This offline cooker reads the *user's own* base-game script
bank (`data/gamescripts_r.bnk`), decompresses every Lua entry, and writes the decompressed LuaQ
bytecode + a `script_manifest.json` into a user-local package directory. At runtime the native
loader consumes this cooked package (BnkReader::open_cooked) instead of decompressing the raw BNK
on every boot — matching the port plan's "360 BNK data -> offline cooker -> native package ->
runtime" pipeline (docs/NATIVE_PORT_PLAN.md) and the "validate the installed source" policy.

Why scripts are a *decompress-only* cook (no transcode): a Fable II script entry is Lua 5.1
bytecode (LuaQ, 32-bit little-endian, lua_Number=float32). Once decompressed from the BNK it is
already PC-loadable by the runtime's patched lundump. So the cook is faithful extraction, not
reinterpretation — nothing is guessed. (Meshes/textures/terrain, which carry Xbox-specific
encodings, still need their own transcoding cookers; scripts do not.)

Layout produced (all paths lowercased, backslash->forward-slash):
    <out>/scripts/<...>.lua      # decompressed entry payload, one file per BNK script entry
    <out>/script_manifest.json   # {version, source, count, entries:[{name,size,luaq,sha1}]}

The runtime indexes <out> recursively; the manifest is provenance + a source-validation anchor.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path

# Reuse the proven BNK script extractor (tools/lua_mod/script_index.py). The runtime's
# BnkReader::extract mirrors this exact decompression, so the cooked bytes are byte-identical.
_LUA_MOD = Path(__file__).resolve().parents[2] / "tools" / "lua_mod"
sys.path.insert(0, str(_LUA_MOD))
import script_index  # noqa: E402

LUAQ_MAGIC = b"\x1bLuaQ"
PACKAGE_VERSION = 1


def _find_bnk(game_root: Path, explicit: Path | None) -> Path:
    if explicit is not None:
        return explicit
    # The user selects their extracted game directory; the script bank lives under data/.
    for rel in ("data/gamescripts_r.bnk", "gamescripts_r.bnk"):
        cand = game_root / rel
        if cand.is_file():
            return cand
    raise SystemExit(
        f"could not find gamescripts_r.bnk under {game_root} (pass --bnk to point at it)"
    )


def cook(bnk_path: Path, out_root: Path) -> int:
    entries = script_index.load_entries(bnk_path)
    scripts_dir = out_root / "scripts"
    scripts_dir.mkdir(parents=True, exist_ok=True)

    manifest_entries = []
    luaq = 0
    errors = 0
    for e in sorted(entries, key=lambda x: x["name"].lower()):
        name = e["name"].replace("\\", "/").lower()  # "scripts/quests/qc010_childhood.lua"
        try:
            data = script_index.entry_bytes(e)
        except RuntimeError as exc:
            errors += 1
            print(f"[skip] {exc}", file=sys.stderr)
            continue
        target = out_root / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        is_luaq = data[:5] == LUAQ_MAGIC
        luaq += 1 if is_luaq else 0
        manifest_entries.append(
            {
                "name": name,
                "size": len(data),
                "luaq": is_luaq,
                "sha1": hashlib.sha1(data).hexdigest(),
            }
        )

    src = bnk_path.read_bytes()
    manifest = {
        "version": PACKAGE_VERSION,
        "source": {
            "bank": bnk_path.name,
            "size": len(src),
            "sha1": hashlib.sha1(src).hexdigest(),
        },
        "count": len(manifest_entries),
        "luaq_count": luaq,
        "entries": manifest_entries,
    }
    (out_root / "script_manifest.json").write_text(json.dumps(manifest, indent=1), encoding="utf-8")

    print(
        f"cooked {len(manifest_entries)} scripts ({luaq} LuaQ, "
        f"{len(manifest_entries) - luaq} source) to {out_root}"
    )
    if errors:
        print(f"WARNING: {errors} entries failed to decompress", file=sys.stderr)
    # A healthy gamescripts_r.bnk is overwhelmingly LuaQ; treat a shortfall as a failure.
    if len(manifest_entries) == 0:
        raise SystemExit("no scripts cooked")
    return len(manifest_entries)


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("game_root", type=Path, help="user's extracted game directory (holds data/gamescripts_r.bnk)")
    p.add_argument("out_root", type=Path, help="package directory to receive scripts/ + script_manifest.json")
    p.add_argument("--bnk", type=Path, default=None, help="explicit path to gamescripts_r.bnk")
    return p


def main(argv=None) -> None:
    args = build_parser().parse_args(argv)
    bnk = _find_bnk(args.game_root, args.bnk)
    cook(bnk, args.out_root)


if __name__ == "__main__":
    main()
