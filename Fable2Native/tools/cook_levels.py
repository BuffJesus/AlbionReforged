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
import gzip
import json
import math
import struct
from pathlib import Path

# Terrain heightfield constants (ghidra_out/terrain_mesh_re.txt, validated byte-exact):
# tile spacing is a FIXED engine constant (0.5 wu/cell), NOT the .ghf header float;
# UV repeats every 8 wu (kUvRepeatsPerWu = 0.125).
TERRAIN_TILE = 0.5
TERRAIN_UV_PER_WU = 0.125


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


def _norm(p: str) -> str:
    """Lowercased, forward-slashed bnk key (matches ModelParser.cpp:35 normalisation)."""
    return p.lower().replace("\\", "/")


def _bnk_name_index(bnk_path: Path):
    """Build {norm_name: exact_name} + {leaf: exact_name} from a bnk's file table.

    Uses AssetBrowser's BNKReader only to LIST (the table decompresses fine); the
    entry BYTES are pulled via f2tool.exe, whose C++ BnkCore handles the level
    _models.bnk 'C' chunk compression the Python reader can't inflate.
    """
    import sys as _sys
    addon = Path(__file__).resolve().parents[2] / "Fable2AssetBrowser" / "source" / "Archive"
    _sys.path.insert(0, str(addon))
    from bnk_reader import BNKReader  # noqa: E402

    by_norm, by_leaf = {}, {}
    for e in BNKReader(str(bnk_path)).list_files():
        exact = e["name"]
        n = _norm(exact)
        by_norm[n] = exact
        by_leaf.setdefault(n.rsplit("/", 1)[-1], exact)
    return by_norm, by_leaf


def _resolve(index, model_path: str):
    """model path -> exact stored bnk name (norm match, then leaf fallback)."""
    by_norm, by_leaf = index
    n = _norm(model_path)
    return by_norm.get(n) or by_leaf.get(n.rsplit("/", 1)[-1])


def _instance_transform(block: dict, inst: dict):
    """Return (pos[3], yaw, scale) in RENDER axes.

    PROVEN against real chapter2slums data + prop_instance_xform @ LevelLoader.cpp:1328
    (ghidra_out/model_glue_lmp_format.txt section D): the loader swaps game(x,y,z) ->
    render(x,z,y); type-2 uses the sin/cos path (values[6]=sin, [7]=cos yaw); type-21 is
    pre-normalised (yaw_sin_cos + scale).
    """
    if block["kind"] == 2:
        v = inst["values"]
        pos = (v[0], v[2], v[1])
        yaw = math.atan2(v[6], v[7])
        scale = v[9] if v[9] and v[9] > 0.0 else 1.0
    else:  # type-21 (already normalised: pos + yaw_sin_cos + scale)
        p = inst["pos"]
        pos = (p[0], p[2], p[1])
        s, c = inst.get("yaw_sin_cos", (0.0, 1.0))
        yaw = math.atan2(s, c)
        scale = inst.get("scale", 1.0) or 1.0
    return pos, yaw, scale


def _cook_textures(tokens, textures_bnks, tex_cook: Path, f2tool: Path,
                   out_dir: Path, tmp: Path, log=print) -> dict:
    """Extract + decode each referenced `.tex` into a loose DDS, searching every source bnk.

    `textures_bnks` = ordered list of container Paths (e.g. globals_textures.bnk, the level's
    own textures.bnk). Returns {token: absolute-dds-path} for every texture that cooked.
    Shared textures are self-contained comp-1/11 LhTex blobs (verified on chapter2slums),
    decoded by f2native_cook_lh_tex (the AssetBrowser LhTexCodec, productised). comp-7 tiled
    BCn (needs a header bnk) is skipped with a warning; the material falls back to flat colour.
    """
    import subprocess
    sources = [b for b in (textures_bnks or []) if b]
    if not sources or not tex_cook:
        return {}
    out_dir.mkdir(parents=True, exist_ok=True)
    indices = [(b, _bnk_name_index(b)) for b in sources]
    cooked: dict[str, str] = {}
    for token in tokens:
        if token in cooked:
            continue
        cooked[token] = ""  # mark seen (so a failed texture isn't retried every material)
        bnk = exact = None
        for b, idx in indices:
            hit = _resolve(idx, token)
            if hit:
                bnk, exact = b, hit
                break
        if not exact:
            log(f"  tex skip (not in any textures bnk): {token}")
            continue
        # Sanitize the DDS filename (spaces -> _) so the absolute path emitted into the
        # whitespace-delimited F2SCENE stays tokenizable, even for space-named .tex entries.
        stem = _norm(token).rsplit("/", 1)[-1].rsplit(".", 1)[0].replace(" ", "_")
        raw = tmp / f"{stem}.tex"
        dds = out_dir / f"{stem}.dds"
        try:
            subprocess.run([str(f2tool), "extract", str(bnk), exact, str(raw)],
                           check=True, capture_output=True)
            result = subprocess.run([str(tex_cook), str(raw), str(dds)],
                                    capture_output=True, text=True)
            if result.returncode != 0 or not dds.is_file():
                log(f"  tex skip ({result.stdout.strip() or result.stderr.strip()}): {token}")
                continue
        except Exception as exc:  # noqa: BLE001 - skip-and-continue
            log(f"  tex skip ({type(exc).__name__}): {token}")
            continue
        cooked[token] = str(dds.resolve())
    return cooked


def _terrain_ground_texture(ehf_path: Path, f2tool: Path, log=print):
    """Pick the dominant ground albedo for a terrain .ehf from its LOD table + splat map.

    Runs `f2tool ehf` (which parses the EHF via the authoritative AssetBrowser
    EhfChunkParser) and reads the per-LOD `strs0` base-diffuse names plus the splat
    coverage histogram. The splat index encodes the 16 paint layers as index=lod*17;
    255 is the unpainted-base sentinel (falls back to LOD 0 in the engine's sampler,
    per LevelLoader.cpp bake-composite). We sum coverage per LOD and return the base
    diffuse of the most-painted layer. Returns (tex_token, base_scale) or None.

    This is Rung 1 (single dominant tiled ground texture). Full per-cell splat
    compositing across all LODs is a later step (the .ehf carries the data).
    """
    import subprocess
    try:
        out = subprocess.run([str(f2tool), "ehf", str(ehf_path)],
                             capture_output=True, text=True, check=True).stdout
    except Exception as exc:  # noqa: BLE001 - skip-and-continue
        log(f"  terrain ehf skip ({type(exc).__name__}: {exc})")
        return None
    lods: dict[int, tuple[str, float]] = {}
    hist: dict[int, int] = {}
    for line in out.splitlines():
        parts = line.split()
        if parts and parts[0] == "lod":
            li = int(parts[1])
            base_scale = 0.125
            strs0 = ""
            for p in parts[2:]:
                if p.startswith("base_scale="):
                    try:
                        base_scale = float(p.split("=", 1)[1])
                    except ValueError:
                        pass
                elif p.startswith("strs0="):
                    strs0 = p.split("=", 1)[1]
            if strs0 and strs0 != "-":
                lods[li] = (strs0, base_scale)
        elif parts and parts[0] == "splat_hist":
            for p in parts[1:]:
                idx, _, cnt = p.partition(":")
                if cnt:
                    hist[int(idx)] = int(cnt)
    if not lods:
        return None
    # Sum splat coverage per LOD (index = lod*17; 255 sentinel -> LOD 0 fallback).
    cover: dict[int, int] = {}
    for idx, cnt in hist.items():
        lod = 0 if idx == 255 else int(round(idx / 17.0))
        if lod not in lods:
            lod = 0
        cover[lod] = cover.get(lod, 0) + cnt
    dom = max(cover, key=cover.get) if cover else min(lods)
    tex, scale = lods[dom]
    total = sum(cover.values()) or 1
    log(f"  terrain ground = LOD {dom} '{tex}' (base_scale {scale}, "
        f"{100 * cover.get(dom, 0) // total}% splat coverage)")
    return tex, scale


