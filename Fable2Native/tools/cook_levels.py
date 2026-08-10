#!/usr/bin/env python3
"""Cook a Fable II level's static geometry manifest from its engine_level file.

Offline install-time cooker (like cook_videos.py / cook_mdl.py): reads the real
`*.engine_level` (the `LevelGraphicsFile` container) and parses the prop blocks
(model paths + instance transforms). This is a FAITHFUL port of AssetBrowser's
byte-exact `Level::ParseEngineLevel` (src/Level/LevelLoader.cpp) — the parser the
world-format decomp (ghidra_out/world_level_format.txt) confirmed matches the
guest reader 0x82ABAEF0. It is a translation of the proven algorithm, not a
re-derivation.

Stage 1 (this file): parse + emit a manifest of the real props. Stage 2 will
cook each prop MDL (via cook_mdl.py / fable_mdl_format) and merge instances into
one .f2scene. The native runtime never imports this — it loads the cooked file.

Usage:
    cook_levels.py <path-to.engine_level> [--json out.json]
"""

from __future__ import annotations

import argparse
import json
import math
import struct
from pathlib import Path


class BeReader:
    """Big-endian byte reader — an exact port of LevelLoader.cpp's BeReader."""

    def __init__(self, data: bytes):
        self.p = data
        self.n = len(data)
        self.i = 0

    def need(self, k: int) -> bool:
        return self.i + k <= self.n

    def u8(self):
        if not self.need(1):
            return None
        v = self.p[self.i]
        self.i += 1
        return v

    def u32(self):
        if not self.need(4):
            return None
        v = (self.p[self.i] << 24) | (self.p[self.i + 1] << 16) | (self.p[self.i + 2] << 8) | self.p[self.i + 3]
        self.i += 4
        return v

    def u64(self):
        hi = self.u32()
        lo = self.u32()
        if hi is None or lo is None:
            return None
        return (hi << 32) | lo

    def f32(self):
        u = self.u32()
        if u is None:
            return None
        return struct.unpack(">f", struct.pack(">I", u))[0]

    def half(self):
        if not self.need(2):
            return None
        h = (self.p[self.i] << 8) | self.p[self.i + 1]
        self.i += 2
        return struct.unpack(">e", struct.pack(">H", h & 0xFFFF))[0]

    def skip(self, k: int) -> bool:
        if not self.need(k):
            return False
        self.i += k
        return True

    def cstr(self):
        start = self.i
        limit = min(self.n, start + 4096)
        out = bytearray()
        while self.i < limit:
            c = self.p[self.i]
            self.i += 1
            if c == 0:
                return out.decode("latin-1")
            out.append(c)
        return None


class ParseError(Exception):
    pass


