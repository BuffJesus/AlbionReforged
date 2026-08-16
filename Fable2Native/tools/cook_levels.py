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
                # `h` (u64) = PropInstance.hash — the key into the level's .lmp LightmapFile
                # per-instance baked lighting probes (LevelEdit.cpp patch_lmp_probes @762).
                insts.append({"pos": values[0:3], "values": values, "hash": h})
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
                   out_dir: Path, tmp: Path, texture_headers_bnks=None, log=print) -> dict:
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
    header_sources = [b for b in (texture_headers_bnks or []) if b]
    header_indices = [(b, _bnk_name_index(b)) for b in header_sources]
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
            cook_args = [str(tex_cook), str(raw), str(dds)]
            # Some globals textures are bare payloads paired with an entry in
            # globals_texture_headers.bnk rather than self-describing LhTex mips. Supply
            # PF/width/height metadata to the native cooker when that header is available.
            for hb, hidx in header_indices:
                h_exact = _resolve(hidx, token)
                if not h_exact:
                    continue
                hraw = tmp / f"{stem}.texture_header"
                subprocess.run([str(f2tool), "extract", str(hb), h_exact, str(hraw)],
                               check=True, capture_output=True)
                header = hraw.read_bytes()
                if len(header) >= 0x1c:
                    pf = struct.unpack_from(">I", header, 0x18)[0]
                    hw = struct.unpack_from(">I", header, 0x10)[0]
                    hh = struct.unpack_from(">I", header, 0x14)[0]
                    if pf and hw and hh:
                        cook_args += ["--pf", str(pf), "--width", str(hw), "--height", str(hh)]
                break
            result = subprocess.run(cook_args, capture_output=True, text=True)
            if result.returncode != 0 or not dds.is_file():
                log(f"  tex skip ({result.stdout.strip() or result.stderr.strip()}): {token}")
                continue
        except Exception as exc:  # noqa: BLE001 - skip-and-continue
            log(f"  tex skip ({type(exc).__name__}): {token}")
            continue
        cooked[token] = str(dds.resolve())
    return cooked


def _fnv1_env(s: str, lower: bool) -> int:
    """EnvironmentTextureHash (SkyboxPreviewBinding.cpp): FNV-1 (mul-then-xor) over a texture
    path with '/'->'\\' and optional lowercasing. The theme stores this hash for its textures."""
    if lower:
        s = s.lower()
    s = s.replace("/", "\\")
    h = 0x811C9DC5
    for ch in s.encode("utf-8", "ignore"):
        h = (h * 0x01000193) & 0xFFFFFFFF
        h ^= ch
    return h


def _resolve_env_texture_hash(target_hash: int, texture_bnks) -> str:
    """Map a theme texture GUID back to its exact .tex bnk entry name by hashing every entry
    name (4 variants: full-path/leaf x lower/as-is), mirroring ResolveEnvironmentTextureHash."""
    if not target_hash or target_hash == 0x811C9DC5:
        return None
    for b in texture_bnks or []:
        if not b:
            continue
        try:
            by_norm, _ = _bnk_name_index(b)
        except Exception:  # noqa: BLE001 - a bad bnk just yields no match
            continue
        for exact in by_norm.values():
            leaf = exact.replace("/", "\\").rsplit("\\", 1)[-1]
            for cand in (exact, leaf):
                for lc in (True, False):
                    if _fnv1_env(cand, lc) == target_hash:
                        return exact
    return None


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