def _terrain_splat_composite(ehf_path: Path, f2tool: Path, tex_cook: Path,
                             tex_sources, splat_bake: Path, out_dir: Path,
                             tmp: Path, res: int = 2048, log=print):
    """Bake a full per-cell ground albedo by compositing ALL LOD ground textures
    through the level's `.ehf` splat map (Rung 2 — the game's real terrain painting:
    grass / dirt / cobble / path regions, not one tiled texture).

    Pipeline: `f2tool ehf` -> each LOD's strs0 base diffuse -> cook to DDS (the normal
    albedo pass) -> `terrain_splat_bake <ehf> <out.dds> --res N --lod i=<dds>...` which
    ports the AssetBrowser LevelLoader bake-composite (sample_mat tiled by world pos,
    sample_mask from the splat, per-chunk-layer blend). Returns (abs_dds_path, (minx,
    minz, spanx, spanz)) where the bounds map the baked map across the terrain in game
    XY (the caller sets whole-terrain UVs from them), or None on any failure.
    """
    import subprocess, re
    if not (splat_bake and Path(splat_bake).is_file()):
        log("  terrain splat skip (terrain_splat_bake.exe not built)")
        return None
    if not (tex_cook and tex_sources):
        return None
    try:
        out = subprocess.run([str(f2tool), "ehf", str(ehf_path)],
                             capture_output=True, text=True, check=True).stdout
    except Exception as exc:  # noqa: BLE001
        log(f"  terrain splat skip (ehf dump: {type(exc).__name__})")
        return None
    lods: dict[int, str] = {}
    for line in out.splitlines():
        p = line.split()
        if p and p[0] == "lod":
            li = int(p[1])
            for t in p[2:]:
                if t.startswith("strs0="):
                    v = t.split("=", 1)[1]
                    if v and v != "-":
                        lods[li] = v
    if not lods:
        return None
    tex_map = _cook_textures(list(lods.values()), tex_sources, tex_cook, f2tool,
                             out_dir, tmp, log=lambda s: None)
    lod_args = []
    for li, tok in sorted(lods.items()):
        dds = tex_map.get(tok, "")
        if dds:
            lod_args += ["--lod", f"{li}={dds}"]
    if not lod_args:
        log("  terrain splat skip (no LOD textures cooked)")
        return None
    out_dds = out_dir / "terrain_splat.dds"
    out_dir.mkdir(parents=True, exist_ok=True)
    cmd = [str(splat_bake), str(ehf_path), str(out_dds), "--res", str(res)] + lod_args
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, check=True)
    except Exception as exc:  # noqa: BLE001
        detail = getattr(exc, "stderr", "") or getattr(exc, "stdout", "") or type(exc).__name__
        log(f"  terrain splat skip (bake failed: {str(detail).strip()[:200]})")
        return None
    m = re.search(r"BOUNDS minx=(\S+) minz=(\S+) spanx=(\S+) spanz=(\S+)", r.stdout)
    if not m or not out_dds.is_file():
        log("  terrain splat skip (no BOUNDS / no output DDS)")
        return None
    bounds = tuple(float(m.group(i)) for i in range(1, 5))
    log(f"  terrain splat composite: {len(lod_args) // 2} LODs -> {out_dds.name} "
        f"(world X[{bounds[0]:.0f},{bounds[0] + bounds[2]:.0f}] "
        f"Y[{bounds[1]:.0f},{bounds[1] + bounds[3]:.0f}], {res}x{res})")
    return str(out_dds.resolve()), bounds


def _build_terrain(ghf_bytes: bytes, stride: int = 1, uv_scale: float = TERRAIN_UV_PER_WU,
                   uv_world=None):
    """Decode a .ghf heightfield and build a render mesh (positions/normals/uvs/indices)
    in RENDER axes. Faithful port of ghidra_out/terrain_mesh_re.txt §1-4 (validated
    byte-exact on chapter2slums). Returns (positions, normals, uvs, indices) or None.

    `uv_scale` = texture repeats per world unit (default 0.125). When a ground albedo is
    cooked from the .ehf its LOD base_scale is passed here so the tiling matches the game.
    `uv_world` = (minx, minz, spanx, spanz) in game XY; when set, UVs map the WHOLE terrain
    0..1 across those bounds (for a pre-baked splat-composite albedo that already tiles
    internally), overriding `uv_scale`.

    .ghf = gzip stream; raw = origin(3×be_f32) + wCells(be_u32 @0xC) + hCells(be_u32 @0x10)
    + W*H cells of 14 bytes {f32 height, f32 water, u32 materialGUID, u8, u8}. Vertex per
    cell at game(x*tile, y*tile, height); render = {x,z,y} swap = (x*tile, height, y*tile);
    normal = central-difference (hl-hr, 2*tile, hd-hu); UV = (x*tile*0.125, y*tile*0.125).
    """
    raw = gzip.decompress(ghf_bytes)
    if len(raw) < 0x14:
        return None
    ox, oy, oz = struct.unpack_from(">fff", raw, 0)
    w, h = struct.unpack_from(">II", raw, 0xC)
    if not (0 < w <= 8192 and 0 < h <= 8192) or 0x14 + w * h * 14 != len(raw):
        return None
    tile = TERRAIN_TILE
    # Heights (full grid), then subsample by stride.
    heights = [struct.unpack_from(">f", raw, 0x14 + i * 14)[0] for i in range(w * h)]

    def hgt(cx, cy):
        cx = 0 if cx < 0 else (w - 1 if cx >= w else cx)
        cy = 0 if cy < 0 else (h - 1 if cy >= h else cy)
        return heights[cy * w + cx]

    s = max(1, int(stride))
    xs = list(range(0, w, s))
    ys = list(range(0, h, s))
    gw, gh = len(xs), len(ys)
    eff = tile * s  # effective spacing on the subsampled grid (for the normal)
    # Emit in GAME space (X, Y, Z=height); the F2SCENE writer applies the same
    # {x,z,y}->render swap it uses for prop meshes, so terrain lines up under buildings.
    positions, normals, uvs = [], [], []
    for cy in ys:
        for cx in xs:
            gx, gy, gz = cx * tile + ox, cy * tile + oy, hgt(cx, cy) + oz
            positions.extend((gx, gy, gz))
            hl, hr = hgt(cx - s, cy), hgt(cx + s, cy)
            hd, hu = hgt(cx, cy - s), hgt(cx, cy + s)
            # game-space normal (Z=up): (-dh/dx, -dh/dy, 1) ∝ (hl-hr, hd-hu, 2*eff).
            nx, ny, nz = (hl - hr), (hd - hu), 2.0 * eff
            nlen = math.sqrt(nx * nx + ny * ny + nz * nz) or 1.0
            normals.extend((nx / nlen, ny / nlen, nz / nlen))
            if uv_world:
                mnx, mnz, spx, spz = uv_world
                uvs.extend(((gx - mnx) / spx if spx else 0.0,
                            (gy - mnz) / spz if spz else 0.0))
            else:
                uvs.extend((gx * uv_scale, gy * uv_scale))
    indices = []
    for j in range(gh - 1):
        for i in range(gw - 1):
            i00 = j * gw + i
            i10 = j * gw + i + 1
            i01 = (j + 1) * gw + i
            i11 = (j + 1) * gw + i + 1
            indices.extend((i00, i01, i10, i10, i01, i11))
    return positions, normals, uvs, indices


