#!/usr/bin/env python3
"""Cook a level's NAMED QUEST ENTITIES/MARKERS into a `.f2names` sidecar.

Why a sidecar and not F2SCENE: the scene format + `native_scene.h` are owned by the ENVIRONMENT
session, and named entities are a GAMEPLAY concern (they carry no geometry - they are the points
and volumes a quest script asks for by name). So this cook writes a small companion file that the
gameplay runtime loads next to the scene, with no schema negotiation and no render-side change.

Why it is needed (measured): the childhood quest resolves its cast and markers through
`QuestThreadBase.GetEntityWithName` (103 call sites in qc010_childhood.lua), which the port backs
with SearchTools over `NativeGame::entity_names`. That map is populated only by the hero and by
`Debug.CreateEntityAt`, so every lookup returned nothing and every world-gated beat parked - see
the SPIN SET in docs/childhood_stub_census.txt.

⚠ LEVEL, NOT SCENARIO (verified this session): the childhood plays on BWSSlums'
**defaultscenario**, NOT `chapter2slums`. `chapter2slums.save` holds just 2 leftover `QC010_*`
names, while `defaultscenario.save` holds 95 - and gameflow.lua only calls
`Layers.ActivateScenario('Albion','BWSSlums','Chapter2Slums')` at the childhood's RESOLUTION
(the good/evil branch), i.e. Chapter2Slums is the POST-childhood state of the same map.

Decode path: `<level>.save` is an XML name->GUID registry and `<level>.gdb` the per-GUID record
table; `npc_markerdump --filter ""` follows the validated SimpleTransformComponent (0x619F96CF)
-> Position (0x...) / Rotation (0x21EBC83B) chain (ghidra_out/npc_spawn_re.txt §1.2). Entities
with no such component are dropped by the tool and reported here as `no_transform` - that is
expected for CREATURES (Rose, Theresa, ...), which are spawned rather than statically placed;
what this cook yields is the marker/trigger/point geometry the quest navigates by.

Coordinates: markerdump reports GAME space; this writes RENDER/world space using the cooker's
own {x,z,y} swap (cook_levels.py: game(gx,gy,gz) -> world(gx, gz, gy)), so positions are directly
comparable to the hero's.

Usage:
    python cook_quest_markers.py --level worlds/albion/bwsslums/defaultscenario/defaultscenario \\
        --game-dir Fable2Recomp/assets/game -o bwsslums_default.f2names
"""
from __future__ import annotations

import argparse
import json
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
DEFAULT_F2TOOL = REPO / "Fable2AssetBrowser" / "source" / "build" / "f2tool.exe"
DEFAULT_MARKERDUMP = REPO / "Fable2AssetBrowser" / "source" / "build" / "npc_markerdump.exe"


def extract(f2tool: Path, bnk: Path, entry: str, out: Path) -> bool:
    """Pull one entry out of a .bnk with f2tool (its BnkCore handles the level bank layout)."""
    res = subprocess.run([str(f2tool), "extract", str(bnk), entry.replace("/", "\\"), str(out)],
                         capture_output=True, text=True)
    return res.returncode == 0 and out.is_file() and out.stat().st_size > 0


def cook(level: str, game_dir: Path, out_path: Path, f2tool: Path, markerdump: Path,
         log=print) -> int:
    levels_bnk = game_dir / "data" / "levels.bnk"
    for tool, what in ((f2tool, "f2tool"), (markerdump, "npc_markerdump")):
        if not tool.is_file():
            log(f"error: {what} not found at {tool}")
            return 0
    if not levels_bnk.is_file():
        log(f"error: levels.bnk not found at {levels_bnk}")
        return 0

    tmp = Path(tempfile.mkdtemp(prefix="f2names_"))
    save_p, gdb_p = tmp / "level.save", tmp / "level.gdb"
    if not extract(f2tool, levels_bnk, level + ".save", save_p):
        log(f"error: could not extract {level}.save")
        return 0
    if not extract(f2tool, levels_bnk, level + ".gdb", gdb_p):
        log(f"error: could not extract {level}.gdb")
        return 0

    dump = tmp / "entities.json"
    res = subprocess.run([str(markerdump), str(save_p), str(gdb_p),
                          "--filter", "", "--out", str(dump)], capture_output=True, text=True)
    if res.returncode != 0 or not dump.is_file():
        log(f"error: npc_markerdump failed: {res.stderr.strip() or res.stdout.strip()}")
        return 0
    data = json.loads(dump.read_text(encoding="utf-8"))
    markers = data.get("markers", [])

    records = []
    for m in markers:
        gx, gy, gz = m["pos"]
        rot = m.get("rot") or [0.0, 0.0, 0.0]
        records.append({
            "name": m["name"],
            "pos": [gx, gz, gy],        # game -> world {x,z,y}, matching cook_levels.py
            "yaw": rot[1] if len(rot) > 1 else 0.0,
        })
    records.sort(key=lambda r: r["name"])

    # Line format, not JSON: the runtime has no JSON reader and this file is small, so a
    # tab-separated table keeps the loader trivial and the artifact diffable. Names cannot
    # contain a tab, so the split is unambiguous.
    lines = [
        "F2NAMES 1",
        "# level: %s" % level,
        "# source: npc_markerdump --filter '' over <level>.save + <level>.gdb",
        "# space: world (game {x,z,y} swap applied, matching cook_levels.py)",
        "# entities with no SimpleTransformComponent (dropped, mostly spawned creatures): %d"
        % data.get("miss", 0),
        "# name\tx\ty\tz\tyaw",
    ]
    for r in records:
        lines.append("%s\t%.4f\t%.4f\t%.4f\t%.4f"
                     % (r["name"], r["pos"][0], r["pos"][1], r["pos"][2], r["yaw"]))
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    out = {"no_transform": data.get("miss", 0)}
    quest = sum(1 for r in records if r["name"][:1] == "Q" and "_" in r["name"])
    log(f"cooked {len(records)} named entities ({quest} quest-prefixed, "
        f"{out['no_transform']} had no transform component) -> {out_path}")
    return len(records)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--level", required=True,
                    help="level path inside levels.bnk, WITHOUT extension "
                         "(e.g. worlds/albion/bwsslums/defaultscenario/defaultscenario)")
    ap.add_argument("--game-dir", type=Path, default=REPO / "Fable2Recomp" / "assets" / "game")
    ap.add_argument("-o", "--out", type=Path, required=True, help="output .f2names path")
    ap.add_argument("--f2tool", type=Path, default=DEFAULT_F2TOOL)
    ap.add_argument("--markerdump", type=Path, default=DEFAULT_MARKERDUMP)
    args = ap.parse_args(argv)
    return 0 if cook(args.level, args.game_dir, args.out, args.f2tool, args.markerdump) else 1


if __name__ == "__main__":
    raise SystemExit(main())