def _load_lmp_probes(lmp_path: Path, log=print):
    """Load the level's .lmp LightmapFile per-prop-instance baked lighting probes.

    RE'd (no guessing): the .lmp is gzip of inner magic "LightmapFile"; the tail holds
    n records x 56 bytes, preceded by a u32-BE count `n` at (total - 56*n - 4). Each
    record = [8-byte PropInstance.hash BE][48-byte payload = 12 BE floats]. The 12 floats
    are order-1 SPHERICAL HARMONICS per RGB, channel-major: [R0 R1 R2 R3][G0..][B0..],
    coeff0 = DC/ambient. Authorities: LevelEdit.cpp patch_lmp_probes@762 (record layout) +
    guest loader Function_82A55140 (48B verbatim, hash-keyed rb-tree) + the shipped Xenos
    shader VSHADER_STANDARDMATERIAL_..._AMB2 (const g_PRTConstants) which gives the EXACT
    eval (ghidra_out/prop_ambient_shader_re.txt):  amb.c = C0 + (N.x*C1 + N.y*C2 + N.z*C3)
    per channel, against the OBJECT-space normal, NO scale (coeffs pre-folded at bake). The
    renderer evaluates this per-vertex. Returns {hash(u64): (12 floats)} in channel-major order.
    """
    import gzip
    try:
        raw = gzip.decompress(Path(lmp_path).read_bytes())
    except Exception as exc:  # noqa: BLE001
        log(f"  lmp skip ({type(exc).__name__}: {exc})")
        return {}
    if raw[:12] != b"LightmapFile":
        log("  lmp skip (magic mismatch)")
        return {}
    total = len(raw)
    n = 0
    c = (total - 4) // 56
    while c >= 1:
        pos = total - 56 * c - 4
        if pos >= 16 and struct.unpack_from(">I", raw, pos)[0] == c:
            n = c
            break
        c -= 1
    if not n:
        log("  lmp skip (probe section not found)")
        return {}
    sec = total - 56 * n
    probes = {}
    for i in range(n):
        off = sec + i * 56
        h = struct.unpack_from(">Q", raw, off)[0]
        probes[h] = struct.unpack_from(">12f", raw, off + 8)
    log(f"  lmp: {n} baked lighting probes loaded")
    return probes




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
    Faithful port of ghidra_out/water_system_re.txt (BE). Returns authored body tuples
    (pos,nrm,uv,idx,normal_map,params) or None."""
    MARKER = 0x00000FEC
    r = BeReader(water_bytes)
    version = r.u32()
    body_count = r.u32()
    if version != 2 or not body_count or body_count > 256:
        return None  # empty/vista placeholder or corrupt
    offsets = [r.u32() for _ in range(body_count)]
    bodies = []
    for off in offsets:
        if off is None or off >= r.n:
            continue
        r.i = off
        if r.u32() != MARKER:
            continue
        r.f32()                                   # param_a
        base_h = r.f32()
        params = [r.f32() for _ in range(37)]     # WaterFile::params, shader-side
        body_normal_map = r.cstr()                # normal_map path
        body_normal_map = body_normal_map.replace("\\", "/") if body_normal_map else ""
        r.cstr()                                  # secondary_map path
        patch_count = r.u32()
        if patch_count is None or patch_count > 4096:
            continue
        pos, nrm, uv, idx = [], [], [], []
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
        if idx:
            bodies.append((pos, nrm, uv, idx, body_normal_map, params))
    return bodies or None


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
    """Cook a flat sea/backdrop .ehf vista mesh at its authored footprint.
    ghidra_out/ehf_vista_re.txt: the 63-byte
    BE header carries origin (f0/f1), grid dims (u0/u1) and tile (f2); the render surface is
    a flat plane whose height is the constant repeated across the body's 850A0 vertices.
    Returns (pos,nrm,uv,idx) in GAME space (X, Y, height) — the writer applies the {x,z,y}
    swap. Flat-vista path only (the sea_vista is planar).

    The mesh remains at the authored footprint and is a backdrop/vista surface, not a
    substitute for a shipped `.water` body. In particular, do not widen `sea_vista.ehf`
    across the castle approach: that creates a second surface underneath the real
    `sea_vista.water` and invents geometry in the backdrop/skybox region."""
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

    # SLOPED-VISTA path (ghidra_out/ehf_vista_re.txt §3/§4B): the body's "850A0" patches carry
    # the DISTANT-LOD render vertices (8 verts per 160-byte subcell). Deduping their (x,y)
    # recovers a coarse grid whose z is the real terrain height — so a sloped vista (distant
    # hills, e.g. spring_vista) gets real elevated geometry instead of a flat plane. The
    # sea_vista is planar (all z == height) so this yields the SAME flat result; any parse
    # deviation falls back to the header grid below. Returns None from the helper on any issue.
    def _coarse_from_850a0():
        try:
            n850 = struct.unpack_from(">I", ehf_bytes, p + 4)[0]
            if not (0 < n850 <= 4096):
                return None
            q = p + 8
            verts = {}  # (xr,yr) -> list of heights
            for _ in range(n850):
                fa, fb, w_sub, h_sub = struct.unpack_from(">ffII", ehf_bytes, q)
                q += 16
                if not (0 < w_sub * h_sub <= 1 << 20):
                    return None
                for _s in range(w_sub * h_sub):
                    base = q + 0x40  # skip the 64-byte bbox header; 8 verts follow
                    for vi in range(8):
                        vx, vy, vz = struct.unpack_from(">fff", ehf_bytes, base + vi * 12)
                        if math.isfinite(vx) and math.isfinite(vy) and math.isfinite(vz):
                            verts.setdefault((round(vx, 2), round(vy, 2)), []).append(vz)
                    q += 160
                q += 24  # patch aabb_min[3] + aabb_max[3]
            if len(verts) < 4:
                return None
            xs = sorted({k[0] for k in verts})
            ys = sorted({k[1] for k in verts})
            if len(xs) < 2 or len(ys) < 2 or len(xs) * len(ys) > 1 << 18:
                return None

            def _h(gx, gy):
                hs = verts.get((gx, gy))
                if hs:
                    hs = sorted(hs)
                    return hs[len(hs) // 2]  # median (skirt/neighbour samples are outliers)
                return None
            # Only worth the coarse mesh if the surface is actually sloped; else fall back to
            # the trivial flat header grid (identical result, simpler).
            allh = [h for h in (_h(gx, gy) for gx in xs for gy in ys) if h is not None]
            if not allh or (max(allh) - min(allh)) < 1.0:
                return None
            cpos, cnrm, cuv, cidx = [], [], [], []
            node = {}
            for gy in ys:
                for gx in xs:
                    hh = _h(gx, gy)
                    if hh is None:  # fill a gap with the column/row nearest value
                        hh = min(allh)
                    node[(gx, gy)] = len(cpos) // 3
                    cpos.extend((gx, gy, hh))
                    cnrm.extend((0.0, 0.0, 1.0))
                    cuv.extend((gx * 0.02, gy * 0.02))
            for j in range(len(ys) - 1):
                for i in range(len(xs) - 1):
                    v00 = node[(xs[i], ys[j])]
                    v10 = node[(xs[i + 1], ys[j])]
                    v01 = node[(xs[i], ys[j + 1])]
                    v11 = node[(xs[i + 1], ys[j + 1])]
                    cidx.extend((v00, v01, v10, v10, v01, v11))
            return (cpos, cnrm, cuv, cidx) if cidx else None
        except (struct.error, ValueError, IndexError):
            return None

    coarse = _coarse_from_850a0()
    if coarse:
        return coarse

    # FLAT-VISTA path (sea_vista): header grid at the authored constant plane height and
    # footprint. Real water is parsed separately from the matching .water file.
    nx = u0
    pos, nrm, uv, idx = [], [], [], []
    for cy in range(u1):
        for cx in range(nx):
            px, py = ox + cx * tile, oy + cy * tile
            pos.extend((px, py, height))
            nrm.extend((0.0, 0.0, 1.0))
            uv.extend((px * 0.02, py * 0.02))
    for j in range(u1 - 1):
        for i in range(nx - 1):
            v00 = j * nx + i
            v10 = v00 + 1
            v01 = (j + 1) * nx + i
            v11 = v01 + 1
            idx.extend((v00, v01, v10, v10, v01, v11))
    return (pos, nrm, uv, idx) if idx else None


def resolve_genv_theme(genv_path: Path, env_gdb_path: Path,
                       tod_hours: float = 12.0, log=print):
    """Failure-safe wrapper: any parse error -> None (falls back to hardcoded theme)."""
    try:
        return _resolve_genv_theme_impl(genv_path, env_gdb_path, tod_hours, log)
    except Exception as e:  # corrupt/truncated .genv or .gdb must never crash the cook
        log(f"genv theme: parse failed ({type(e).__name__}: {e}); using hardcoded theme")
        return None


def _resolve_genv_theme_impl(genv_path: Path, env_gdb_path: Path,
                             tod_hours: float, log):
    """Resolve a level's representative environment theme from its per-cell .genv grid.

    .genv STRUCTURE (byte-verified against slums.genv, and independently re-derived):
    a 0x20 header + a dim x dim (dim @0x14/0x18 = 72) grid of BE-u32 cells (row
    stride @0x0C/0x10 = dim*4 = 0x120, cell world-scale float @0x1C = 4.0), where
    0xFFFFFFFF = no zone and every other value is an environmentthemes.gdb record
    GUID = a per-region EnvironmentThemeDaySet. Each DaySet holds per-hour entry
    records {TimeOfDay(hours 0..24), Theme ref}; the theme's Sky/Lighting sub-records
    (via 64-deep Parent inheritance) carry the colours. This CORRECTS the stale
    env_theme_colors_re.txt §4 note that called these values "coincidental floats".

    The colour/inheritance reads faithfully mirror the shipped Fable2AssetBrowser
    EnvironmentThemeParser (findField Parent walk, normalizeTimeOfDay hours->0..1,
    readColour sub-record-OR-flat, clamp01(v/255)*Factor).

    HEURISTIC (cook-only, NOT engine parity): the retail engine does NOT sample
    .genv per-cell — it blends one level-wide DaySet by the game clock. For a
    STATIC bake we pick the DOMINANT (largest) zone's DaySet as the play-area's
    representative theme (verified: the town's 457-cell zone; PlayerStart-cell
    sampling would land in an undefined cell / the black backdrop zone here, so it
    is deliberately NOT used). Then select its nearest-to-`tod_hours` theme and
    return the values cook_level emits as sun/sunlight/sky.

    Returns a dict (sun_dir, sunlight, sky, plus raw params) or None on failure.
    """
    import struct as _st, math as _m
    try:
        gv = genv_path.read_bytes()
        gd = env_gdb_path.read_bytes()
    except OSError as e:
        log(f"genv theme: cannot read inputs ({e}); using hardcoded theme")
        return None
    beU = lambda b, o: _st.unpack_from(">I", b, o)[0]
    beF = lambda b, o: _st.unpack_from(">f", b, o)[0]

    # --- .genv grid: dominant zone GUID by cell count -------------------------
    stride = beU(gv, 0x0C); dim = beU(gv, 0x14)
    if dim <= 0 or dim > 4096 or stride != dim * 4 or 0x20 + dim * stride > len(gv):
        log(f"genv theme: unexpected .genv header (dim={dim} stride={stride}); skipping")
        return None
    from collections import Counter
    cells = Counter()
    for r in range(dim):
        row = 0x20 + r * stride
        for c in range(dim):
            v = beU(gv, row + c * 4)
            if v != 0xFFFFFFFF:
                cells[v] += 1
    if not cells:
        log("genv theme: no zones in .genv; skipping")
        return None

    # --- environmentthemes.gdb reader (§1 recipe, big-endian) -----------------
    if gd[0:4] != b"GDB\x00":
        log("genv theme: env gdb bad magic; skipping"); return None
    count = beU(gd, 0x04); size_a = beU(gd, 0x08); size_b = beU(gd, 0x0C)
    schema_base = 0x18 + size_a; hash_base = schema_base + size_b
    keys = [beU(gd, hash_base + i * 4) for i in range(count)]
    rec_off = []; cur = 0x18
    for _ in range(count):
        rec_off.append(cur); so = schema_base + beU(gd, cur); fc = beU(gd, so) >> 8
        if fc > 256:
            fc = (gd[so] | (gd[so + 1] << 8)) + gd[so + 2]
        cur += 4 + fc * 4

    def lookup(g):
        lo, hi = 0, count - 1
        while lo <= hi:
            m = (lo + hi) // 2
            if keys[m] == g: return m
            if keys[m] < g: lo = m + 1
            else: hi = m - 1
        return -1

    def fields(rec):
        so = schema_base + beU(gd, rec); fc = beU(gd, so) >> 8
        if fc > 256:
            fc = (gd[so] | (gd[so + 1] << 8)) + gd[so + 2]
        H = [beU(gd, so + 4 + i * 4) for i in range(fc)]
        D = [beU(gd, so + 4 + fc * 4 + i * 4) for i in range(fc)]
        return H, D

    kParent = 0x5F6317D5

    def find_local(rec, fh, etype):
        H, D = fields(rec)
        for i, h in enumerate(H):
            if h == fh:
                t = D[i] >> 24
                if etype == 0xFF or t == etype:
                    return (t, rec + 4 + i * 4)
        return None

    def find_field(rec, fh, etype=0xFF):
        cur = rec; seen = set()
        for _ in range(64):
            if cur in seen: return None
            seen.add(cur)
            r = find_local(cur, fh, etype)
            if r: return r
            p = find_local(cur, kParent, 6)  # oracle: Parent is strictly type-6
            if not p: return None
            praw = beU(gd, p[1])
            if praw == 0: return None
            pi = lookup(praw)
            if pi < 0: return None
            cur = rec_off[pi]
        return None

    def to_record(res):
        t, vo = res; raw = beU(gd, vo)
        if raw == 0 or t not in (4, 6, 7): return None
        i = lookup(raw); return rec_off[i] if i >= 0 else None

    def resolve_ref(rec, fh):
        r = find_field(rec, fh, 0xFF); return to_record(r) if r else None

    def read_float(rec, fh):
        r = find_field(rec, fh, 3)
        if not r: return None
        v = beF(gd, r[1])
        return v if _m.isfinite(v) else None  # oracle readFloat rejects non-finite

    def clamp01(x): return 0.0 if x < 0 else 1.0 if x > 1 else x

    kRed, kGreen, kBlue, kFactor = 0x3A232172, 0x608C9792, 0xB1911CC9, 0xBF21DA70

    def read_colour(rec, rh, gh, bh):
        r = read_float(rec, rh); g = read_float(rec, gh); b = read_float(rec, bh)
        if None in (r, g, b): return None
        return [clamp01(r / 255.0), clamp01(g / 255.0), clamp01(b / 255.0)]

    def read_colour_subrec(rec, fh):
        c = resolve_ref(rec, fh)
        if c is None: return None
        o = read_colour(c, kRed, kGreen, kBlue)
        if o is None: return None
        f = read_float(c, kFactor)
        if f and f > 0: o = [x * f for x in o]
        return o

    def read_flat(rec, rh, gh, bh, fh):
        o = read_colour(rec, rh, gh, bh)
        if o is None: return None
        f = read_float(rec, fh) if fh else None
        if f and f > 0: o = [x * f for x in o]
        return o

    kSky, kLighting, kMain = 0x2420BFA4, 0x0B152C5D, 0x40A12D92
    kSkyColour = 0xD78A6E40
    SR, SG, SB, SF = 0x86B2D6AD, 0x0E4C7541, 0xFDCC27B4, 0x1273DB31
    MR, MG, MB, MF = 0x9F76036F, 0xE3D88F9B, 0x3E7D387A, 0xE67DD6DB
    kSunInt, kElev, kZoff, kXY = 0xC868C0DC, 0x2682515B, 0x2EF474B9, 0x2E4D729C
    kRayleigh, kMie = 0x59837340, 0xD7FC122C  # BetaRayleighMult / BetaMieMult
    kTOD, kTheme = 0x9723C2C9, 0xB57E3290
    # complementary (horizon) + sunset colours: sub-record OR flat fields
    kCompl, kComplBias = 0x5CBE1462, 0x2DA0C989
    CR, CG, CB, CF = 0x507FFA3F, 0xDA12836B, 0x0593BF6A, 0xD433126B
    kSunset = 0x897262B7
    UR, UG, UB, UF = 0x896E8E84, 0x8FC37AA4, 0xCFED8B8F, 0xB2447252
    # fogging: the Fogging sub-record carries CloseFogColour + near/far fog control points
    kFogging = 0xDDF56C9A
    kFogColour = 0x66353755
    FR, FG, FB, FF = 0x2229E682, 0x4D9D5A22, 0x876515F9, 0x010020C0
    kFogStart, kNearDist, kFarDist, kFarDens = 0x754D898A, 0xDA3F7AAA, 0xFF154645, 0xE1DF20B4

    def norm_tod(t):
        if not _m.isfinite(t): return 0.5
        if 1.0 < t <= 24.0: t *= 1.0 / 24.0
        t -= _m.floor(t)
        return t + 1.0 if t < 0 else t

    def select_theme(dayset_rec, want):
        """selectThemeFromDaySet: nearest normalized TimeOfDay to `want` (0..1)."""
        best = None; best_d = 1e9
        H, D = fields(dayset_rec)
        for i, h in enumerate(H):
            if (D[i] >> 24) not in (4, 6, 7) or h == kParent: continue
            entry = to_record((D[i] >> 24, dayset_rec + 4 + i * 4))
            if entry is None: continue
            theme = resolve_ref(entry, kTheme)
            if theme is None: continue
            rt = read_float(entry, kTOD)
            t = norm_tod(rt) if rt is not None else 0.5
            d = abs(t - want)
            if d < best_d: best_d, best = d, (theme, t)
        return best

    # helper: resolve a theme's SkyColour (sub-record OR flat, per the oracle)
    def theme_sky(theme):
        sr = resolve_ref(theme, kSky) or theme
        return read_colour_subrec(sr, kSkyColour) or read_flat(sr, SR, SG, SB, SF)

    # Dominant zone -> dayset -> nearest-TOD theme. Skip zones whose selected theme
    # is degenerate (all-zero/black sky = interior/backdrop, e.g. slums zone B): the
    # dominant-by-count proxy is only safe if we reject the degenerate backdrop zones,
    # otherwise a level with a large black zone would bake a black sky.
    want = norm_tod(float(tod_hours))
    best_theme = None; picked_zone = None; fallback = None
    for zone_guid, n in cells.most_common():
        zi = lookup(zone_guid)
        if zi < 0: continue
        sel = select_theme(rec_off[zi], want)
        if sel is None: continue
        sky = theme_sky(sel[0])
        if fallback is None and sky is not None:
            fallback = (sel[0], (zone_guid, n, sel[1]))  # first resolvable, even if black
        if sky is not None and max(sky) > 1e-4:  # a real (non-black) daytime theme
            best_theme, picked_zone = sel[0], (zone_guid, n, sel[1])
            break
    if best_theme is None and fallback is not None:
        best_theme, picked_zone = fallback  # only degenerate zones exist; use one
    if best_theme is None:
        log("genv theme: no resolvable dayset in any zone; skipping")
        return None

    sky_rec = resolve_ref(best_theme, kSky) or best_theme
    sky = read_colour_subrec(sky_rec, kSkyColour) or read_flat(sky_rec, SR, SG, SB, SF)
    if sky is None:
        log("genv theme: theme has no SkyColour; skipping"); return None
    # explicit None checks: a legitimately-stored 0.0 must not be clobbered by `or`
    _si = read_float(sky_rec, kSunInt); sun_int = _si if _si is not None else 1.0
    # atmosphere scattering multipliers (Hoffman-Preetham betas; SkyboxRenderer.cpp PS).
    _ray = read_float(sky_rec, kRayleigh); rayleigh = _ray if _ray is not None else 1.0
    _mie = read_float(sky_rec, kMie); mie = _mie if _mie is not None else 0.83
    elev = read_float(sky_rec, kElev) or 0.0
    zoff = read_float(sky_rec, kZoff) or 0.0
    xy = read_float(sky_rec, kXY) or 0.0
    lighting = resolve_ref(best_theme, kLighting)
    main = None
    if lighting is not None:
        main = read_colour_subrec(lighting, kMain) or read_flat(lighting, MR, MG, MB, MF)
    if main is None:
        main = [1.0, 0.902, 0.4353]  # theme-neutral warm fallback

    # Authored AMBIENT model (Lighting sub-record) — replaces native's hardcoded hemisphere.
    # AmbientColour = flat ambient (RGB/255 * Factor); SkyColourFinalBounceTop/Bottom = the
    # hemisphere sky-bounce gradient (up/down, RGB/255, no factor). fable2-theme-ambient-lighting.
    AR, AG, AB, AF = 0x727639A0, 0xF27F72A0, 0x27DDDA4B, 0xB5453B7E
    BTR, BTG, BTB = 0x6947924E, 0xDB65EC1E, 0xD465A1E5     # SkyColourFinalBounceTop R/G/B
    BBR, BBG, BBB = 0xEC904B16, 0xC4A8D3D6, 0xAD0FBA0D     # SkyColourFinalBounceBottom R/G/B
    # AmbientColour lives in the Lighting sub-record; SkyColourFinalBounceTop/Bottom live in the
    # theme's LightingPreprocessor sub-record (byte-verified on 0x72d66d23).
    kLightingPreproc = 0x43217304
    ambient_flat = read_flat(lighting, AR, AG, AB, AF) if lighting is not None else None
    preproc = resolve_ref(best_theme, kLightingPreproc)
    sky_bounce_top = read_colour(preproc, BTR, BTG, BTB) if preproc is not None else None  # /255, clamp01
    sky_bounce_bot = read_colour(preproc, BBR, BBG, BBB) if preproc is not None else None

    # horizon (complementary) + sunset tints — raw HDR (Factor can push >1); the emit
    # step display-maps them. horizon = the gradient's bottom; sunset = warm sun-halo.
    horizon = read_colour_subrec(sky_rec, kCompl) or read_flat(sky_rec, CR, CG, CB, CF)
    # sunset feeds the `sky_sunset` opcode (dawn/dusk sun-halo). compl_bias is resolved and
    # returned but not yet emitted — staged for a future gradient-bias opcode.
    sunset = read_colour_subrec(sky_rec, kSunset) or read_flat(sky_rec, UR, UG, UB, UF)
    compl_bias = read_float(sky_rec, kComplBias)

    # Fogging (env_theme_colors_re.txt §6): the theme's Fogging sub-record -> CloseFogColour +
    # near/far fog control points. Distance fog on world geometry ties it to the horizon.
    fog_rec = resolve_ref(best_theme, kFogging)
    fog_color = fog_start = fog_end = fog_max = None
    if fog_rec is not None:
        fog_color = read_colour_subrec(fog_rec, kFogColour) or read_flat(fog_rec, FR, FG, FB, FF)
        _fs = read_float(fog_rec, kFogStart)
        fog_start = _fs if _fs is not None else read_float(fog_rec, kNearDist)
        fog_end = read_float(fog_rec, kFarDist)
        fog_max = read_float(fog_rec, kFarDens)

    # sun_axis -> Y-up sun_toward (sky_system_re.txt §3), tod=want, time_factor=1.0
    theta = (want - 0.5) * 2.0 * _m.pi
    er = _m.radians(elev); phi = _m.radians(xy + zoff)
    gx = _m.cos(theta) * _m.cos(er); up = _m.cos(theta) * _m.sin(er); gz = _m.sin(theta)
    tx = gx * _m.cos(phi) - gz * _m.sin(phi)
    ty = up
    tz = gx * _m.sin(phi) + gz * _m.cos(phi)
    mag = _m.sqrt(tx * tx + ty * ty + tz * tz) or 1.0
    sun_toward = (tx / mag, ty / mag, tz / mag)
    sun_dir = (-sun_toward[0], -sun_toward[1], -sun_toward[2])  # light dir = FROM sun
    # sunlight the world PS multiplies into N.L: main_light_colour * sun_intensity
    sunlight = [c * sun_int for c in main]

    # Cloud layers (SkyboxRenderer.cpp cloud pass; EnvironmentThemeParser applyCloudThemeRecord).
    # The theme's Clouds sub-record holds up to 4 Layer records, each a density-map texture GUID
    # + scroll/shape/lighting params. finaliseCloudTheme counts a layer as enabled when it carries
    # >=1 field. We resolve them here (faithful to readCloudLayerRecord) and hand the raw params to
    # cook_level, which resolves the density GUID -> a cooked DDS and emits the cloud opcodes.
    kClouds = 0x7439046F
    kLayer = (0x6A570941, 0x6A570942, 0x6A570943, 0x6A570944)
    kDensityMap = 0x13821B7F
    kPosX, kPosY = 0x1E72B2E4, 0x1E72B2E5
    kSizeX, kSizeY = 0x9C014CCE, 0x9C014CCF
    kTexScaleX, kTexScaleY = 0x0A8BA024, 0x0A8BA025
    kVelX, kVelY = 0x5CE30740, 0x5CE30741
    kHeight, kTransparency = 0xF47DB020, 0x383FDB33
    kNormalStrength, kTranslucency = 0xB5B0AE93, 0x114E67B1
    kBrightness, kAmbient = 0xC452018C, 0x15DD1091

    def read_cloud_layer(layer_rec):
        """readCloudLayerRecord: return a dict of the layer's params (only present fields), or
        None if the record carries no cloud fields at all."""
        n = 0
        out = {}

        def p(hash_, key):
            nonlocal n
            v = read_float(layer_rec, hash_)
            if v is not None:
                out[key] = v
                n += 1

        # density map: the raw u32 field value (a texture GUID), NOT resolved to a record here.
        dm = find_field(layer_rec, kDensityMap, 0xFF)
        if dm is not None:
            raw = beU(gd, dm[1])
            if raw not in (0, 0x811C9DC5):
                out["density_hash"] = raw
                n += 1
        p(kPosX, "position_x"); p(kPosY, "position_y")
        p(kSizeX, "size_x"); p(kSizeY, "size_y")
        p(kTexScaleX, "texture_scale_x"); p(kTexScaleY, "texture_scale_y")
        p(kVelX, "velocity_x"); p(kVelY, "velocity_y")
        p(kHeight, "height"); p(kTransparency, "transparency")
        p(kBrightness, "brightness"); p(kAmbient, "ambient_light")
        p(kNormalStrength, "normal_strength"); p(kTranslucency, "translucency_strength")
        return out if n > 0 else None

    def read_clouds(theme):
        """applyCloudThemeRecord: resolve the Clouds sub-record (or the theme itself), then read
        the 4 Layer records; fall back to reading layer 0 straight off the Clouds record."""
        clouds = resolve_ref(theme, kClouds) or theme
        layers = []
        for lh in kLayer:
            lr = resolve_ref(clouds, lh)
            layers.append(read_cloud_layer(lr) if lr is not None else None)
        if not any(layers):
            direct = read_cloud_layer(clouds)
            # AB requires >=2 fields or a density map for the direct-read fallback
            if direct is not None and (len(direct) >= 2 or "density_hash" in direct):
                layers[0] = direct
        return layers

    clouds = read_clouds(best_theme)
    n_clouds = sum(1 for l in clouds if l)
    if n_clouds:
        log(f"genv theme: {n_clouds} cloud layer(s) resolved")

    # Celestial billboards (SkyboxRenderer.cpp element pass): the moon (phase billboard) + moon
    # glare + stars, read from the Sky record. Gated by moon_intensity/star_brightness so daytime
    # themes (moon_intensity=0) emit nothing. The sun disc/beams/glare are read too but chapter2slums
    # authors none; other levels/themes may. Field hashes from EnvironmentThemeParser.cpp @78-96.
    kMoonInt, kMoonSize = 0x8DA685FD, 0x771E440D
    kMoonGlareInt, kMoonGlareSize = 0x326BAB3E, 0x593E77C4
    kMoonTransp, kStarBright = 0xAF2DDBD4, 0xE52870D8
    kMoonAxisElev, kMoonAxisZoff, kMoonAxisXY = 0xF32010F6, 0x425F14D0, 0x695E86AB
    kMoonTex, kMoonGlareTex = 0x20D88F43, 0xF2C7518C

    def read_tex_ref(rec, fh):
        r = find_field(rec, fh, 0xFF)
        if not r:
            return 0
        raw = beU(gd, r[1])
        return 0 if raw in (0, 0x811C9DC5) else raw

    moon = None
    _mi = read_float(sky_rec, kMoonInt)
    moon_intensity = _mi if _mi is not None else 0.0
    if moon_intensity and moon_intensity > 0.0:
        m_elev = read_float(sky_rec, kMoonAxisElev)
        m_elev = m_elev if m_elev is not None else 26.0
        m_zoff = read_float(sky_rec, kMoonAxisZoff) or 0.0
        m_xy = read_float(sky_rec, kMoonAxisXY) or 0.0
        # moon direction (EvaluateFrame lines 831-855): same tod theta as the sun, moon axis.
        me = _m.radians(m_elev); mphi = _m.radians(m_zoff + m_xy)
        cos_t = _m.cos(theta); sin_t = _m.sin(theta)  # theta from the sun block above
        m_gx = cos_t * _m.cos(me); m_up = cos_t * _m.sin(me)
        cmp_, smp = _m.cos(mphi), _m.sin(mphi)
        mtx = -(m_gx * cmp_ - sin_t * smp)
        mty = -m_up
        mtz = -(m_gx * smp + sin_t * cmp_)
        mmag = _m.sqrt(mtx * mtx + mty * mty + mtz * mtz) or 1.0
        moon = {
            "dir": (mtx / mmag, mty / mmag, mtz / mmag),  # render-space toward the moon
            "intensity": moon_intensity,
            "size": (read_float(sky_rec, kMoonSize) if read_float(sky_rec, kMoonSize) is not None else 1.0),
            "glare_intensity": read_float(sky_rec, kMoonGlareInt) or 0.0,
            "glare_size": (read_float(sky_rec, kMoonGlareSize) if read_float(sky_rec, kMoonGlareSize) is not None else 1.0),
            "transparency": read_float(sky_rec, kMoonTransp) or 0.0,
            "moon_tex": read_tex_ref(sky_rec, kMoonTex),
            "glare_tex": read_tex_ref(sky_rec, kMoonGlareTex),
        }
        log(f"genv theme: moon (intensity {moon_intensity:.2f}, size {moon['size']:.2f})")
    _sb = read_float(sky_rec, kStarBright)
    star_brightness = _sb if _sb is not None else 0.0

    log(f"genv theme: dominant zone {picked_zone[0]:#010x} ({picked_zone[1]} cells), "
        f"theme @tod={picked_zone[2]:.3f} sky=({sky[0]:.3f},{sky[1]:.3f},{sky[2]:.3f}) "
        f"sun_int={sun_int:.2f}")
    return {"clouds": clouds, "moon": moon, "star_brightness": star_brightness,
            "sun_dir": sun_dir, "sunlight": sunlight, "sky": sky,
            "sky_colour": sky, "sun_intensity": sun_int,
            "sun_elev": elev, "main_light": main, "tod": want,
            "horizon": horizon, "sunset": sunset, "compl_bias": compl_bias,
            "fog_color": fog_color, "fog_start": fog_start,
            "fog_end": fog_end, "fog_max": fog_max,
            "rayleigh": rayleigh, "mie": mie,
            "ambient_flat": ambient_flat, "sky_bounce_top": sky_bounce_top,
            "sky_bounce_bot": sky_bounce_bot}


def cook_level(engine_level: Path, header_bnk: Path, body_bnk: Path, f2tool: Path,
               out_scene: Path, types=(2, 21), max_per_block=None, log=print,
               textures_bnks=None, texture_headers_bnks=None, tex_cook: Path = None,
               tex_out_dir: Path = None, terrain_ghf: Path = None,
               terrain_ehf: Path = None, terrain_splat: bool = True,
               terrain_splat_res: int = 2048, splat_bake: Path = None,
               level_lmp: Path = None,
               terrain_stride: int = 1, hero_model: str = None,
               hero_body_bnk: Path = None, hero_pos=None,
               water_file: Path = None, npcs: bool = False,
               npc_body_bnk: Path = None, npc_markerdump: Path = None,
               npc_level_save: Path = None, npc_level_gdb: Path = None,
               npc_limit: int = None, npc_parts=None,
               lights: bool = False, lightdump: Path = None,
               globals_gdb: Path = None, light_limit: int = None,
               props: bool = False, propdump: Path = None,
               prop_limit: int = None, vista_ehf: Path = None,
               env_theme: dict = None) -> dict:
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
    hero_start = None                   # render-space PlayerStart + yaw for inspection framing
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

    # Per-prop baked lighting probes (.lmp LightmapFile). Keyed by PropInstance.hash; the
    # DC/ambient SH term lights each static prop with its baked GI instead of the flat
    # hemisphere floor (fixes the "dark building faces"). Only type-2 props carry probes.
    lmp_probes = _load_lmp_probes(level_lmp, log=log) if level_lmp else {}

    # Level-average baked ambient: the mean of every baked probe (12 SH coeffs). The
    # directional terms (C1..C3) largely cancel across differently-oriented probes, leaving
    # essentially the level's mean DC ambient. Used as the fallback for STRUCTURES (type-2)
    # that ship WITHOUT their own probe — e.g. the Fairfax castle, whose hash isn't in the
    # .lmp — so they light like their probed neighbours instead of dropping to the synthetic
    # gray hemisphere (which, under the grazing midday sun, renders them near-black). This is
    # the level's OWN baked GI, not an invented lift. Foliage/terrain keep the hemisphere.
    mean_sh = None
    if lmp_probes:
        cols = list(zip(*lmp_probes.values()))  # 12 columns
        mean_sh = tuple(sum(c) / len(c) for c in cols)

    # Second pass: one instance record per (instance x geom-mesh).
    instances, n_inst, n_blocks, n_probed, n_fallback = [], 0, 0, 0, 0
    backdrop_mesh_names = set()  # mesh names of horizon vista props — kept, but excluded
                                 # from the camera-fit bounds so they don't blow up the AABB
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
            # Full order-1 SH probe (12 coeffs, channel-major) — the renderer evaluates
            # amb = C0 + N.(C1,C2,C3) per channel per-vertex (the exact game shader eval).
            sh = lmp_probes.get(inst.get("hash"))
            if sh is not None:
                n_probed += 1
            elif block["kind"] == 2 and mean_sh is not None:
                # Unprobed STRUCTURE (castle/wall/townhouse) → level-average baked ambient
                # so it matches its probed neighbours rather than the near-black hemisphere.
                sh = mean_sh
                n_fallback += 1
            for name in names:
                instances.append((name, pos, yaw, scale, sh))
            n_inst += 1
    if lmp_probes:
        log(f"  lmp: {n_probed}/{n_inst} prop instances got a baked lighting probe"
            f" (+{n_fallback} unprobed structures use the level-mean ambient fallback)")

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
    # Water bodies. Repeatable: the level heightfield's .water (town canals/sea) AND the
    # vista .water (the seaward strip that partially bridges the castle-approach void). Both
    # are real shipped bodies at sea level ~36.6; extending a sea plane beyond them would be
    # guessing (the rest of the void is backdrop/skybox), so only shipped bodies are cooked.
    water_files = ([water_file] if isinstance(water_file, (str, Path)) else list(water_file or []))
    for wi, wf in enumerate(f for f in water_files if f):
        try:
            wbuilt = _build_water(Path(wf).read_bytes())
        except Exception as exc:  # noqa: BLE001
            wbuilt = None
            log(f"  water skip ({type(exc).__name__}: {exc})")
        if wbuilt:
            for bi, (w_pos, w_nrm, w_uv, w_idx, w_normal_map, w_params) in enumerate(wbuilt):
                w_mat = len(materials)
                wname = f"water{wi}_{bi}"
            # Material name MUST stay "water" — the renderer keys the animated translucent
                # water shader off material.name == "water" (native_world_renderer.cpp).
                w_opts = ["normal=" + w_normal_map] if w_normal_map else []
                w_opts.append("water_params=" + ",".join(f"{p:.9g}" for p in w_params))
                w_opts.append("water_opacity=0.42")
                materials.append(("water", w_opts, (0.14, 0.34, 0.52, 1.0)))
                meshes.append((wname, w_mat, w_pos, w_nrm, w_uv, w_idx))
                instances.append((wname, (0.0, 0.0, 0.0), 0.0, 1.0))
                n_inst += 1
                log(f"water{wi}_{bi} ({Path(wf).name}): {len(w_pos)//3} verts / {len(w_idx)//3} tris")

    # Distant sea/coast backdrop (.ehf) — the flat plane that fills the seaward void
    # between the town heightfield and Fairfax castle, kept at its authored footprint.
    # Shipped .water bodies are the only source of native water geometry; do not widen
    # or relabel this vista across the castle approach.
    if vista_ehf:
        try:
            vbuilt = _build_ehf(Path(vista_ehf).read_bytes())
        except Exception as exc:  # noqa: BLE001
            vbuilt = None
            log(f"  vista skip ({type(exc).__name__}: {exc})")
        if vbuilt:
            v_pos, v_nrm, v_uv, v_idx = vbuilt
            v_mat = len(materials)
            materials.append(("vista", [], (0.24, 0.33, 0.46, 1.0)))
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
                hero_start = (hx, hy, hz, hero_yaw)
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

            # Distant-backdrop VISTA models (RS_/TS_Vista_*, FarMountains, skydome) are the
            # horizon scenery — e.g. the childhood Tattered Spire `TS_Vista_HalfBuilt_V1`
            # placed at game (591,-797) ~1000wu past the town (chapter2slums.save
            # Layer_Spire_HalfBuilt). They ARE part of the level (the AssetBrowser draws them),
            # so we now COOK them — but we tag their meshes so the auto-fit camera bounds
            # (the `focus` directive below) EXCLUDE them; otherwise the ~1000wu spire blows up
            # the scene AABB and shrinks the town to a dot. They still draw, sitting small on
            # the horizon exactly as in-game.
            def _is_backdrop(mp: str) -> bool:
                low = mp.lower()
                return ("vista" in low or "farmountain" in low or "skydome" in low
                        or "backdrop" in low)

            # Dedup coincident vista STORY-STATE variants: the Tattered Spire ships as three
            # models (Started/HalfBuilt/FullyBuilt) at the IDENTICAL position — only the
            # story-active layer renders in-game, but propdump reports all three (it doesn't read
            # layer load-state), so drawing them all Z-fights as three overlaid meshes. Keep one
            # per position, preferring the childhood-era HalfBuilt.
            bd_by_pos = {}
            for p in found:
                if not _is_backdrop(p.get("model", "")):
                    continue
                gp = p.get("pos") or [0.0, 0.0, 0.0]
                k = (round(gp[0], 1), round(gp[1], 1), round(gp[2], 1))
                cur = bd_by_pos.get(k)
                if cur is None or ("halfbuilt" in p["model"].lower()
                                   and "halfbuilt" not in cur["model"].lower()):
                    bd_by_pos[k] = p
            _keep_bd = {id(p) for p in bd_by_pos.values()}
            found = [p for p in found
                     if not _is_backdrop(p.get("model", "")) or id(p) in _keep_bd]

            prop_mesh_names: dict[str, list] = {}   # model key -> [mesh_name per geom]
            n_prop_inst, n_prop_models, n_prop_skip, n_backdrop = 0, 0, 0, 0
            for p in found:
                model_path = p.get("model", "")
                if not model_path:
                    continue
                is_bd = _is_backdrop(model_path)
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
                # Horizon backdrop props (the Tattered Spire vista, FarMountains) are huge and,
                # shaded like town props (no probe + grazing sun), read as a black mass. The game
                # draws the vista layer nearly UNLIT. Emulate that with a bright, slightly sky-tinted
                # ambient SH probe (DC only) so the backdrop reads as pale distant scenery — and
                # tag its meshes out of the camera-fit bounds. No shader change needed (the probe
                # path already replaces the hemisphere floor when probe.w>0.5).
                bd_sh = None
                if is_bd:
                    backdrop_mesh_names.update(names)
                    n_backdrop += 1
                    bd_sh = (0.85, 0, 0, 0, 0.90, 0, 0, 0, 1.0, 0, 0, 0)  # DC sky-ish white
                for name in names:
                    instances.append((name, (rx, ry, rz), yaw, 1.0, bd_sh))
                n_prop_inst += 1
                n_inst += 1
            log(f"props: cooked {n_prop_models} distinct models -> {n_prop_inst} instances "
                f"({n_prop_skip} models skipped: unsupported mesh/no body; "
                f"{n_backdrop} backdrop-vista entities kept but excluded from camera fit)")
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
            if (o.startswith("albedo=") or o.startswith("normal=")
                    or o.startswith("material=")):
                # normal maps cook too (cook_lh_tex decodes comp-3 BC5 -> RGBA8 DDS) and, since
                # world_shading_model_re.txt §7 ladder step 3, so do spec/"material" masks — the
                # world PS/frag samples t2 for a Blinn-Phong highlight. All three go through the
                # same _cook_textures() pass; grayscale spec .tex cook to a single-channel DDS.
                albedo_tokens.append(o.split("=", 1)[1])
    tex_sources = [b for b in (textures_bnks or []) if b]
    tex_map = _cook_textures(albedo_tokens, tex_sources, tex_cook, f2tool,
                             tex_out_dir or (out_scene.parent / (out_scene.stem + ".textures")),
                             tmp, texture_headers_bnks=texture_headers_bnks,
                             log=log) if (tex_sources and tex_cook) else {}
    n_tex = sum(1 for v in tex_map.values() if v)
    if tex_sources and tex_cook:
        log(f"cooked {n_tex}/{len(tex_map)} distinct albedo textures -> DDS")

    # Emit F2SCENE (matches native_scene.cpp load_native_scene grammar).
    with out_scene.open("w", encoding="utf-8", newline="\n") as out:
        out.write(f"# Cooked from {engine_level.name} (v{info['version']}) — "
                  f"{len(meshes)} meshes / {n_inst} instances / {n_blocks} blocks\n")
        out.write("F2SCENE 1\n")
        if env_theme is not None:
            # Per-level theme resolved from the level's .genv -> environmentthemes.gdb
            # (resolve_genv_theme; closes the .genv gap in env_theme_colors_re.txt §4).
            # sun = light DIRECTION (-sun_toward, Y-up); sunlight = main_light*sun_intensity
            # (the world PS multiplies it into N.L); sky = the theme's SkyColour.
            sd = env_theme["sun_dir"]; sl = env_theme["sunlight"]; sk = env_theme["sky"]
            out.write(f"# env theme from .genv (tod={env_theme['tod']:.3f}, "
                      f"sun_intensity={env_theme['sun_intensity']:.3f})\n")
            out.write(f"sun {sd[0]:.5g} {sd[1]:.5g} {sd[2]:.5g}\n")
            out.write(f"sunlight {sl[0]:.5g} {sl[1]:.5g} {sl[2]:.5g}\n")
            out.write(f"sky {sk[0]:.5g} {sk[1]:.5g} {sk[2]:.5g} 1\n")
            # horizon (complementary) tint for the sky gradient's bottom. The theme
            # value is raw HDR (Factor can push >1); the runtime gradient PS clamps at
            # output with no tonemap, so display-map here with per-channel Reinhard
            # (retail dome uses Reinhard; SkyDomeXex.cpp:557) to avoid a blown-white
            # horizon. Absent -> renderer keeps its hardcoded blue fallback.
            hz = env_theme.get("horizon")
            if hz is not None:
                hz = [c / (1.0 + c) for c in hz]
                out.write(f"sky_horizon {hz[0]:.5g} {hz[1]:.5g} {hz[2]:.5g}\n")
            # sunset tint = theme sunset_colour: a warm dawn/dusk halo the sky PS adds toward
            # a low sun (no-op when the sun is high/below). Typically LDR; clamp for safety.
            ss = env_theme.get("sunset")
            if ss is not None:
                ss = [min(max(c, 0.0), 1.0) for c in ss]
                out.write(f"sky_sunset {ss[0]:.5g} {ss[1]:.5g} {ss[2]:.5g}\n")
            # complementary_bias reshapes the horizon->zenith ramp (0 = linear).
            cb = env_theme.get("compl_bias")
            if cb is not None:
                out.write(f"sky_bias {min(max(cb, 0.0), 1.0):.5g}\n")
            # Analytic-atmosphere params (Hoffman-Preetham single scattering; SkyboxRenderer PS).
            # Presence of this line switches the sky from the flat gradient stand-in to the retail
            # atmosphere dome. sun_intensity, rayleigh (BetaRayleighMult), mie (BetaMieMult).
            out.write(f"sky_atmos {env_theme['sun_intensity']:.5g} "
                      f"{env_theme.get('rayleigh', 1.0):.5g} {env_theme.get('mie', 0.83):.5g}\n")
            # Distance fog on world geometry (env_theme_colors_re.txt §6). Emitted only when the
            # theme carries a full fogging record; fog_range max=far_density enables it.
            fc = env_theme.get("fog_color")
            fs, fe, fm = env_theme.get("fog_start"), env_theme.get("fog_end"), env_theme.get("fog_max")
            if fc is not None and None not in (fs, fe, fm) and fe > fs and fm > 0.0:
                fc = [min(max(c, 0.0), 1.0) for c in fc]
                out.write(f"fog_color {fc[0]:.5g} {fc[1]:.5g} {fc[2]:.5g}\n")
                out.write(f"fog_range {fs:.6g} {fe:.6g} {min(max(fm, 0.0), 1.0):.5g}\n")
            # Authored AMBIENT model (Lighting sub-record: AmbientColour flat +
            # SkyColourFinalBounceTop/Bottom hemisphere bounce). Replaces the renderer's
            # hardcoded cool-blue hemisphere with the theme's real ambient. Emitted only when the
            # theme authors the full set (all three present) so scenes without it are byte-identical
            # (renderer keeps its hemisphere fallback). fable2-theme-ambient-lighting.
            af = env_theme.get("ambient_flat")
            bt = env_theme.get("sky_bounce_top"); bb = env_theme.get("sky_bounce_bot")
            if af is not None and bt is not None and bb is not None:
                out.write(f"ambient {af[0]:.5g} {af[1]:.5g} {af[2]:.5g}\n")
                out.write(f"sky_bounce {bt[0]:.5g} {bt[1]:.5g} {bt[2]:.5g} "
                          f"{bb[0]:.5g} {bb[1]:.5g} {bb[2]:.5g}\n")
            # Cloud layers (SkyboxRenderer.cpp cloud pass). Each theme Layer carries a density-map
            # texture GUID + scroll/shape/lighting params; we resolve the GUID -> exact .tex name,
            # cook it to DDS alongside the other textures, and emit one `cloud_layer` per layer plus
            # a `cloud_globals`. The cloud pass draws each layer as a flat scrolling quad at its
            # authored height, alpha-blended behind the world. Emitted only when a layer resolves a
            # density map (IsActive requires a bound density texture); absent -> no cloud pass, so
            # scenes/themes without clouds are byte-identical.
            cl_layers = env_theme.get("clouds") or []
            cl_emitted = 0
            for li, layer in enumerate(cl_layers):
                if not layer or "density_hash" not in layer:
                    continue
                tok = _resolve_env_texture_hash(layer["density_hash"], tex_sources)
                if not tok:
                    log(f"  cloud L{li + 1}: density GUID {layer['density_hash']:#010x} "
                        f"unresolved in the texture bnks; skipping layer")
                    continue
                dmap = _cook_textures([tok], tex_sources, tex_cook, f2tool,
                                      tex_out_dir or (out_scene.parent / (out_scene.stem + ".textures")),
                                      tmp, texture_headers_bnks=texture_headers_bnks, log=log)
                dds = dmap.get(tok, "")
                if not dds:
                    log(f"  cloud L{li + 1}: density '{tok}' failed to cook; skipping layer")
                    continue
                g = layer.get
                if cl_emitted == 0:
                    # cloud_global.x = global brightness (1), .z = alpha-test reference. Retail
                    # cloud context uses 5/255 (AlphaTestThreshold(config, render_context_two)).
                    out.write("cloud_globals 1 0.019608\n")
                # density path first, then: height size_x size_y scale_x scale_y vel_x vel_y
                #   transparency brightness ambient normal_strength (all in authored units;
                #   the renderer applies the *0.001 velocity scale + ShaderNormalStrength).
                out.write("cloud_layer {} {:.6g} {:.6g} {:.6g} {:.7g} {:.7g} {:.6g} {:.6g} "
                          "{:.6g} {:.6g} {:.6g} {:.6g}\n".format(
                              dds,
                              g("height", 0.0), g("size_x", 0.0), g("size_y", 0.0),
                              g("texture_scale_x", 0.001), g("texture_scale_y", 0.001),
                              g("velocity_x", 0.0), g("velocity_y", 0.0),
                              g("transparency", 0.0), g("brightness", 1.0),
                              g("ambient_light", 0.0), g("normal_strength", 0.0)))
                cl_emitted += 1
                log(f"  cloud L{li + 1}: '{tok}' h={g('height', 0.0):.1f} "
                    f"size=({g('size_x', 0.0):.0f},{g('size_y', 0.0):.0f}) "
                    f"alpha={g('transparency', 0.0):.2f}")
            if cl_emitted:
                log(f"clouds: emitted {cl_emitted} layer(s)")
            # Celestial moon billboard (SkyboxRenderer.cpp element pass). Emitted only when the theme
            # authors a moon (moon_intensity>0 -> night); daytime themes emit nothing.
            moon = env_theme.get("moon")
            if moon and moon.get("moon_tex"):
                mtok = _resolve_env_texture_hash(moon["moon_tex"], tex_sources)
                gtok = _resolve_env_texture_hash(moon["glare_tex"], tex_sources) if moon.get("glare_tex") else None
                mdds = ""
                if mtok:
                    mdds = _cook_textures([mtok], tex_sources, tex_cook, f2tool,
                                          tex_out_dir or (out_scene.parent / (out_scene.stem + ".textures")),
                                          tmp, texture_headers_bnks=texture_headers_bnks, log=log).get(mtok, "")
                gdds = ""
                if gtok:
                    gdds = _cook_textures([gtok], tex_sources, tex_cook, f2tool,
                                          tex_out_dir or (out_scene.parent / (out_scene.stem + ".textures")),
                                          tmp, texture_headers_bnks=texture_headers_bnks, log=log).get(gtok, "")
                if mdds:
                    md = moon["dir"]
                    # sky_moon <moon_dds> <glare_dds|-> <dir.xyz> <intensity> <size> <transparency>
                    #   <glare_intensity> <glare_size> <exposure> <phase>. exposure = the retail
                    #   dome HDR scale (dome_misc.z = 10). phase is runtime lunar state (not in the
                    #   theme); bake a static full moon (cell 4 of the 8-phase strip) as the
                    #   representative, most-visible night — same static-pick spirit as --tod.
                    out.write("sky_moon {} {} {:.6g} {:.6g} {:.6g} {:.6g} {:.6g} {:.6g} {:.6g} "
                              "{:.6g} {:.6g} {:d}\n".format(
                                  mdds, gdds if gdds else "-", md[0], md[1], md[2],
                                  moon["intensity"], moon["size"], moon["transparency"],
                                  moon["glare_intensity"], moon["glare_size"], 10.0, 4))
                    log(f"moon: emitted (dir {md[0]:.2f},{md[1]:.2f},{md[2]:.2f})")
            # Star brightness carried for a future procedural star field (not yet rendered).
            sb = env_theme.get("star_brightness") or 0.0
            if sb > 0.0:
                out.write(f"sky_stars {sb:.5g}\n")
        else:
            # Real chapter2slums midday theme (ghidra_out/env_theme_colors_re.txt, from
            # environmentthemes.gdb, BE bytes /255): sun = light DIRECTION = -sun_toward
            # (sun_toward Y-up = 0.9135,0.4067,0.0048); sky zenith blue (167,208,255).
            out.write("sun -0.9135 -0.4067 -0.0048\n")
            # Warm directional sun colour = theme main_light_colour raw (255,230,111)/255
            # (env_theme_colors_re.txt §0); the world PS tints the N.L term with it.
            out.write("sunlight 1.0 0.902 0.4353\n")
            out.write("sky 0.6549 0.8157 1.0 1\n")
        if hero_start is not None:
            out.write(f"hero_start {hero_start[0]:.6g} {hero_start[1]:.6g} "
                      f"{hero_start[2]:.6g} {hero_start[3]:.6g}\n")
        # Camera-fit bounds (render space) over the TOWN geometry only — every instance except
        # the horizon vista props (backdrop_mesh_names). Emitting an explicit `focus` frees the
        # renderer's auto-orbit from having to fit the ~1000wu spire (which would shrink the town
        # to a dot). World-baked meshes (terrain/water/vista, instanced at the origin) contribute
        # their vertex extents (game verts -> render via the {x,z,y} swap); placed props
        # contribute their instance position.
        mesh_verts = {m[0]: m[2] for m in meshes}
        flo = [float("inf")] * 3
        fhi = [float("-inf")] * 3
        for rec in instances:
            iname, ipos = rec[0], rec[1]
            if iname in backdrop_mesh_names:
                continue
            if ipos == (0.0, 0.0, 0.0) and iname in mesh_verts:
                vp = mesh_verts[iname]
                for vi in range(0, len(vp), 3):
                    for a, val in enumerate((vp[vi], vp[vi + 2], vp[vi + 1])):  # {x,z,y} swap
                        flo[a] = min(flo[a], val)
                        fhi[a] = max(fhi[a], val)
            else:
                for a in range(3):
                    flo[a] = min(flo[a], ipos[a])
                    fhi[a] = max(fhi[a], ipos[a])
        if all(math.isfinite(flo[a]) and math.isfinite(fhi[a]) for a in range(3)):
            fc = [0.5 * (flo[a] + fhi[a]) for a in range(3)]
            fr = max(0.5 * (fhi[a] - flo[a]) for a in range(3)) or 1.0
            out.write(f"focus {fc[0]:.6g} {fc[1]:.6g} {fc[2]:.6g} {fr:.6g}\n")
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
                    # Spec/"material" mask: repoint at the cooked DDS (t2). Drop if it didn't
                    # cook so the PS uses its black-default spec (spec_mask 0 = no highlight).
                    dds = tex_map.get(o[len("material="):], "")
                    if dds:
                        emit.append("material=" + dds.replace("\\", "/"))
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
        for rec in instances:
            name, pos, yaw, scale = rec[:4]
            sh = rec[4] if len(rec) > 4 else None
            line = (f"instance {name} {pos[0]:.9g} {pos[1]:.9g} {pos[2]:.9g} "
                    f"0 {yaw:.9g} 0 {scale:.9g}")
            if sh is not None:
                # Per-instance baked order-1 SH probe (.lmp, 12 coeffs channel-major). The
                # renderer evaluates amb=C0+N.(C1,C2,C3) per channel per-vertex against the
                # object normal (native_world_renderer), replacing the hemisphere floor.
                line += " sh " + " ".join(f"{c:.6g}" for c in sh)
            out.write(line + "\n")
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
    ap.add_argument("--texture-headers-bnk", type=Path, action="append", dest="texture_headers_bnk",
                    help="a texture-header bnk (repeatable; supplies PF/width/height for bare global "
                         "texture payloads such as PF40 water normals)")
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
    ap.add_argument("--level-lmp", type=Path,
                    help="the level's .lmp LightmapFile -> per-prop baked lighting probes "
                         "(DC ambient term lights static props with their baked GI)")
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
    ap.add_argument("--water-file", type=Path, action="append", dest="water_file",
                    help="an extracted .water file -> flat water planes (repeatable: the "
                         "heightfield .water AND the vista sea .water)")
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
    ap.add_argument("--genv", type=Path,
                    help="the level's extracted .genv (per-cell env-theme grid) -> resolve the "
                         "real per-level sky/sun theme instead of the hardcoded slums midday")
    ap.add_argument("--env-gdb", type=Path,
                    default=Path(__file__).resolve().parents[2] / "Fable2Recomp" / "assets" / "game"
                    / "data" / "environmentthemes" / "environmentthemes.gdb",
                    help="environmentthemes.gdb (the shared theme palette; needed with --genv)")
    ap.add_argument("--tod", type=float, default=12.0,
                    help="time-of-day in hours (0..24) for theme selection (default 12 = midday)")
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
        env_theme = None
        if args.genv:
            if not (args.env_gdb and args.env_gdb.is_file()):
                ap.error("--genv requires --env-gdb pointing at environmentthemes.gdb")
            env_theme = resolve_genv_theme(args.genv, args.env_gdb, tod_hours=args.tod)
        cook_level(args.engine_level, args.header_bnk, args.body_bnk, args.f2tool,
                   args.cook, types=types, max_per_block=args.max_per_block,
                   textures_bnks=args.textures_bnk, texture_headers_bnks=args.texture_headers_bnk,
                   tex_cook=tex_cook,
                   tex_out_dir=args.tex_out_dir, terrain_ghf=args.terrain_ghf,
                   terrain_ehf=args.terrain_ehf, terrain_splat=args.terrain_splat,
                   terrain_splat_res=args.terrain_splat_res, splat_bake=args.splat_bake,
                   level_lmp=args.level_lmp,
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
                   prop_limit=args.prop_limit, vista_ehf=args.vista_ehf,
                   env_theme=env_theme)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
