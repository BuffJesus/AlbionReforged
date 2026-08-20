#!/usr/bin/env python3
"""Print any GDB record BY NAME, with readable field names and nested sub-records.

This makes the game's authored data legible — which is the foundation of every modding workflow
here: you cannot edit what you cannot read. A `.gdb` is otherwise a wall of 32-bit hashes; the
file's own tables turn it back into text:

  * NAME TABLE   fnv1(name) -> record GUID       (what GDB.GetRecord(name) uses)
  * STRING TABLE fnv1(s) -> s                    (type-4 field VALUES *and* the FIELD NAMES)

So a record like the childhood's first cutscene reads as:

    QC010_SetRoseMode
      BackgroundCutscene    type=0 = 1
      UseCutsceneCamera     type=0 = 0
      SceneElements         type=6
          SetEntityMode     type=6
              AnimationGroup  type=4 = 'RoseWarmingUp'
              Character       type=4 = 'QC010_Rose'

Field types (ghidra_out/gdb_entity_spec.txt): 0=bool 1=s32 2=u32 3=float 4=string 6=record-ref.
Fields inherit up the `parent` chain (the field literally named "parent", hash 0x5F6317D5), so
`--inherit` also shows what a record picks up from its archetype.

Usage:
    python gdb_record_dump.py <file.gdb> <RecordName> [<RecordName> ...] [--depth N] [--inherit]
    python gdb_record_dump.py <file.gdb> --grep QC010_        # every name containing a substring
    python gdb_record_dump.py <file.gdb> --list               # how many names/strings it holds
"""
from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gdb_anim_slots import GdbView, fnv1, _be  # noqa: E402

TYPE_NAMES = {0: "bool", 1: "s32", 2: "u32", 3: "float", 4: "string", 6: "record"}


def field_rows(v: GdbView, record: int):
    """[(name_or_hash, type, raw_value)] for one record's own fields."""
    s = v._schema_at(record)
    if not s:
        return []
    schema_off, fc = s
    hashes, descs = schema_off + 4, schema_off + 4 + fc * 4
    out = []
    for i in range(fc):
        fh = _be(v.b, hashes + i * 4)
        ftype = _be(v.b, descs + i * 4) >> 24
        val = _be(v.b, record + 4 + i * 4)
        out.append((v.text(fh, "0x%08X" % fh), ftype, val))
    return out


def render(v: GdbView, guid: int, depth: int, max_depth: int, inherit: bool, seen: set, out):
    record = v.lookup(guid)
    if record is None:
        out.append("%s(guid 0x%08X: no record in this file)" % ("    " * depth, guid))
        return
    if guid in seen:
        out.append("%s(cycle back to 0x%08X)" % ("    " * depth, guid))
        return
    seen = seen | {guid}
    pad = "    " * depth
    for name, ftype, val in field_rows(v, record):
        tn = TYPE_NAMES.get(ftype, "t%d" % ftype)
        if ftype == 3:
            shown = " = %.4f" % struct.unpack(">f", struct.pack(">I", val))[0]
        elif ftype == 4:
            shown = " = %r" % v.text(val, "0x%08X" % val)
        elif ftype in (0, 1, 2):
            shown = " = %d" % (struct.unpack(">i", struct.pack(">I", val))[0]
                               if ftype == 1 else val)
        else:
            shown = ""
        is_parent = name == "parent"
        out.append("%s%-26s %-7s%s" % (pad, name, tn, shown))
        if ftype == 6 and depth < max_depth and (inherit or not is_parent):
            render(v, val, depth + 1, max_depth, inherit, seen, out)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("gdb", type=Path)
    ap.add_argument("names", nargs="*")
    ap.add_argument("--grep", help="dump every record name containing this substring")
    ap.add_argument("--list", action="store_true", help="just report what the file holds")
    ap.add_argument("--depth", type=int, default=3, help="max sub-record depth (default 3)")
    ap.add_argument("--inherit", action="store_true",
                    help="also descend the `parent` chain (what the record inherits)")
    args = ap.parse_args(argv)

    v = GdbView(args.gdb.read_bytes())
    if not v.ok:
        print("error: not a readable .gdb: %s" % args.gdb)
        return 1
    print("# %s: %d records, %d names, %d interned strings"
          % (args.gdb.name, v.count, len(v.name_pairs), len(v.strings)))
    if args.list:
        return 0

    wanted = list(args.names)
    if args.grep:
        # Names are stored only as hashes, so recover them from the interned string pool: a string
        # whose hash is in the name table IS a record name.
        keys = set(k for k, _ in v.name_pairs)
        wanted += sorted(s for h, s in v.strings.items() if h in keys and args.grep.lower() in s.lower())
    if not wanted:
        print("nothing to dump (give a name, --grep, or --list)")
        return 1

    for name in wanted:
        guid = v.guid_for_name(name)
        if guid is None:
            print("\n%s: NOT in this file's name table" % name)
            continue
        print("\n%s  (guid 0x%08X)" % (name, guid))
        out = []
        render(v, guid, 1, args.depth, args.inherit, set(), out)
        print("\n".join(out))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