def _build_water(water_bytes: bytes):
    """Parse a .water file → one merged flat water-plane mesh in GAME space (worldX, worldZ,
    base_height); the F2SCENE writer's {x,z,y} swap puts the plane at render Y=base_height.
    Faithful port of ghidra_out/water_system_re.txt (BE). Returns (pos,nrm,uv,idx) or None."""
    MARKER = 0x00000FEC
    r = BeReader(water_bytes)
    version = r.u32()
    body_count = r.u32()
    if version != 2 or not body_count or body_count > 256:
        return None  # empty/vista placeholder or corrupt
    offsets = [r.u32() for _ in range(body_count)]
    pos, nrm, uv, idx = [], [], [], []
    for off in offsets:
        if off is None or off >= r.n:
            continue
        r.i = off
        if r.u32() != MARKER:
            continue
        r.f32()                                   # param_a
        base_h = r.f32()
        for _ in range(37):                       # material params (shader-side; skip)
            r.f32()
        r.cstr()                                  # normal_map path
        r.cstr()                                  # secondary_map path
        patch_count = r.u32()
        if patch_count is None or patch_count > 4096:
            continue
        for _t in range(patch_count):
            if r.u32() != MARKER:
                break
            cx, cz = r.f32(), r.f32()
            ex, ez = r.f32(), r.f32()
            cwf, chf = r.f32(), r.f32()
            aux_count = r.u32() or 0
            for _a in range(min(aux_count, 64)):
                r.u32()
            mask_count = r.u32() or 0
            mask = [r.u8() for _ in range(mask_count)]
            r.u32()                               # tile-end marker
            cw = max(int(cwf or 1), 1)
            ch = max(int(chf or 1), 1)
            if mask_count != cw * ch or None in (cx, cz, ex, ez, base_h):
                continue
            hx, hz = abs(ex) * 0.5, abs(ez) * 0.5
            x0, x1 = cx - hx, cx + hx
            z0, z1 = cz - hz, cz + hz
            base_v = len(pos) // 3
            for mz in range(ch + 1):
                for mx in range(cw + 1):
                    px = x0 + (x1 - x0) * mx / cw
                    pz = z0 + (z1 - z0) * mz / ch
                    pos.extend((px, pz, base_h))  # game (X, Z-ground, height)
                    nrm.extend((0.0, 0.0, 1.0))   # up in game Z
                    uv.extend((px * 0.05, pz * 0.05))
            for mz in range(ch):
                for mx in range(cw):
                    if not mask[mz * cw + mx]:
                        continue
                    v00 = base_v + mz * (cw + 1) + mx
                    v10 = v00 + 1
                    v01 = base_v + (mz + 1) * (cw + 1) + mx
                    v11 = v01 + 1
                    idx.extend((v00, v11, v10, v00, v01, v11))
    return (pos, nrm, uv, idx) if idx else None


# Default child-male villager part set (all validated through fable_mdl_format's
# 28-byte skinned path, NO StringBlock — ghidra_out/npc_spawn_re.txt §2.3). A matched
# {head,torso,legs} authored in one shared rig space stacks into a standing villager.
NPC_CHILD_MALE_PARTS = [
    r"Art\Characters\Npc\Random Villagers\Children\dotXSI\CH_mchild_head_01\CH_mchild_head_01.mdl",
    r"Art\Characters\Npc\Random Villagers\Children\dotXSI\CH_mchild_torso_01\CH_mchild_torso_01.mdl",
    r"Art\Characters\Npc\Random Villagers\Children\dotXSI\CH_mchild_legs_01\CH_mchild_legs_01.mdl",
]
NPC_CHILD_FEMALE_PARTS = [
    r"Art\Characters\Npc\Random Villagers\Children\dotXSI\CH_fchild_head_01\CH_fchild_head_01.mdl",
    r"Art\Characters\Npc\Random Villagers\Children\dotXSI\CH_fchild_torso_01\CH_fchild_torso_01.mdl",
    r"Art\Characters\Npc\Random Villagers\Children\dotXSI\CH_fchild_legs_01\CH_fchild_legs_01.mdl",
]


def read_npc_markers(level_save: Path, level_gdb: Path, markerdump: Path,
                     log=print) -> list:
    """Read creature-spawn markers from <level>.save + <level>.gdb via npc_markerdump.

    npc_markerdump follows the SimpleTransformComponent 0x619F96CF -> Position/Rotation
    chain that GdbParser::LookupPlacement skips (ghidra_out/npc_spawn_re.txt §1.2). It is
    the ONE piece of new read code the NPC cook needs; everything else reuses cook helpers.
    Returns [{"name","pos":[gx,gy,gz],"yaw"}] in GAME space (the caller applies {x,z,y}).
    """
    import subprocess, tempfile
    if not markerdump or not Path(markerdump).is_file():
        log(f"  npc skip (markerdump tool not found: {markerdump})")
        return []
    tmp_json = Path(tempfile.mkdtemp(prefix="f2npc_")) / "markers.json"
    try:
        subprocess.run([str(markerdump), str(level_save), str(level_gdb),
                        "--out", str(tmp_json)], check=True, capture_output=True)
        data = json.loads(tmp_json.read_text(encoding="utf-8"))
    except Exception as exc:  # noqa: BLE001
        log(f"  npc skip (marker read failed: {type(exc).__name__}: {exc})")
        return []
    markers = data.get("markers", [])
    log(f"npc markers: {len(markers)} creature-spawn points "
        f"(hit={data.get('hit')} miss={data.get('miss')})")
    return markers


def read_props(level_save: Path, level_gdb: Path, propdump: Path,
               model_indices, globals_gdb: Path = None, log=print) -> list:
    """Read the level's renderable .gdb/.save ENTITY PROPS via the propdump tool.

    The engine_level's type-2/21 blocks only carry the ~16 static prop archetypes;
    the town's real density (furniture, crates, barrels, doors, walls, railings,
    ~37 townhouse facades — ~1800 entities) lives in the .save/.gdb layer. propdump
    resolves each entity -> GraphicAppearanceStaticMeshComponent (0x29CF50D1) ->
    model-resource -> ModelFile (0x0C17DB4E) model-path HASH, plus the transform
    chain, exactly like lightdump/npc_markerdump (ghidra_out/gdb_component_schemas.txt
    + gdb_instantiation_re.txt). The model-path strings aren't in the gdb (verified);
    they are the MODEL-BANK entry names, so we feed propdump the bank name tables
    (from the same header/body bnks the cooker glues from) as --models-list files and
    it reverse-maps the FNV-1(lower) hash -> the literal .mdl path.

    Returns [{"name","pos":[gx,gy,gz],"yaw","model"}] in GAME space (caller applies
    the {x,z,y} swap). Entities with no static model / no transform are dropped by
    the tool (counted in no_model / no_xform).
    """
    import subprocess, tempfile
    if not propdump or not Path(propdump).is_file():
        log(f"  props skip (propdump tool not found: {propdump})")
        return []
    tmpd = Path(tempfile.mkdtemp(prefix="f2props_"))
    # Write the model-bank name tables so propdump can reverse the model-path hash.
    list_args = []
    for tag, index in model_indices:
        by_norm, _by_leaf = index
        # The bnk stores the EXACT (original-case) names; recover them from by_norm's
        # values (by_norm maps norm->exact). propdump hashes each (and the .gmd-strip).
        names = sorted(set(by_norm.values()))
        lf = tmpd / f"models_{tag}.txt"
        lf.write_text("\n".join(names), encoding="utf-8")
        list_args += ["--models-list", str(lf)]
    out_json = tmpd / "props.json"
    cmd = [str(propdump), str(level_save), str(level_gdb)]
    if globals_gdb and Path(globals_gdb).is_file():
        cmd += ["--globals-gdb", str(globals_gdb)]
    cmd += list_args + ["--out", str(out_json)]
    try:
        subprocess.run(cmd, check=True, capture_output=True)
        data = json.loads(out_json.read_text(encoding="utf-8"))
    except Exception as exc:  # noqa: BLE001
        log(f"  props skip (read failed: {type(exc).__name__}: {exc})")
        return []
    props = data.get("props", [])
    log(f"props: {len(props)} renderable gdb entities "
        f"(no_model={data.get('no_model')} unresolved={data.get('unresolved_name')} "
        f"no_xform={data.get('no_xform')} markers_skipped={data.get('markers_skipped')})")
    return props


def read_lights(level_save: Path, level_gdb: Path, lightdump: Path,
                globals_gdb: Path = None, log=print) -> list:
    """Read a level's LOCAL POINT LIGHTS (lamp posts, lanterns, braziers, placeable
    accents) via the lightdump tool (ghidra_out/level_lights_effects_re.txt §1).

    lightdump reads <level>.save + <level>.gdb and the 0xBB61B654 light component
    (Intensity/Range/Colour/LightType), chaining kHashParent into globals.gdb so a
    placed light inherits its archetype Colour. Returns
    [{"name","pos":[gx,gy,gz],"rgb":[r,g,b],"intensity","range","type"}] in GAME
    space (caller applies {x,z,y} + rgb/255). Off/culled lights (range<=0) are
    already filtered by the tool.
    """
    import subprocess, tempfile
    if not lightdump or not Path(lightdump).is_file():
        log(f"  lights skip (lightdump tool not found: {lightdump})")
        return []
    tmp_json = Path(tempfile.mkdtemp(prefix="f2lights_")) / "lights.json"
    cmd = [str(lightdump), str(level_save), str(level_gdb)]
    if globals_gdb and Path(globals_gdb).is_file():
        cmd += ["--globals-gdb", str(globals_gdb)]
    else:
        log("  lights: no globals.gdb -> colours may fall back to white")
    cmd += ["--out", str(tmp_json)]
    try:
        subprocess.run(cmd, check=True, capture_output=True)
        data = json.loads(tmp_json.read_text(encoding="utf-8"))
    except Exception as exc:  # noqa: BLE001
        log(f"  lights skip (read failed: {type(exc).__name__}: {exc})")
        return []
    lights = data.get("lights", [])
    log(f"lights: {len(lights)} local point lights "
        f"(hit={data.get('hit')} miss={data.get('miss')} off={data.get('off')} "
        f"no_colour={data.get('no_colour')})")
    return lights


