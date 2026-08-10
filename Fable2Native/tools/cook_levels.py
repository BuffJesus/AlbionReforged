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


def cook_level(engine_level: Path, header_bnk: Path, body_bnk: Path, f2tool: Path,
               out_scene: Path, types=(2, 21), max_per_block=None, log=print,
               textures_bnks=None, tex_cook: Path = None,
               tex_out_dir: Path = None) -> dict:
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
            materials.append((f"mat_{mid}_{gi}", opts))
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

    # Cook the referenced albedo textures (globals_textures.bnk .tex -> loose DDS). The runtime
    # only samples albedo (t0), so albedo is what turns the flat-grey buildings textured.
    albedo_tokens = []
    for _name, opts in materials:
        for o in opts:
            if o.startswith("albedo="):
                albedo_tokens.append(o[len("albedo="):])
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
        out.write("sun -0.4 -0.82 -0.4\n")
        out.write("sky 0.52 0.62 0.78 1\n")
        for name, opts in materials:
            # Repoint albedo at the cooked loose DDS (absolute path; the runtime loads it
            # directly). Drop albedo tokens that didn't cook so the material shows its flat
            # base colour instead of the unresolved .tex name (which would sample white).
            emit = []
            for o in opts:
                if o.startswith("albedo="):
                    dds = tex_map.get(o[len("albedo="):], "")
                    if dds:
                        emit.append("albedo=" + dds.replace("\\", "/"))
                    # else: skip albedo -> flat base colour fallback
                elif o.startswith("normal=") or o.startswith("material="):
                    continue  # runtime samples albedo only; drop uncooked normal/spec tokens
                else:
                    emit.append(o)
            out.write(f"material {name} 0.72 0.72 0.72 1" + ("".join(" " + o for o in emit)) + "\n")
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

    log(f"cooked {len(meshes)} meshes, {n_inst} instances ({n_blocks} blocks) -> {out_scene}")
    return {"meshes": len(meshes), "instances": n_inst, "blocks": n_blocks}


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
                   tex_out_dir=args.tex_out_dir)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
