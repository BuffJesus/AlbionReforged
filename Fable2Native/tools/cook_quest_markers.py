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
table; `npc_markerdump --filter ""` follows the validated 0x619F96CF -> Position / Rotation
(0x21EBC83B) chain (ghidra_out/npc_spawn_re.txt 1.2). Entities with no such component are dropped
by that pass; the second pass below picks them up as DECLARED entities.

Naming note (verified 2026-08-19): field hash 0x619F96CF is FNV-1("PhysicsSimpleComponent")
exactly - `CECPhysicsSimple` in ghidra_out/gdb_component_registry.txt, typeId 2. The project has
been calling it "SimpleTransformComponent"; the CHAIN is validated either way, only the name was
wrong.

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

    # name -> GUID for the marker pass too, so every row can carry its record id.
    import re as _re0
    registry_guid = {mm.group(1): int(mm.group(2), 16) for mm in
                     _re0.finditer(r'<Entity\s+name="([^"]*)"[^>]*>\s*(0x[0-9A-Fa-f]+)',
                                   save_p.read_text(encoding="latin-1", errors="replace"))}

    records = []
    for m in markers:
        gx, gy, gz = m["pos"]
        rot = m.get("rot") or [0.0, 0.0, 0.0]
        records.append({
            "name": m["name"],
            "pos": [gx, gz, gy],        # game -> world {x,z,y}, matching cook_levels.py
            "yaw": rot[1] if len(rot) > 1 else 0.0,
            "kind": "marker",
            "guid": registry_guid.get(m["name"], 0),
        })

    # DECLARED ENTITIES (no placement component). The level's .save is a name -> GUID registry;
    # an entry with no transform-bearing component is not a marker but a real entity the level
    # declares and the scripts command by name — the childhood's cast, for instance:
    #   QC010_VillagerA -> GraphicAppearanceMorphComponent + PhysicsSimulationCharacterNavigator
    #   QC010_Rose      -> PhysicsSimulationCharacterNavigator + AIBrainComponent
    #   QC010_Theresa   -> PhysicsSimulationCharacterNavigator
    # (dumped with tools/gdb_entity_dump.py). Their records live in **globals.gdb**, not the level
    # gdb, and they carry NO position — they are placed by script (teleported to markers), which is
    # why the marker pass drops them.
    # Emitting them lets GetEntityWithName return a real handle, which is what the quest needs to
    # command them at all. ⚠ FLAGGED: position is NOT data-backed (there is none to read); they are
    # seeded at the origin and it is the script's job to move them. Mesh/AI/appearance still need
    # the GDB archetype instantiation chain (ghidra_out/gdb_instantiation_re.txt) — an entity here
    # is a HANDLE, not a visible character.
    placed = {r["name"] for r in records}
    declared = 0
    skipped = 0
    try:
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        from gdb_anim_slots import GdbView, fnv1  # the verified GDB reader
        import re as _re
        xml = save_p.read_text(encoding="latin-1", errors="replace")
        registry = {m.group(1): int(m.group(2), 16) for m in
                    _re.finditer(r'<Entity\s+name="([^"]*)"[^>]*>\s*(0x[0-9A-Fa-f]+)', xml)}
        views = [GdbView(gdb_p.read_bytes())]
        globals_gdb = game_dir / "data" / "Globals" / "globals.gdb"
        if globals_gdb.is_file():
            views.append(GdbView(globals_gdb.read_bytes()))
        # A CREATURE carries its authored placement on the character-navigator component, NOT on
        # the PhysicsSimple component the marker pass follows — which is why creatures looked
        # position-less. Verified on QC010_Rose:
        #   PhysicsSimulationCharacterNavigatorComponent -> Position { X=200.013, Y=91.422,
        #                                                              Z=52.731 }
        # which lands right beside the QC010_ChildhoodStart marker, as it should. Read it here so
        # the cast is placed where the level authors put it.
        import struct as _struct
        NAV = fnv1("PhysicsSimulationCharacterNavigatorComponent")
        POS, ROT = fnv1("Position"), fnv1("Rotation")
        AXIS = {a: fnv1(a) for a in ("X", "Y", "Z")}

        def _vec(view, rec_guid, field_hash):
            """{X,Y,Z} floats off a type-6 sub-record, or None."""
            sub = view.find_field(rec_guid, field_hash, 6)
            if sub is None:
                return None
            out = {}
            for axis, ah in AXIS.items():
                raw = view.find_field(sub, ah, 3)
                if raw is None:
                    return None
                out[axis] = _struct.unpack(">f", _struct.pack(">I", raw))[0]
            return out

        placed_creatures = 0
        for name, guid in sorted(registry.items()):
            if name in placed:
                continue
            owner = next((v for v in views if v.ok and v.lookup(guid) is not None), None)
            if owner is None:
                skipped += 1
                continue
            pos, yaw = [0.0, 0.0, 0.0], 0.0
            nav = owner.find_field(guid, NAV, 6)
            if nav is not None:
                p3 = _vec(owner, nav, POS)
                if p3 and (p3["X"] or p3["Y"] or p3["Z"]):
                    # game(x,y,z) -> world {x,z,y}, the same swap the marker pass applies
                    pos = [p3["X"], p3["Z"], p3["Y"]]
                    placed_creatures += 1
                r3 = _vec(owner, nav, ROT)
                if r3:
                    yaw = r3["Y"]
            records.append({"name": name, "pos": pos, "yaw": yaw, "kind": "entity",
                            "guid": guid})
            declared += 1
        log(f"  declared entities: {declared} ({placed_creatures} had an authored "
            f"navigator position)")
    except Exception as exc:  # noqa: BLE001 — the marker pass is still valid without this
        log("  declared-entity pass skipped (%s: %s)" % (type(exc).__name__, exc))

    records.sort(key=lambda r: r["name"])

    # Line format, not JSON: the runtime has no JSON reader and this file is small, so a
    # tab-separated table keeps the loader trivial and the artifact diffable. Names cannot
    # contain a tab, so the split is unambiguous.
    lines = [
        "F2NAMES 1",
        "# level: %s" % level,
        "# source: npc_markerdump --filter '' over <level>.save + <level>.gdb",
        "# space: world (game {x,z,y} swap applied, matching cook_levels.py)",
        "# kind=marker: placed, position read from its transform-bearing component.",
        "# kind=entity: DECLARED in the .save with a GDB record but NO position - script-placed",
        "#              (the quest teleports it). Seeded at the origin; position NOT data-backed.",
        "# registry entries with neither a transform nor a GDB record: %d" % skipped,
        "# name\tx\ty\tz\tyaw\tkind\tgdbGuid",
        "# gdbGuid is the entity's record id, so the runtime can reach its AUTHORED data at",
        "# runtime (e.g. AnimationManagerComponent.Animations for a cutscene PlayAnimation beat).",
    ]
    for r in records:
        lines.append("%s\t%.4f\t%.4f\t%.4f\t%.4f\t%s\t0x%08X"
                     % (r["name"], r["pos"][0], r["pos"][1], r["pos"][2], r["yaw"], r["kind"],
                        r.get("guid", 0)))
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    out = {"no_transform": data.get("miss", 0)}
    quest = sum(1 for r in records if r["name"][:1] == "Q" and "_" in r["name"])
    log(f"cooked {len(records)} named entities: {len(records) - declared} placed markers + "
        f"{declared} declared entities ({quest} quest-prefixed); {skipped} registry entries had "
        f"neither a transform nor a GDB record -> {out_path}")
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