def _build_ehf(ehf_bytes: bytes):
    """Cook a flat sea/backdrop .ehf vista mesh (the distant coast that fills the void
    between the town heightfield and the castle). ghidra_out/ehf_vista_re.txt: the 63-byte
    BE header carries origin (f0/f1), grid dims (u0/u1) and tile (f2); the render surface is
    a flat plane whose height is the constant repeated across the body's 850A0 vertices.
    Returns (pos,nrm,uv,idx) in GAME space (X, Y, height) — the writer applies the {x,z,y}
    swap. Flat-vista path only (the sea_vista is planar)."""
    if len(ehf_bytes) < 0x3f or ehf_bytes[:23] != b"HeightFieldGraphicsFile":
        return None
    ox, oy = struct.unpack_from(">ff", ehf_bytes, 0x1b)
    u0, u1 = struct.unpack_from(">II", ehf_bytes, 0x23)
    tile = struct.unpack_from(">f", ehf_bytes, 0x2b)[0]
    body_off = struct.unpack_from(">I", ehf_bytes, 0x37)[0]
    if not (0 < u0 <= 4096 and 0 < u1 <= 4096 and tile > 0 and body_off < len(ehf_bytes)):
        return None
    # Flat height = the pre-850A0 float in the body, reached via the exact EhfChunkParser
    # walk (skip the two leading .tex blobs). ghidra_out/ehf_vista_re.txt §7 / EhfChunkParser.
    def _skip_tex(p):
        mt = struct.unpack_from(">I", ehf_bytes, p + 0x20)[0]
        pf = struct.unpack_from(">I", ehf_bytes, p + 0x18)[0]
        if pf == 98:
            w = struct.unpack_from(">I", ehf_bytes, p + 0x10)[0]
            h = struct.unpack_from(">I", ehf_bytes, p + 0x14)[0]
            return p + mt + w * h * 2
        comp = struct.unpack_from(">I", ehf_bytes, p + mt + 4)[0]
        return p + mt + 8 + comp
    p = _skip_tex(body_off)
    p = _skip_tex(p)
    height = struct.unpack_from(">f", ehf_bytes, p)[0]
    if not math.isfinite(height):
        return None
    pos, nrm, uv, idx = [], [], [], []
    for cy in range(u1):
        for cx in range(u0):
            px, py = ox + cx * tile, oy + cy * tile
            pos.extend((px, py, height))
            nrm.extend((0.0, 0.0, 1.0))
            uv.extend((px * 0.02, py * 0.02))
    for j in range(u1 - 1):
        for i in range(u0 - 1):
            v00 = j * u0 + i
            v10 = v00 + 1
            v01 = (j + 1) * u0 + i
            v11 = v01 + 1
            idx.extend((v00, v01, v10, v10, v01, v11))
    return (pos, nrm, uv, idx) if idx else None


