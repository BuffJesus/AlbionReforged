#!/usr/bin/env python3
"""Dump a NAMED level entity's GDB record: its component fields, resolved to component names.

Why: `<level>.save` is a name -> GUID registry and `<level>.gdb` holds the per-GUID record. An
entity's record carries one field per component (the field-name hash is FNV-1 of the component
class name minus its "CEC" prefix — the same convention entity_build_components_from_record walks,
ghidra_out/gdb_instantiation_re.txt). Static props/markers expose a SimpleTransformComponent, which
is what tools/cook_quest_markers.py follows; entities WITHOUT one are the interesting case (spawned
creatures), and this tool answers what they DO carry instead of guessing.

Component names come from ghidra_out/gdb_component_registry.txt (the 261 CEC* classes recovered
from the registrar hub); each is hashed both with and without the "CEC" prefix so either naming
convention resolves.

Usage:
    python gdb_entity_dump.py <level.save> <level.gdb> <EntityName> [<EntityName> ...]
    python gdb_entity_dump.py <level.save> <level.gdb> --grep QC010_Villager
"""
from __future__ import annotations

import argparse
import re
import struct
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
from gdb_anim_slots import GdbView, fnv1, _be  # noqa: E402  (the verified GDB reader)

REGISTRY = REPO / "ghidra_out" / "gdb_component_registry.txt"

# Field-name hashes that are not component classes but show up on entity records.
WELL_KNOWN = ["Position", "Rotation", "Scale", "Name", "kHashParent", "Definition", "Archetype",
              "CreatureType", "EntityType", "Health", "Level", "Faction", "Layer", "LoadState"]


def component_names():
    names = []
    if REGISTRY.exists():
        for line in REGISTRY.read_text(encoding="latin-1").splitlines():
            m = re.search(r"name=(\S+)", line)
            if m:
                names.append(m.group(1))
    return names


def hash_table():
    """FNV-1 hash -> label, for every component class (with and without the CEC prefix)."""
    table = {}
    for n in component_names():
        table.setdefault(fnv1(n), n)
        if n.startswith("CEC"):
            table.setdefault(fnv1(n[3:]), n[3:] + "  (= " + n + ")")
        table.setdefault(fnv1(n + "Component"), n + "Component")
        if n.startswith("CEC"):
            table.setdefault(fnv1(n[3:] + "Component"), n[3:] + "Component")
    for n in WELL_KNOWN:
        table.setdefault(fnv1(n), n)
    return table


def read_save_names(path: Path):
    """<Entity name="X">0xHASH</Entity> -> {name: guid}. Minimal scanner, no XML dep."""
    xml = path.read_text(encoding="latin-1", errors="replace")
    out = {}
    for m in re.finditer(r'<Entity\s+name="([^"]*)"[^>]*>\s*(0x[0-9A-Fa-f]+|\d+)', xml):
        val = m.group(2)
        out[m.group(1)] = int(val, 16) if val.lower().startswith("0x") else int(val)
    return out


def dump_record(gdb: GdbView, rec: int, labels):
    s = gdb._schema_at(rec)
    if not s:
        print("    (no schema)")
        return
    schema_off, fc = s
    hashes = schema_off + 4
    descs = hashes + fc * 4
    print("    %d fields" % fc)
    for i in range(fc):
        fh = _be(gdb.b, hashes + i * 4)
        desc = _be(gdb.b, descs + i * 4)
        ftype = desc >> 24
        val = _be(gdb.b, rec + 4 + i * 4)
        label = labels.get(fh, "")
        print("      hash=0x%08X type=%-3d value=0x%08X  %s" % (fh, ftype, val, label))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("save", type=Path)
    ap.add_argument("gdb", type=Path)
    ap.add_argument("names", nargs="*")
    ap.add_argument("--grep", help="dump every entity whose name contains this substring")
    args = ap.parse_args(argv)

    reg = read_save_names(args.save)
    view = GdbView(args.gdb.read_bytes())
    if not view.ok:
        print("error: could not open gdb")
        return 1
    labels = hash_table()
    print("# %d named entities in the save, %d records in the gdb, %d known field labels"
          % (len(reg), view.count, len(labels)))

    wanted = list(args.names)
    if args.grep:
        wanted += [n for n in sorted(reg) if args.grep.lower() in n.lower()]
    for name in wanted:
        guid = reg.get(name)
        if guid is None:
            print("\n%s: NOT in the save registry" % name)
            continue
        rec = view.lookup(guid)
        print("\n%s  guid=0x%08X  %s" % (name, guid, "record found" if rec else "NO GDB RECORD"))
        if rec:
            dump_record(view, rec, labels)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
