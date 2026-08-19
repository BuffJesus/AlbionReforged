#!/usr/bin/env python3
"""Find which shipped script defines a function at a given `linedefined`.

A stripped LuaQ proto keeps linedefined/lastlinedefined even without a line table, so a stack
traceback's `<?:N>` is an exact fingerprint. This walks every script's proto tree and reports
every (script, proto path) whose linedefined matches, plus the string constants it references —
which usually identifies the function outright.
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(r"D:\Documents\Fable2RE\Fable2Native\tools")))
import importlib.util
spec = importlib.util.spec_from_file_location(
    "seg", r"D:\Documents\Fable2RE\Fable2Native\tools\script_engine_globals.py")
seg = importlib.util.module_from_spec(spec)
spec.loader.exec_module(seg)

import struct


class R2(seg._R):
    pass


def proto_with_lines(r):
    """Same layout as seg._proto but keeps linedefined and the proto tree."""
    r.string()
    linedefined = r.integer()
    lastlinedefined = r.integer()
    r.u8(); r.u8(); r.u8(); r.u8()
    ncode = r.integer()
    code = [struct.unpack(r.e + "I", r.raw(4))[0] for _ in range(ncode)]
    consts = []
    for _ in range(r.integer()):
        t = r.u8()
        if t == 0:
            consts.append(None)
        elif t == 1:
            consts.append(bool(r.u8()))
        elif t == 3:
            r.raw(r.num_sz); consts.append(0.0)
        elif t == 4:
            consts.append(r.string())
        else:
            raise ValueError("bad const %d" % t)
    protos = [proto_with_lines(r) for _ in range(r.integer())]
    for _ in range(r.integer()):
        r.integer()
    for _ in range(r.integer()):
        r.string(); r.integer(); r.integer()
    for _ in range(r.integer()):
        r.string()
    return {"line": linedefined, "last": lastlinedefined, "code": code,
            "consts": consts, "protos": protos}


def parse(data):
    if data[:4] != b"\x1bLua" or data[4] != 0x51:
        return None
    endian = "<" if data[6] == 1 else ">"
    r = seg._R(data, endian, data[7], data[8], data[10])
    r.p = 12
    return proto_with_lines(r)


def walk(p, path, want, out):
    if p["line"] in want:
        strs = [c for c in p["consts"] if isinstance(c, str)][:14]
        out.append((path, p["line"], p["last"], strs))
    for i, sub in enumerate(p["protos"]):
        walk(sub, "%s.proto[%d]" % (path, i), want, out)


def main():
    args = [a for a in sys.argv[1:]]
    if not args:
        print("usage: find_proto_by_line.py <scripts-dir> <linedefined> [<linedefined> ...]\n"
              "  <scripts-dir> = a cooked script package (tools/cook_scripts.py output, the\n"
              "  <out>/scripts folder). Prints every script+proto defining a function at that\n"
              "  line, with its string constants.")
        return 1
    root = Path(args[0])
    want = {int(a) for a in args[1:]}
    if not root.is_dir() or not want:
        print("error: need an existing scripts dir and at least one line number")
        return 1
    for f in sorted(root.rglob("*.lua")):
        try:
            p = parse(f.read_bytes())
        except Exception:
            continue
        if p is None:
            continue
        out = []
        walk(p, "main", want, out)
        for path, line, last, strs in out:
            rel = str(f.relative_to(root)).replace("\\", "/")
            print("%-46s %-22s line %d-%d  %s" % (rel, path, line, last, ", ".join(strs)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