def cook_level(engine_level: Path, header_bnk: Path, body_bnk: Path, f2tool: Path,
               out_scene: Path, types=(2, 21), max_per_block=None, log=print,
               textures_bnks=None, tex_cook: Path = None,
               tex_out_dir: Path = None, terrain_ghf: Path = None,
               terrain_ehf: Path = None, terrain_splat: bool = True,
               terrain_splat_res: int = 2048, splat_bake: Path = None,
               terrain_stride: int = 1, hero_model: str = None,
               hero_body_bnk: Path = None, hero_pos=None,
               water_file: Path = None, npcs: bool = False,
               npc_body_bnk: Path = None, npc_markerdump: Path = None,
               npc_level_save: Path = None, npc_level_gdb: Path = None,
               npc_limit: int = None, npc_parts=None,
               lights: bool = False, lightdump: Path = None,
               globals_gdb: Path = None, light_limit: int = None,
               props: bool = False, propdump: Path = None,
               prop_limit: int = None, vista_ehf: Path = None) -> dict:
    """Stage 2: glue every prop model (header++body) and merge instances into one F2SCENE.

    Data-driven from ghidra_out/model_glue_lmp_format.txt (the RE'd glue) — no guessing.
    Skips (with a warning) any model whose bank entry is missing or whose MDL body uses a
    stride fable_mdl_format doesn't yet decode (e.g. some foliage), so type-2 landmarks
    still render.
    """
    import subprocess, sys as _sys, tempfile
    addon = Path(__file__).resolve().parents[2] / "Fable2AssetBrowser" / "source" / "addons"
    _sys.path.insert(0, str(addon))
    import fable_mdl_format as mdl  # noqa: E402
    from cook_mdl import add_normals, texture_token  # reuse the per-MDL emit helpers

    # Optional idle-pose baker (Fable2Native/tools/fable_pose.py): bakes a natural standing idle
    # onto the skinned characters (hero/NPCs) so they aren't in the raw bind A-pose. Any failure
    # (missing anim bank / numpy) leaves it None and characters fall back to the bind pose.
    _sys.path.insert(0, str(Path(__file__).resolve().parent))
    fable_pose = None
    _anim_clips = _anim_df = None
    try:
        import fable_pose  # noqa: E402
        _anim_clips, _anim_df = fable_pose.load_anim_bank(header_bnk.parent.parent)
    except Exception as exc:  # noqa: BLE001
        log(f"  pose: idle-anim bank unavailable ({type(exc).__name__}); characters use bind pose")

    def _make_poser(char_info):
        if fable_pose is None or _anim_clips is None or _anim_df is None:
            return None
        try:
            return fable_pose.make_poser(char_info, _anim_clips, _anim_df)
        except Exception:  # noqa: BLE001
            return None

    info = parse_engine_level(engine_level.read_bytes())
    hidx = _bnk_name_index(header_bnk)
    bidx = _bnk_name_index(body_bnk)
    tmp = Path(tempfile.mkdtemp(prefix="f2cook_"))

    def extract(bnk: Path, exact: str, tag: str) -> bytes:
        dst = tmp / tag
        subprocess.run([str(f2tool), "extract", str(bnk), exact, str(dst)],
                       check=True, capture_output=True)
        return dst.read_bytes()

    # Cook each DISTINCT model once -> list of geoms (positions/indices/uvs/textures).
    model_geoms: dict[str, list] = {}

    def cook_model(model_path: str):
        key = _norm(model_path)
        if key in model_geoms:
            return model_geoms[key]
        he = _resolve(hidx, model_path)
        be = _resolve(bidx, model_path)
        if not he or not be:
            log(f"  skip (no bank entry): {model_path}")
            model_geoms[key] = None
            return None
        try:
            glued = extract(header_bnk, he, "h.bin") + extract(body_bnk, be, "b.bin")
            # Pass the model path so the MDL parser can gate the foliage buffer layout
            # (grass/trees use strides 48/36 the common paths can't read) — see
            # ghidra_out/foliage_system_re.txt.
            _, geoms = mdl.parse(glued, log=lambda m: None, file_path=model_path)
        except Exception as exc:  # noqa: BLE001 - want to skip-and-continue
            log(f"  skip ({type(exc).__name__}): {model_path}")
            model_geoms[key] = None
            return None
        model_geoms[key] = geoms or None
        return model_geoms[key]

    # First pass: cook models + assign stable mesh/material names.
    mesh_names: dict[str, list] = {}   # model key -> [mesh_name per geom]
    materials, meshes = [], []          # F2SCENE material / mesh records
    for block in info["prop_blocks"]:
        if block["kind"] not in types or not block.get("model"):
            continue
        key = _norm(block["model"])
        if key in mesh_names:
            continue
        geoms = cook_model(block["model"])
        if not geoms:
            mesh_names[key] = None
            continue
        mid = len(mesh_names)
        names = []
        for gi, g in enumerate(geoms):
            mat_idx = len(materials)
            opts = []
            for attr, tok in (("diffuse", "albedo"), ("normal_tex", "normal"),
                              ("specular_tex", "material")):
                val = getattr(g, attr, "")
                if val:
                    # Keep the RAW path (spaces preserved) for bnk resolution — some .tex
                    # entries are stored with spaces ("bs_gatehouse_stone top.tex"), which
                    # texture_token's space->underscore would break. These tokens never reach
                    # the F2SCENE text: albedo is replaced by the cooked DDS path (or dropped),
                    # and normal/material are dropped, so no space is emitted.
                    opts.append(f"{tok}={val.replace(chr(92), '/')}")
            materials.append((f"mat_{mid}_{gi}", opts, (0.72, 0.72, 0.72, 1.0)))
            positions = g.positions
            normals = g.normals or add_normals(positions, g.indices)
            name = f"m{mid}_{gi}"
            names.append(name)
            meshes.append((name, mat_idx, positions, normals, g.uvs, g.indices))
        mesh_names[key] = names

    # Second pass: one instance record per (instance x geom-mesh).
    instances, n_inst, n_blocks = [], 0, 0
    for block in info["prop_blocks"]:
        if block["kind"] not in types or not block.get("model"):
            continue
        names = mesh_names.get(_norm(block["model"]))
        if not names:
            continue
        n_blocks += 1
        insts = block["instances"]
        if max_per_block:
            insts = insts[:max_per_block]
        for inst in insts:
            pos, yaw, scale = _instance_transform(block, inst)
            for name in names:
                instances.append((name, pos, yaw, scale))
            n_inst += 1

    # Terrain: append the level heightfield as one ground mesh (flat earth-tone material,
    # Phase T1 — ghidra_out/terrain_mesh_re.txt). Game-space verts flow through the same
    # emit swap as props; identity instance (mesh is already world-placed).
    if terrain_ghf:
        # Ground albedo from the level's .ehf (data-driven). Prefer the FULL per-cell splat
        # COMPOSITE (all LOD textures blended by the splat map = grass/dirt/cobble/path
        # regions, Rung 2); fall back to the single dominant ground texture (Rung 1) if the
        # composite baker isn't built or fails; else a flat earth colour.
        t_ground = t_composite = None
        _tex_out = tex_out_dir or (out_scene.parent / (out_scene.stem + ".textures"))
        _tex_sources = [b for b in (textures_bnks or []) if b]
        if terrain_ehf:
            if terrain_splat:
                t_composite = _terrain_splat_composite(
                    Path(terrain_ehf), f2tool, tex_cook, _tex_sources, splat_bake,
                    _tex_out, tmp, res=terrain_splat_res or 2048, log=log)
            if not t_composite:
                t_ground = _terrain_ground_texture(Path(terrain_ehf), f2tool, log=log)
        t_uv_scale = t_ground[1] if t_ground else TERRAIN_UV_PER_WU
        t_uv_world = t_composite[1] if t_composite else None
        try:
            built = _build_terrain(Path(terrain_ghf).read_bytes(), stride=terrain_stride or 1,
                                   uv_scale=t_uv_scale, uv_world=t_uv_world)
        except Exception as exc:  # noqa: BLE001
            built = None
            log(f"  terrain skip ({type(exc).__name__}: {exc})")
        if built:
            t_pos, t_nrm, t_uv, t_idx = built
            t_mat = len(materials)
            # White base_colour so the sampled ground albedo shows at full fidelity
            # (base = base_colour * albedo, per native_world_renderer PS).
            if t_composite:
                # Pre-baked absolute DDS -> bypass the bnk albedo cook via albedo_abs=.
                materials.append(("terrain", ["albedo_abs=" + t_composite[0]], (1.0, 1.0, 1.0, 1.0)))
            elif t_ground:
                materials.append(("terrain", ["albedo=" + t_ground[0]], (1.0, 1.0, 1.0, 1.0)))
            else:
                materials.append(("terrain", [], (0.33, 0.30, 0.24, 1.0)))
            meshes.append(("terrain0", t_mat, t_pos, t_nrm, t_uv, t_idx))
            instances.append(("terrain0", (0.0, 0.0, 0.0), 0.0, 1.0))
            n_inst += 1
            log(f"terrain: {len(t_pos)//3} verts / {len(t_idx)//3} tris "
                f"(stride {terrain_stride or 1})")

    # Water: append the level's water bodies as flat blue planes (Phase A — opaque water-colour
    # mesh; the animated bump/fresnel shader is a later phase). water_system_re.txt.
    if water_file:
        try:
            wbuilt = _build_water(Path(water_file).read_bytes())
        except Exception as exc:  # noqa: BLE001
            wbuilt = None
            log(f"  water skip ({type(exc).__name__}: {exc})")
        if wbuilt:
            w_pos, w_nrm, w_uv, w_idx = wbuilt
            w_mat = len(materials)
            materials.append(("water", [], (0.14, 0.34, 0.52, 1.0)))
            meshes.append(("water0", w_mat, w_pos, w_nrm, w_uv, w_idx))
            instances.append(("water0", (0.0, 0.0, 0.0), 0.0, 1.0))
            n_inst += 1
            log(f"water: {len(w_pos)//3} verts / {len(w_idx)//3} tris")

    # Distant sea/coast backdrop (.ehf) — the flat plane that fills the seaward void
    # between the town heightfield and Fairfax castle (ghidra_out/ehf_vista_re.txt).
    if vista_ehf:
        try:
            vbuilt = _build_ehf(Path(vista_ehf).read_bytes())
        except Exception as exc:  # noqa: BLE001
            vbuilt = None
            log(f"  vista skip ({type(exc).__name__}: {exc})")
        if vbuilt:
            v_pos, v_nrm, v_uv, v_idx = vbuilt
            v_mat = len(materials)
            materials.append(("vista", [], (0.28, 0.30, 0.30, 1.0)))
            meshes.append(("vista0", v_mat, v_pos, v_nrm, v_uv, v_idx))
            instances.append(("vista0", (0.0, 0.0, 0.0), 0.0, 1.0))
            n_inst += 1
            log(f"vista: {len(v_pos)//3} verts / {len(v_idx)//3} tris")

    # Hero: cook the child hero (globals body bnk) and drop one instance into the town so the
    # first level isn't empty of characters (ghidra_out/hero_render_re.txt). The model is skinned;
    # fable_mdl_format decodes the bind pose (child standing) and the emit swap stands it upright.
    # Placement: PlayerStart XYZ needs a .gdb walk (GAP-P1); for a first render use the building
    # centroid (render space) at the median prop height.
    if hero_model and hero_body_bnk:
        try:
            gidx = _bnk_name_index(hero_body_bnk)  # globals_models.bnk (hero polymsh bodies)
            he = _resolve(hidx, hero_model)
            be = _resolve(gidx, hero_model)
            if not (he and be):
                log(f"  hero skip (no bank entry): {hero_model}")
            else:
                glued = extract(header_bnk, he, "hero_h.bin") + extract(hero_body_bnk, be, "hero_b.bin")
                hinfo, hgeoms = mdl.parse(glued, log=lambda m: None, file_path=hero_model)
                hero_poser = _make_poser(hinfo)  # bake a standing idle (None -> bind pose)
                # chapter2slums PlayerStart (ghidra_out/npc_spawn_re.txt, validated from the
                # .gdb SimpleTransformComponent 0x619F96CF): game(192.622,169.201,48.637) yaw
                # -1.288 -> render {x,z,y}. Overridable with --hero-pos.
                hero_yaw = -1.288
                if hero_pos:
                    hx, hy, hz = hero_pos
                else:
                    hx, hy, hz = 192.622, 48.637, 169.201
                for gi, g in enumerate(hgeoms or []):
                    mat_idx = len(materials)
                    opts = []
                    val = getattr(g, "diffuse", "")
                    if val:
                        opts.append("albedo=" + val.replace(chr(92), "/"))
                    materials.append((f"hero_{gi}", opts, (0.80, 0.70, 0.62, 1.0)))
                    posed = hero_poser(g) if hero_poser else None
                    positions = posed if posed else g.positions
                    normals = add_normals(positions, g.indices)  # recompute from posed tris
                    name = f"hero{gi}"
                    meshes.append((name, mat_idx, positions, normals, g.uvs, g.indices))
                    instances.append((name, (hx, hy, hz), hero_yaw, 1.0))
                    n_inst += 1
                log(f"hero: {len(hgeoms or [])} geoms at PlayerStart render "
                    f"({hx:.1f},{hy:.1f},{hz:.1f}) yaw {hero_yaw}")
        except Exception as exc:  # noqa: BLE001
            log(f"  hero skip ({type(exc).__name__}: {exc})")

    # NPCs: populate the town with posed (bind-pose) villagers at the creature-spawn
    # markers (ghidra_out/npc_spawn_re.txt). Mirrors the --hero cook, generalised to a
    # modular {head,torso,legs} part-set placed at N marker transforms. The parts share
    # the rig bind pose, so all three stack at ONE instance transform per marker.
    if npcs and npc_body_bnk:
        try:
            markers = read_npc_markers(npc_level_save, npc_level_gdb, npc_markerdump, log=log)
            if npc_limit and npc_limit > 0:
                markers = markers[:npc_limit]
            parts = npc_parts or NPC_CHILD_MALE_PARTS
            nidx = _bnk_name_index(npc_body_bnk)  # globals_models.bnk (villager polymsh bodies)
            # Cook each part MDL ONCE -> list of (mesh_name, mat_idx) for its geoms.
            part_meshes = []  # [[(mesh_name, mat_idx), ...] per part]
            for part in parts:
                he = _resolve(hidx, part)
                be = _resolve(nidx, part)
                if not (he and be):
                    log(f"  npc part skip (no bank entry): {part}")
                    continue
                try:
                    glued = extract(header_bnk, he, "npc_h.bin") + extract(npc_body_bnk, be, "npc_b.bin")
                    pinfo, pgeoms = mdl.parse(glued, log=lambda m: None, file_path=part)
                except Exception as exc:  # noqa: BLE001 - StringBlock parts, etc: skip-and-continue
                    log(f"  npc part skip ({type(exc).__name__}): {part}")
                    continue
                npc_poser = _make_poser(pinfo)  # same idle pose as the hero (None -> bind)
                pi = len(part_meshes)
                geom_recs = []
                for gi, g in enumerate(pgeoms or []):
                    mat_idx = len(materials)
                    opts = []
                    val = getattr(g, "diffuse", "")
                    if val:
                        opts.append("albedo=" + val.replace(chr(92), "/"))
                    materials.append((f"npc{pi}_{gi}", opts, (0.80, 0.70, 0.62, 1.0)))
                    posed = npc_poser(g) if npc_poser else None
                    positions = posed if posed else g.positions
                    normals = add_normals(positions, g.indices)
                    name = f"npc{pi}_{gi}"
                    meshes.append((name, mat_idx, positions, normals, g.uvs, g.indices))
                    geom_recs.append(name)
                if geom_recs:
                    part_meshes.append(geom_recs)
            # One instance per (marker x part-geom): render pos = game {x,z,y}; yaw = Rotation.VecX.
            n_npc = 0
            for m in markers:
                gp = m.get("pos") or [0.0, 0.0, 0.0]
                rx, ry, rz = gp[0], gp[2], gp[1]  # game (x,y,z) -> render (x,z,y)
                yaw = float(m.get("yaw", 0.0))
                for geom_recs in part_meshes:
                    for name in geom_recs:
                        instances.append((name, (rx, ry, rz), yaw, 1.0))
                n_npc += 1
                n_inst += 1
            log(f"npcs: {n_npc} villagers ({len(part_meshes)} parts each) at creature markers")
        except Exception as exc:  # noqa: BLE001
            log(f"  npc skip ({type(exc).__name__}: {exc})")

    # GDB entity props: the town's real density (furniture/crates/barrels/doors/walls/
    # railings + the ~37 townhouse facades — ~1800 placed entities) lives in the
    # .save/.gdb layer, NOT the engine_level. read_props() resolves each entity ->
    # GraphicAppearanceStaticMeshComponent -> ModelFile model-path (via propdump), and
    # here we glue+place each like a static prop: cook each DISTINCT model once (dedup),
    # then emit one instance per entity at its transform (same {x,z,y} swap + yaw).
    # The models live in EITHER the level body bnk (most) or globals_models (a few), so
    # we try both bodies. Textures cook through the same _cook_textures() pass below.
    if props and propdump:
        try:
            # Build the model-bank name tables propdump needs to reverse the hash:
            # the level body bnk (bidx) + the header bnk (hidx, has every model name).
            model_indices = [("level", bidx), ("headers", hidx)]
            prop_body_bnk = npc_body_bnk or hero_body_bnk  # globals_models for the ~33
            prop_bidx = _bnk_name_index(prop_body_bnk) if prop_body_bnk else None
            found = read_props(npc_level_save, npc_level_gdb, propdump, model_indices,
                               globals_gdb=globals_gdb, log=log)
            if prop_limit and prop_limit > 0:
                # Cap by DISTINCT model (cook fewer meshes) while keeping all instances
                # of the kept models — a representative dense first pass.
                seen_models, kept = set(), []
                for p in found:
                    m = p.get("model", "")
                    if m and m not in seen_models and len(seen_models) >= prop_limit:
                        continue
                    seen_models.add(m)
                    kept.append(p)
                found = kept

            def cook_prop_model(model_path: str):
                """Glue+parse a prop model, trying the level body bnk then globals_models."""
                key = _norm(model_path)
                if key in model_geoms:
                    return model_geoms[key]
                he = _resolve(hidx, model_path)
                if not he:
                    model_geoms[key] = None
                    return None
                for bnk, idx in ((body_bnk, bidx),
                                 (prop_body_bnk, prop_bidx) if prop_bidx else (None, None)):
                    if not bnk or not idx:
                        continue
                    be = _resolve(idx, model_path)
                    if not be:
                        continue
                    try:
                        glued = extract(header_bnk, he, "ph.bin") + extract(bnk, be, "pb.bin")
                        _, geoms = mdl.parse(glued, log=lambda m: None, file_path=model_path)
                    except Exception as exc:  # noqa: BLE001 - skip-and-continue (unsupported stride/StringBlock)
                        model_geoms[key] = None
                        return None
                    model_geoms[key] = geoms or None
                    return model_geoms[key]
                model_geoms[key] = None
                return None

            # Distant-backdrop VISTA models (RS_/TS_Vista_*, FarMountains, skydome) are
            # enormous scenery meant to sit at the horizon behind a special vista shader.
            # As town props they render dark AND blow up the scene AABB (shrinking the
            # town to a dot under the auto-fit camera), so skip them — they're backdrop,
            # not the street density this pass adds.
            def _is_backdrop(mp: str) -> bool:
                low = mp.lower()
                return ("vista" in low or "farmountain" in low or "skydome" in low
                        or "backdrop" in low)

            prop_mesh_names: dict[str, list] = {}   # model key -> [mesh_name per geom]
            n_prop_inst, n_prop_models, n_prop_skip, n_backdrop = 0, 0, 0, 0
            for p in found:
                model_path = p.get("model", "")
                if not model_path:
                    continue
                if _is_backdrop(model_path):
                    n_backdrop += 1
                    continue
                key = _norm(model_path)
                if key not in prop_mesh_names:
                    geoms = cook_prop_model(model_path)
                    if not geoms:
                        prop_mesh_names[key] = None
                        n_prop_skip += 1
                    else:
                        pid = f"p{len(prop_mesh_names)}"
                        names = []
                        for gi, g in enumerate(geoms):
                            mat_idx = len(materials)
                            opts = []
                            for attr, tok in (("diffuse", "albedo"), ("normal_tex", "normal"),
                                              ("specular_tex", "material")):
                                val = getattr(g, attr, "")
                                if val:
                                    opts.append(f"{tok}={val.replace(chr(92), '/')}")
                            materials.append((f"mat_{pid}_{gi}", opts, (0.72, 0.72, 0.72, 1.0)))
                            positions = g.positions
                            normals = g.normals or add_normals(positions, g.indices)
                            name = f"{pid}_{gi}"
                            names.append(name)
                            meshes.append((name, mat_idx, positions, normals, g.uvs, g.indices))
                        prop_mesh_names[key] = names
                        n_prop_models += 1
                names = prop_mesh_names.get(key)
                if not names:
                    continue
                gp = p.get("pos") or [0.0, 0.0, 0.0]
                rx, ry, rz = gp[0], gp[2], gp[1]  # game (x,y,z) -> render (x,z,y)
                yaw = float(p.get("yaw", 0.0))
                for name in names:
                    instances.append((name, (rx, ry, rz), yaw, 1.0))
                n_prop_inst += 1
                n_inst += 1
            log(f"props: cooked {n_prop_models} distinct models -> {n_prop_inst} instances "
                f"({n_prop_skip} models skipped: unsupported mesh/no body; "
                f"{n_backdrop} backdrop-vista entities skipped)")
        except Exception as exc:  # noqa: BLE001
            log(f"  props skip ({type(exc).__name__}: {exc})")

    # Local point lights: read the level's lamp/lantern/brazier/placeable light entities
    # (ghidra_out/level_lights_effects_re.txt §1) and emit `light` records. Cap the count
    # so the b1 cbuffer array (64) isn't exceeded — keep the brightest (I*R^2 ~ reach).
    scene_lights = []  # [(px,py,pz, r,g,b, range, intensity)] render-space, colour 0..1
    if lights and lightdump:
        try:
            found = read_lights(npc_level_save, npc_level_gdb, lightdump,
                                globals_gdb=globals_gdb, log=log)
            for L in found:
                gp = L.get("pos") or [0.0, 0.0, 0.0]
                rx, ry, rz = gp[0], gp[2], gp[1]  # game (x,y,z) -> render (x,z,y)
                rgb = L.get("rgb") or [255, 255, 255]
                cr, cg, cb = rgb[0] / 255.0, rgb[1] / 255.0, rgb[2] / 255.0
                rng = float(L.get("range", 0.0))
                inten = float(L.get("intensity", 1.0))
                if rng <= 0.0 or inten <= 0.0:
                    continue
                scene_lights.append((rx, ry, rz, cr, cg, cb, rng, inten))
            # Rank by approximate visual reach (intensity * range^2) and cap.
            cap = light_limit if (light_limit and light_limit > 0) else 64
            scene_lights.sort(key=lambda l: l[7] * l[6] * l[6], reverse=True)
            if len(scene_lights) > cap:
                log(f"lights: capping {len(scene_lights)} -> {cap} brightest (b1 cbuffer cap)")
                scene_lights = scene_lights[:cap]
        except Exception as exc:  # noqa: BLE001
            log(f"  lights skip ({type(exc).__name__}: {exc})")

    # Cook the referenced albedo textures (globals_textures.bnk .tex -> loose DDS). The runtime
    # only samples albedo (t0), so albedo is what turns the flat-grey buildings textured.
    albedo_tokens = []
    for _name, opts, _base in materials:
        for o in opts:
            if o.startswith("albedo=") or o.startswith("normal="):
                # normal maps cook too now (cook_lh_tex decodes comp-3 BC5 -> RGBA8 DDS,
                # which the runtime loader reads); the world PS samples t1 for bump detail.
                albedo_tokens.append(o.split("=", 1)[1])
    tex_sources = [b for b in (textures_bnks or []) if b]
    tex_map = _cook_textures(albedo_tokens, tex_sources, tex_cook, f2tool,
                             tex_out_dir or (out_scene.parent / (out_scene.stem + ".textures")),
                             tmp, log=log) if (tex_sources and tex_cook) else {}
    n_tex = sum(1 for v in tex_map.values() if v)
    if tex_sources and tex_cook:
        log(f"cooked {n_tex}/{len(tex_map)} distinct albedo textures -> DDS")

    # Emit F2SCENE (matches native_scene.cpp load_native_scene grammar).
    with out_scene.open("w", encoding="utf-8", newline="\n") as out:
        out.write(f"# Cooked from {engine_level.name} (v{info['version']}) — "
                  f"{len(meshes)} meshes / {n_inst} instances / {n_blocks} blocks\n")
        out.write("F2SCENE 1\n")
        # Real chapter2slums midday theme (ghidra_out/env_theme_colors_re.txt, from
        # environmentthemes.gdb, BE bytes /255): sun = light DIRECTION = -sun_toward
        # (sun_toward Y-up = 0.9135,0.4067,0.0048); sky zenith blue (167,208,255).
        out.write("sun -0.9135 -0.4067 -0.0048\n")
        out.write("sky 0.6549 0.8157 1.0 1\n")
        for name, opts, base in materials:
            # Repoint albedo at the cooked loose DDS (absolute path; the runtime loads it
            # directly). Drop albedo tokens that didn't cook so the material shows its flat
            # base colour instead of the unresolved .tex name (which would sample white).
            emit = []
            for o in opts:
                if o.startswith("albedo_abs="):
                    # Pre-baked absolute DDS (e.g. the terrain splat composite) — emit as the
                    # albedo directly; it was never a bnk .tex token so skip the cook/repoint.
                    emit.append("albedo=" + o[len("albedo_abs="):].replace("\\", "/"))
                elif o.startswith("albedo="):
                    dds = tex_map.get(o[len("albedo="):], "")
                    if dds:
                        emit.append("albedo=" + dds.replace("\\", "/"))
                    # else: skip albedo -> flat base colour fallback
                elif o.startswith("normal="):
                    dds = tex_map.get(o[len("normal="):], "")
                    if dds:
                        emit.append("normal=" + dds.replace("\\", "/"))
                    # else: drop -> PS falls back to the geometric normal
                elif o.startswith("material="):
                    continue  # spec map not sampled yet (renderer t2 TODO)
                else:
                    emit.append(o)
            out.write(f"material {name} {base[0]:.4g} {base[1]:.4g} {base[2]:.4g} {base[3]:.4g}"
                      + ("".join(" " + o for o in emit)) + "\n")
        for name, mat_idx, positions, normals, uvs, indices in meshes:
            out.write(f"mesh {name} {len(positions) // 3} {len(indices)} {mat_idx}\n")
            for vi in range(len(positions) // 3):
                px, py, pz = positions[vi * 3:vi * 3 + 3]
                nx, ny, nz = normals[vi * 3:vi * 3 + 3]
                u, v = (uvs[vi * 2:vi * 2 + 2] if uvs else (0.0, 0.0))
                # Model verts are in game space (Z-up); the engine renders Y-up via
                # game_vec_to_xform_axes(x,y,z)={x,z,y}. Apply the SAME swap the instance
                # positions use, so buildings stand upright instead of lying sideways.
                out.write(f"vertex {px:.9g} {pz:.9g} {py:.9g} {nx:.9g} {nz:.9g} {ny:.9g} "
                          f"{u:.9g} {v:.9g}\n")
            for idx in indices:
                out.write(f"index {idx}\n")
        for name, pos, yaw, scale in instances:
            out.write(f"instance {name} {pos[0]:.9g} {pos[1]:.9g} {pos[2]:.9g} "
                      f"0 {yaw:.9g} 0 {scale:.9g}\n")
        for (px, py, pz, cr, cg, cb, rng, inten) in scene_lights:
            out.write(f"light {px:.9g} {py:.9g} {pz:.9g} {cr:.6g} {cg:.6g} {cb:.6g} "
                      f"{rng:.9g} {inten:.6g}\n")

    log(f"cooked {len(meshes)} meshes, {n_inst} instances ({n_blocks} blocks), "
        f"{len(scene_lights)} lights -> {out_scene}")
    return {"meshes": len(meshes), "instances": n_inst, "blocks": n_blocks,
            "lights": len(scene_lights)}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("engine_level", type=Path, help="path to a *.engine_level file")
    ap.add_argument("--json", type=Path, help="write the full manifest as JSON")
    ap.add_argument("--cook", type=Path, metavar="OUT.f2scene",
                    help="stage 2: glue prop models + merge instances into one F2SCENE")
    ap.add_argument("--header-bnk", type=Path, help="globals_model_headers.bnk (MeshFile headers)")
    ap.add_argument("--body-bnk", type=Path, help="the level's <scenario>_models.bnk (polymsh bodies)")
    ap.add_argument("--f2tool", type=Path,
                    default=Path(__file__).resolve().parents[2] / "Fable2AssetBrowser" / "source"
                    / "build" / "f2tool.exe", help="f2tool.exe (decompresses body entries)")
    ap.add_argument("--types", default="2,21", help="prop block types to cook (default 2,21)")
    ap.add_argument("--max-per-block", type=int, help="cap instances per block (for quick tests)")
    ap.add_argument("--textures-bnk", type=Path, action="append", dest="textures_bnk",
                    help="a .tex source bnk (repeatable: globals_textures.bnk, the level's textures.bnk). "
                         "Searched in order.")
    ap.add_argument("--tex-cook", type=Path,
                    default=Path(__file__).resolve().parents[1] / "build" / "RelWithDebInfo"
                    / "f2native_cook_lh_tex.exe",
                    help="f2native_cook_lh_tex.exe (.tex -> DDS decoder)")
    ap.add_argument("--tex-out-dir", type=Path,
                    help="where to write cooked albedo DDS (default <scene>.textures/)")
    ap.add_argument("--terrain-ghf", type=Path,
                    help="the level's extracted .ghf heightfield -> emit a ground mesh")
    ap.add_argument("--terrain-ehf", type=Path,
                    help="the level's main heightfield .ehf -> texture the terrain from its "
                         "LOD table + splat map (full splat composite by default)")
    ap.add_argument("--no-terrain-splat", dest="terrain_splat", action="store_false",
                    help="disable the full splat composite; use the single dominant ground "
                         "texture (Rung 1) instead")
    ap.add_argument("--terrain-splat-res", type=int, default=2048,
                    help="splat-composite baked albedo resolution NxN (default 2048)")
    ap.add_argument("--splat-bake", type=Path,
                    default=Path(__file__).resolve().parents[2] / "Fable2AssetBrowser" / "source"
                    / "build" / "terrain_splat_bake.exe",
                    help="terrain_splat_bake.exe (the .ehf splat-composite baker)")
    ap.add_argument("--terrain-stride", type=int, default=1,
                    help="terrain grid decimation (1=full-res 664K tris; 2/4 for lighter)")
    ap.add_argument("--hero", action="store_true",
                    help="cook the child hero (CH_HeroChild_Male) into the scene")
    ap.add_argument("--hero-model",
                    default=r"Art\Characters\Heros\Child Male\dotXSI\CH_HeroChild_Male\CH_HeroChild_Male.mdl",
                    help="hero model path (in globals_model_headers.bnk + --hero-body-bnk)")
    ap.add_argument("--hero-body-bnk", type=Path, help="globals_models.bnk (hero polymsh bodies)")
    ap.add_argument("--hero-pos", type=float, nargs=3, metavar=("X", "Y", "Z"),
                    help="hero render-space position (default: building centroid)")
    ap.add_argument("--water-file", type=Path,
                    help="the level's extracted .water file -> flat water planes")
    ap.add_argument("--npcs", action="store_true",
                    help="populate the town with bind-pose villagers at creature-spawn markers")
    ap.add_argument("--npc-body-bnk", type=Path,
                    help="globals_models.bnk (villager polymsh bodies; usually = --hero-body-bnk)")
    ap.add_argument("--npc-markerdump", type=Path,
                    default=Path(__file__).resolve().parents[2] / "Fable2AssetBrowser" / "source"
                    / "build" / "npc_markerdump.exe",
                    help="npc_markerdump.exe (reads .save+.gdb creature markers)")
    ap.add_argument("--level-save", type=Path,
                    help="the level's extracted .save (XML entity name->GUID registry)")
    ap.add_argument("--level-gdb", type=Path,
                    help="the level's extracted .gdb (per-GUID record table)")
    ap.add_argument("--npc-limit", type=int,
                    help="cap the number of villagers cooked (first-pass; e.g. 20)")
    ap.add_argument("--npc-female", action="store_true",
                    help="use the child-female part set instead of child-male")
    ap.add_argument("--lights", action="store_true",
                    help="cook the level's local point lights (lamp posts, lanterns, braziers)")
    ap.add_argument("--lightdump", type=Path,
                    default=Path(__file__).resolve().parents[2] / "Fable2AssetBrowser" / "source"
                    / "build" / "lightdump.exe",
                    help="lightdump.exe (reads .save+.gdb light entities)")
    ap.add_argument("--globals-gdb", type=Path,
                    help="data/Globals/globals.gdb (light Colour/LightType archetypes chain here)")
    ap.add_argument("--light-limit", type=int,
                    help="cap the number of cooked lights (default 64 = b1 cbuffer array size)")
    ap.add_argument("--props", action="store_true",
                    help="cook the level's renderable .gdb/.save ENTITY props (furniture, crates, "
                         "walls, railings, townhouse facades — the town's real density)")
    ap.add_argument("--propdump", type=Path,
                    default=Path(__file__).resolve().parents[2] / "Fable2AssetBrowser" / "source"
                    / "build" / "propdump.exe",
                    help="propdump.exe (resolves .gdb entity -> model + transform)")
    ap.add_argument("--vista-ehf", type=Path,
                    help="the level's extracted sea/backdrop .ehf -> emit a distant vista mesh")
    ap.add_argument("--prop-limit", type=int,
                    help="cap the number of DISTINCT prop models cooked (first-pass; e.g. 40)")
    args = ap.parse_args()

    data = args.engine_level.read_bytes()
    info = parse_engine_level(data)

    total_inst = sum(len(b["instances"]) for b in info["prop_blocks"])
    # Distinct models = the stage-2 cook workload: each unique MDL is cooked once,
    # then reused across all its instances (32 meshes vs 7011 for chapter2slums).
    distinct = sorted({b["model"].lower() for b in info["prop_blocks"] if b.get("model")})
    print(f"LevelGraphicsFile v{info['version']}  entries={info['entry_count']}  "
          f"entry_types={info['entry_types']}")
    print(f"prop_blocks={len(info['prop_blocks'])}  distinct_models={len(distinct)}  "
          f"total_instances={total_inst}")
    for b in info["prop_blocks"][:20]:
        first = b["instances"][0]["pos"] if b["instances"] else []
        print(f"  [type {b['kind']:>2}] {len(b['instances']):>5} x  {b['model']}"
              + (f"   first_pos={[round(v, 2) for v in first]}" if first else ""))
    if len(info["prop_blocks"]) > 20:
        print(f"  ... and {len(info['prop_blocks']) - 20} more prop blocks")

    if args.json:
        args.json.write_text(json.dumps(info, indent=1), encoding="utf-8")
        print(f"-> {args.json}")

    if args.cook:
        if not args.header_bnk or not args.body_bnk:
            ap.error("--cook requires --header-bnk and --body-bnk")
        types = tuple(int(t) for t in args.types.split(","))
        tex_cook = args.tex_cook if (args.tex_cook and args.tex_cook.is_file()) else None
        cook_level(args.engine_level, args.header_bnk, args.body_bnk, args.f2tool,
                   args.cook, types=types, max_per_block=args.max_per_block,
                   textures_bnks=args.textures_bnk, tex_cook=tex_cook,
                   tex_out_dir=args.tex_out_dir, terrain_ghf=args.terrain_ghf,
                   terrain_ehf=args.terrain_ehf, terrain_splat=args.terrain_splat,
                   terrain_splat_res=args.terrain_splat_res, splat_bake=args.splat_bake,
                   terrain_stride=args.terrain_stride,
                   hero_model=args.hero_model if args.hero else None,
                   hero_body_bnk=args.hero_body_bnk, hero_pos=args.hero_pos,
                   water_file=args.water_file, npcs=args.npcs,
                   npc_body_bnk=args.npc_body_bnk or args.hero_body_bnk,
                   npc_markerdump=args.npc_markerdump,
                   npc_level_save=args.level_save, npc_level_gdb=args.level_gdb,
                   npc_limit=args.npc_limit,
                   npc_parts=(NPC_CHILD_FEMALE_PARTS if args.npc_female else NPC_CHILD_MALE_PARTS),
                   lights=args.lights,
                   lightdump=args.lightdump if (args.lightdump and args.lightdump.is_file()) else None,
                   globals_gdb=args.globals_gdb, light_limit=args.light_limit,
                   props=args.props,
                   propdump=args.propdump if (args.propdump and args.propdump.is_file()) else None,
                   prop_limit=args.prop_limit, vista_ehf=args.vista_ehf)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