def parse_engine_level(data: bytes) -> dict:
    """Faithful port of Level::ParseEngineLevel. Returns {version, entry_count, prop_blocks}."""
    MAGIC = b"LevelGraphicsFile"
    if len(data) < len(MAGIC) + 8 or data[: len(MAGIC)] != MAGIC:
        raise ParseError("not a LevelGraphicsFile")
    r = BeReader(data)
    r.skip(len(MAGIC))
    version = r.u32()
    entry_count = r.u32()
    if version is None or entry_count is None:
        raise ParseError("truncated header")
    if version < 11 or version > 12:
        raise ParseError(f"unsupported version {version}")

    prop_blocks = []
    entry_types = {}
    for mi in range(entry_count):
        offset = r.i
        etype = r.u32()
        if etype is None:
            raise ParseError(f"truncated at entry {mi}/{entry_count}")
        entry_types[etype] = entry_types.get(etype, 0) + 1

        if etype == 2:
            model_path = r.cstr()
            shadow = r.cstr()
            lod = r.cstr()
            extra = r.cstr()
            if None in (model_path, shadow, lod, extra):
                raise ParseError("truncated type-2 model paths")
            count = r.u32()
            if count is None or count > 100000:
                raise ParseError("type-2 instance count corrupt")
            insts = []
            for _ in range(count):
                f0, f1, f2 = r.u8(), r.u8(), r.u8()
                h = r.u64()
                if None in (f0, f1, f2, h):
                    raise ParseError("truncated type-2 instance header")
                values = [r.f32() for _ in range(20)]
                if any(v is None for v in values):
                    raise ParseError("truncated type-2 instance floats")
                insts.append({"pos": values[0:3], "values": values})
            prop_blocks.append({"kind": 2, "model": model_path, "lod": lod, "instances": insts})

        elif etype in (4, 5, 32):
            s = r.cstr()
            if s is None:
                raise ParseError(f"truncated string for type {etype}")
            if etype == 4 and not r.skip(8):
                raise ParseError("truncated type-4 tail")

        elif etype == 21:
            str_a = r.cstr()
            str_b = r.cstr()
            if str_a is None or str_b is None:
                raise ParseError("truncated type-21 strings")
            if not r.skip(8 + 1 + 1):
                raise ParseError("truncated type-21 hash+flags")
            loop1 = r.u32()
            if loop1 is None or not r.skip(7 * 4 + 12 + 4 + 24) or loop1 > 100000:
                raise ParseError("truncated/corrupt type-21 ext header")
            insts = []
            if version == 11:
                for _ in range(loop1):
                    vals = [r.f32() for _ in range(4)]
                    if any(v is None for v in vals):
                        raise ParseError("truncated type-21 v11 loop1")
                    insts.append({"pos": vals[0:3], "scale": 1.0})
            else:
                for _ in range(loop1):
                    pos = [r.f32(), r.f32(), r.f32()]
                    qx, qy, qz, qw, scale = (r.half(), r.half(), r.half(), r.half(), r.half())
                    if None in pos or None in (qx, qy, qz, qw, scale):
                        raise ParseError("truncated type-21 v12 loop1")
                    num = 2.0 * (qw * qz + qx * qy)
                    den = 1.0 - 2.0 * (qy * qy + qz * qz)
                    mag = math.sqrt(num * num + den * den)
                    yaw = (num / mag, den / mag) if mag > 1e-6 else (0.0, 1.0)
                    s = scale if scale > 0.0 else 1.0
                    insts.append({"pos": pos, "yaw_sin_cos": yaw, "scale": s})
            loop2 = r.u32()
            if loop2 is None or loop2 > 100000:
                raise ParseError("truncated/corrupt type-21 loop2 count")
            for _ in range(loop2):
                a_val, b_val = r.f32(), r.f32()
                p1 = [r.f32(), r.f32(), r.f32()]
                p2 = [r.f32(), r.f32(), r.f32()]
                if None in (a_val, b_val) or None in p1 or None in p2:
                    raise ParseError("truncated type-21 loop2 body")

                def in_bounds(v, lo, hi):
                    return math.isfinite(v) and lo <= v <= hi

                plausible = (
                    in_bounds(p1[0], -2048.0, 2048.0)
                    and in_bounds(p1[1], -2048.0, 2048.0)
                    and in_bounds(p1[2], -512.0, 512.0)
                    and (abs(p1[0]) + abs(p1[1]) + abs(p1[2]) > 0.5)
                )
                if not plausible:
                    continue
                fxy = math.sqrt(p2[0] * p2[0] + p2[1] * p2[1])
                yaw = (p2[1] / fxy, p2[0] / fxy) if (math.isfinite(fxy) and 0.001 < fxy < 100.0) else (0.0, 1.0)
                s = a_val if (math.isfinite(a_val) and 0.05 < a_val < 100.0) else 1.0
                insts.append({"pos": p1, "yaw_sin_cos": yaw, "scale": s})
            if insts:
                prop_blocks.append({"kind": 21, "model": str_a, "lod": str_b, "instances": insts})

        else:
            # Unknown type: the C++ parser bails here (returns what it has). Match it.
            break

    return {"version": version, "entry_count": entry_count, "entry_types": entry_types,
            "prop_blocks": prop_blocks}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("engine_level", type=Path, help="path to a *.engine_level file")
    ap.add_argument("--json", type=Path, help="write the full manifest as JSON")
    args = ap.parse_args()

    data = args.engine_level.read_bytes()
    info = parse_engine_level(data)

    total_inst = sum(len(b["instances"]) for b in info["prop_blocks"])
    print(f"LevelGraphicsFile v{info['version']}  entries={info['entry_count']}  "
          f"entry_types={info['entry_types']}")
    print(f"prop_blocks={len(info['prop_blocks'])}  total_instances={total_inst}")
    for b in info["prop_blocks"][:20]:
        first = b["instances"][0]["pos"] if b["instances"] else []
        print(f"  [type {b['kind']:>2}] {len(b['instances']):>5} x  {b['model']}"
              + (f"   first_pos={[round(v, 2) for v in first]}" if first else ""))
    if len(info["prop_blocks"]) > 20:
        print(f"  ... and {len(info['prop_blocks']) - 20} more prop blocks")

    if args.json:
        args.json.write_text(json.dumps(info, indent=1), encoding="utf-8")
        print(f"-> {args.json}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
