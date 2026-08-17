#!/usr/bin/env python3
"""Resolve Fable II animation-slot names -> bank clip key0 from globals.gdb (data-backed).

The animation bank keys clips by an opaque "id_<key0>"; the mapping from a locomotion/action slot
NAME (Idle/Walk/Run/WalkFootstep/...) to a clip is stored in the GDB: a slot name is FNV-1 hashed
to a GDB FIELD-NAME hash on a creature's animation-set record, and that field's (type 4) raw u32
value IS the bank clip key0. Faithful reimplementation of the AssetBrowser GdbMiniView +
scan_gdb_animation_fields (Fable2AssetBrowser/source/src/animations/AnimBank.cpp), verified
byte-for-byte against globals.gdb (record 0x576283C7 = the hero-human anim set: Idle->id_4B706EF5,
Walk->id_49220AA3, Run->id_4AB9BC89). Fields inherit up the kHashParent chain (type 6).

Usage:
  gdb_anim_slots.py <globals.gdb> resolve <recordHashHex> Idle Walk Run   # slot -> id_<key0>
  gdb_anim_slots.py <globals.gdb> fields  <recordHashHex>                  # all anim (type-4) fields
"""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

KHEADER = 0x18
KHASH_PARENT = 0x5F6317D5  # AnimBank.cpp kHashParent (type-6 field pointing at the parent record)
FNV_BASIS = 0x811C9DC5
FNV_PRIME = 0x01000193


def fnv1(name: str) -> int:
    """Engine FNV-1: basis 0x811C9DC5, prime 0x01000193, MULTIPLY-then-XOR the low byte."""
    h = FNV_BASIS
    for c in name.encode("latin1", "ignore"):
        h = ((h * FNV_PRIME) & 0xFFFFFFFF) ^ c
    return h


def _be(b: bytes, o: int) -> int:
    return struct.unpack_from(">I", b, o)[0]


class GdbView:
    """Minimal read-only GDB record/field view (mirrors AnimBank.cpp GdbMiniView)."""

    def __init__(self, data: bytes):
        self.b = data
        self.ok = False
        if len(data) < KHEADER or data[:4] != b"GDB\x00":
            return
        self.count = _be(data, 0x04)
        self.size_a = _be(data, 0x08)
        self.size_b = _be(data, 0x0C)
        if self.count == 0:
            return
        self.schema_base = KHEADER + self.size_a
        self.hash_base = self.schema_base + self.size_b
        self.body_end = self.schema_base
        if not self._build_offsets():
            return
        self.ok = True

    def _schema_at(self, record: int):
        if record + 4 > self.body_end:
            return None
        schema_off = self.schema_base + _be(self.b, record)
        if schema_off + 4 > self.hash_base:
            return None
        header = _be(self.b, schema_off)
        fc = header >> 8
        if fc > 256:  # extended field count (rare)
            p = self.b
            fc = (p[schema_off] | (p[schema_off + 1] << 8)) + p[schema_off + 2]
            if fc > 1024:
                return None
        if schema_off + 4 + fc * 8 > self.hash_base:
            return None
        return schema_off, fc

    def _build_offsets(self) -> bool:
        self.offsets = []
        cur = KHEADER
        for _ in range(self.count):
            if cur + 4 > self.body_end:
                return False
            self.offsets.append(cur)
            s = self._schema_at(cur)
            if not s:
                return False
            cur += 4 + s[1] * 4
            if cur > self.body_end:
                return False
        return True

    def lookup(self, rec_hash: int):
        """Binary search the sorted hash table -> record offset, or None."""
        lo, hi = 0, self.count
        while lo < hi:
            mid = (lo + hi) // 2
            v = _be(self.b, self.hash_base + mid * 4)
            if v < rec_hash:
                lo = mid + 1
            else:
                hi = mid
        if lo >= self.count or _be(self.b, self.hash_base + lo * 4) != rec_hash:
            return None
        return self.offsets[lo]

    def _find_local(self, record: int, field_hash: int, want_type: int):
        s = self._schema_at(record)
        if not s:
            return None
        schema_off, fc = s
        hashes = schema_off + 4
        descs = hashes + fc * 4
        for i in range(fc):
            if _be(self.b, hashes + i * 4) != field_hash:
                continue
            if (_be(self.b, descs + i * 4) >> 24) != want_type:
                continue
            return _be(self.b, record + 4 + i * 4)
        return None

    def find_field(self, rec_hash: int, field_hash: int, want_type: int = 4):
        """Field value with kHashParent inheritance (like AnimBank.cpp find_field)."""
        record = self.lookup(rec_hash)
        seen = set()
        depth = 0
        while record is not None and depth < 64:
            if record in seen:
                return None
            seen.add(record)
            v = self._find_local(record, field_hash, want_type)
            if v is not None:
                return v
            parent = self._find_local(record, KHASH_PARENT, 6)
            if not parent:
                return None
            record = self.lookup(parent)
            depth += 1
        return None

    def anim_fields(self, rec_hash: int):
        """All local type-4 (clip-ref) fields on a record: [(field_hash, key0)]."""
        record = self.lookup(rec_hash)
        if record is None:
            return []
        s = self._schema_at(record)
        if not s:
            return []
        schema_off, fc = s
        hashes = schema_off + 4
        descs = hashes + fc * 4
        out = []
        for i in range(fc):
            if (_be(self.b, descs + i * 4) >> 24) == 4:
                out.append((_be(self.b, hashes + i * 4), _be(self.b, record + 4 + i * 4)))
        return out


def resolve_slots(gdb: GdbView, rec_hash: int, slots):
    """slot name -> "id_<key0>" (or None) via fnv1(slot) field lookup + parent inheritance."""
    out = {}
    for slot in slots:
        v = gdb.find_field(rec_hash, fnv1(slot), 4)
        out[slot] = ("id_%08X" % v) if v is not None else None
    return out


def main(argv=None) -> None:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("gdb", type=Path)
    sub = p.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("resolve"); r.add_argument("record"); r.add_argument("slots", nargs="+")
    f = sub.add_parser("fields"); f.add_argument("record")
    args = p.parse_args(argv)
    gdb = GdbView(args.gdb.read_bytes())
    if not gdb.ok:
        raise SystemExit(f"not a parseable GDB: {args.gdb}")
    rec = int(args.record, 16)
    if args.cmd == "resolve":
        for slot, cid in resolve_slots(gdb, rec, args.slots).items():
            print(f"  {slot:16s} -> {cid}")
    else:
        fields = gdb.anim_fields(rec)
        print(f"record 0x{rec:08X}: {len(fields)} type-4 (clip-ref) fields")
        for fh, key0 in fields[:60]:
            print(f"  field 0x{fh:08X} -> id_{key0:08X}")


if __name__ == "__main__":
    main()
