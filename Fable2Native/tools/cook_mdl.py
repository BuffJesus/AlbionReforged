#!/usr/bin/env python3
"""Cook one exported/glued Fable II MDL into the native scene prototype.

This is an offline bridge. The native runtime never imports this parser or any
Xbox-era type; it receives the generated .f2scene file instead.
"""

from __future__ import annotations

import argparse
import math
import sys
from pathlib import Path


def add_normals(positions: list[float], indices: list[int]) -> list[float]:
    normals = [0.0] * len(positions)
    for i in range(0, len(indices), 3):
        a, b, c = (indices[i], indices[i + 1], indices[i + 2])
        ax, ay, az = positions[a * 3:a * 3 + 3]
        bx, by, bz = positions[b * 3:b * 3 + 3]
        cx, cy, cz = positions[c * 3:c * 3 + 3]
        ux, uy, uz = bx - ax, by - ay, bz - az
        vx, vy, vz = cx - ax, cy - ay, cz - az
        nx, ny, nz = uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx
        for vertex in (a, b, c):
            normals[vertex * 3] += nx
            normals[vertex * 3 + 1] += ny
            normals[vertex * 3 + 2] += nz
    for i in range(0, len(normals), 3):
        length = math.sqrt(sum(value * value for value in normals[i:i + 3]))
        if length < 1e-8:
            normals[i:i + 3] = [0.0, 1.0, 0.0]
        else:
            normals[i] /= length
            normals[i + 1] /= length
            normals[i + 2] /= length
    return normals


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("mdl", type=Path, help="exported/glued Fable II .mdl")
    parser.add_argument("output", type=Path, help="native .f2scene output")
    args = parser.parse_args()

    addon_dir = Path(__file__).resolve().parents[2] / "Fable2AssetBrowser" / "source" / "addons"
    sys.path.insert(0, str(addon_dir))
    try:
        import fable_mdl_format as mdl
    except ImportError as exc:
        parser.error(f"cannot import the authoritative MDL reader: {exc}")

    data = args.mdl.read_bytes()
    _, geoms = mdl.parse(data, log=lambda message: print(f"[mdl] {message}"))
    if not geoms:
        parser.error("MDL contained no drawable geometry")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", encoding="utf-8", newline="\n") as out:
        out.write("# Native package prototype generated from one MDL\n")
        out.write("F2SCENE 1\n")
        for material_index, geom in enumerate(geoms):
            out.write(f"material mat_{material_index} 0.72 0.72 0.72 1\n")
        for mesh_index, geom in enumerate(geoms):
            positions = geom.positions
            normals = geom.normals or add_normals(positions, geom.indices)
            out.write(f"mesh mesh_{mesh_index} {len(positions) // 3} {len(geom.indices)} {mesh_index}\n")
            for vertex in range(len(positions) // 3):
                px, py, pz = positions[vertex * 3:vertex * 3 + 3]
                nx, ny, nz = normals[vertex * 3:vertex * 3 + 3]
                if geom.uvs:
                    u, v = geom.uvs[vertex * 2:vertex * 2 + 2]
                else:
                    u, v = 0.0, 0.0
                out.write(f"vertex {px:.9g} {py:.9g} {pz:.9g} "
                          f"{nx:.9g} {ny:.9g} {nz:.9g} {u:.9g} {v:.9g}\n")
            for index in geom.indices:
                out.write(f"index {index}\n")
            out.write(f"instance mesh_{mesh_index} 0 0 0 0 0 0 1\n")
    print(f"cooked {len(geoms)} meshes -> {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
