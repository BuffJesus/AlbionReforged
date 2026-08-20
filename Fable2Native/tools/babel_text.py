#!/usr/bin/env python3
"""Decode Fable II's localised text (`book.babel`) — tag -> the real in-game string.

This was the project's long-standing blocker ("the book.babel bulk-text compression codec is
UNKNOWN, so strings must be COOKED/overridden"). The codec is **plain zlib**; the rest is a
straightforward index + block layout, all big-endian:

    0x00  u32  version/magic (0x5B010000 in the shipped en-uk file)
    0x04  u32  entry_count                       (56000 in en-uk)
    0x08  entry_count * { u32 fnv1(TextTag), u32 blockKey, u32 byteOffsetInBlock }
                                                  sorted ascending by hash
    then  u32  block_count                       (333)
          block_count * { u32 blockKey, u32 compressedSize, u32 uncompressedSize }
                        followed immediately by `compressedSize` bytes of a **zlib** stream
                        (0x78 0xDA), decompressing to ~16 KB of packed strings

    a string at `byteOffsetInBlock` is:  u32 length_in_UTF16_code_units (INCLUDING the trailing
                                         NUL), then that many UTF-16 BIG-ENDIAN code units

So: text(tag) = string_at(unzip(block[index[fnv1(tag)].blockKey]), index[...].offset).

Verified against the game: TEXT_LEVEL_FAIRFAX_CASTLE -> 'Castle Fairfax'.

This unlocks the game's OWN words for anything the port displays — quest names, dialogue,
subtitles, menu labels — instead of internal identifiers or invented English.

Usage:
    python babel_text.py <book.babel> --tag TEXT_LEVEL_FAIRFAX_CASTLE [--tag ...]
    python babel_text.py <book.babel> --grep FAIRFAX          # search DECODED text
    python babel_text.py <book.babel> --stats
    python babel_text.py <book.babel> --cook out.f2text       # cook a runtime lookup package
"""
from __future__ import annotations

import argparse
import struct
import sys
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gdb_anim_slots import fnv1  # noqa: E402  (the engine's FNV-1, verified elsewhere)


class BabelText:
    def __init__(self, data: bytes):
        self.b = data
        self.ok = False
        if len(data) < 8:
            return
        self.count = self._be(0x04)
        self.index = {}
        base = 8
        if base + self.count * 12 > len(data):
            return
        for i in range(self.count):
            at = base + i * 12
            self.index[self._be(at)] = (self._be(at + 4), self._be(at + 8))
        off = base + self.count * 12
        self.block_count = self._be(off)
        off += 4
        self.blocks = {}
        for _ in range(self.block_count):
            if off + 12 > len(data):
                return
            key, csz, usz = self._be(off), self._be(off + 4), self._be(off + 8)
            self.blocks[key] = (off + 12, csz, usz)
            off += 12 + csz
        self._cache = {}
        self.ok = True

    def _be(self, o: int) -> int:
        return struct.unpack_from(">I", self.b, o)[0]

    def _block(self, key: int) -> bytes:
        if key not in self._cache:
            at, csz, _usz = self.blocks[key]
            self._cache[key] = zlib.decompressobj().decompress(self.b[at:at + csz])
        return self._cache[key]

    @staticmethod
    def _string_at(blob: bytes, off: int) -> str:
        if off + 4 > len(blob):
            return ""
        n = struct.unpack_from(">I", blob, off)[0]
        if n == 0:
            return ""
        end = off + 4 + (n - 1) * 2          # n counts the trailing NUL
        return blob[off + 4:end].decode("utf-16-be", "replace")

    def text(self, tag: str):
        """The in-game string for a TextTag, or None if the tag is not in this language file."""
        ent = self.index.get(fnv1(tag))
        if not ent:
            return None
        block_key, off = ent
        if block_key not in self.blocks:
            return None
        return self._string_at(self._block(block_key), off)

    def by_hash(self, tag_hash: int):
        ent = self.index.get(tag_hash)
        if not ent:
            return None
        return self._string_at(self._block(ent[0]), ent[1]) if ent[0] in self.blocks else None

    def all_items(self):
        """[(tagHash, text)] for every entry (decompresses every block)."""
        for h in self.index:
            t = self.by_hash(h)
            if t is not None:
                yield h, t


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("babel", type=Path)
    ap.add_argument("--tag", action="append", default=[], help="look one TextTag up")
    ap.add_argument("--grep", help="search the DECODED text (case-insensitive)")
    ap.add_argument("--stats", action="store_true")
    ap.add_argument("--cook", type=Path, help="write a runtime lookup package (.f2text)")
    args = ap.parse_args(argv)

    bt = BabelText(args.babel.read_bytes())
    if not bt.ok:
        print("error: could not parse %s" % args.babel)
        return 1
    if args.stats or not (args.tag or args.grep or args.cook):
        print("# %s: %d entries, %d zlib blocks" % (args.babel.name, bt.count, bt.block_count))
    for t in args.tag:
        print("%-40s %r" % (t, bt.text(t)))
    if args.grep:
        needle = args.grep.lower()
        shown = 0
        for h, txt in bt.all_items():
            if needle in txt.lower():
                print("0x%08X  %r" % (h, txt))
                shown += 1
                if shown >= 40:
                    print("... (more)")
                    break
    if args.cook:
        # Runtime package: sorted { u32 tagHash, u32 utf8Offset } + a UTF-8 blob. UTF-8 keeps it
        # compact and the runtime hashes the tag with the same FNV-1, so lookup is one bsearch.
        items = sorted(bt.all_items())
        blob = bytearray()
        rows = []
        for h, txt in items:
            rows.append((h, len(blob)))
            blob += txt.encode("utf-8") + b"\0"
        out = bytearray(b"F2TEXT\0\0")
        out += struct.pack(">II", len(rows), len(blob))
        for h, o in rows:
            out += struct.pack(">II", h, o)
        out += blob
        args.cook.parent.mkdir(parents=True, exist_ok=True)
        args.cook.write_bytes(bytes(out))
        print("cooked %d strings (%d KB of text) -> %s"
              % (len(rows), len(blob) // 1024, args.cook))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
