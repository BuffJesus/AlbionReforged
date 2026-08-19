#!/usr/bin/env python3
"""Derive the ENGINE-PROVIDED script globals from the game's own Lua bytecode.

Why this exists (measured, not guessed): the native port's auto-stub only fabricates a stub for a
Capitalized global it believes is a retail native (`__is_native`, fed by the Ghidra-derived catalog
`ghidra_out/lua_natives5_catalog.tsv`). That catalog resolved the CLASS name for most natives but
left many rows' class as "?" - so classes like `AmbientPopulationManager` are absent from it. A
script that indexes such a global hits `nil` and its coroutine dies SILENTLY (the script managers
discard coroutine.resume's error), which is exactly how QC010_Childhood was measured to die two
frames in (docs/childhood_stub_census.txt).

The fix is derived from data the game ships, not from a guess: a global that EVERY script only ever
READS - never `SETGLOBAL`s - yet indexes as a table (`GETGLOBAL X` then `GETTABLE`/`SELF` with a
string key) cannot come from the scripts. Since retail runs, the engine must provide it. So the set
of such globals IS the set of engine-provided class tables.

Corroboration (printed per class): each method name found on the class is looked up in the Ghidra
native catalog. A hit is independent, EXE-side evidence that the method is a bound native.

Usage:
    python script_engine_globals.py <cooked-scripts-dir-or-bnk> [--catalog <catalog.tsv>]
        [--known <native_script.cpp>] [-o report.txt]

<cooked-scripts-dir> is what tools/cook_scripts.py writes (<out>/scripts). A .bnk path is read
directly via the repo's sibling script_index/bnk_repack helpers.
"""
from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

# ---- minimal Lua 5.1 (LuaQ) bytecode reader: header + proto tree, code + string constants ----

OPNAMES = [
    "MOVE", "LOADK", "LOADBOOL", "LOADNIL", "GETUPVAL", "GETGLOBAL", "GETTABLE", "SETGLOBAL",
    "SETUPVAL", "SETTABLE", "NEWTABLE", "SELF", "ADD", "SUB", "MUL", "DIV", "MOD", "POW", "UNM",
    "NOT", "LEN", "CONCAT", "JMP", "EQ", "LT", "LE", "TEST", "TESTSET", "CALL", "TAILCALL",
    "RETURN", "FORLOOP", "FORPREP", "TFORLOOP", "SETLIST", "CLOSE", "CLOSURE", "VARARG",
]


class _R:
    def __init__(self, data, endian, int_sz, size_t_sz, num_sz):
        self.d = data
        self.p = 0
        self.e = endian
        self.int_sz = int_sz
        self.size_t_sz = size_t_sz
        self.num_sz = num_sz

    def raw(self, n):
        v = self.d[self.p:self.p + n]
        self.p += n
        return v

    def u8(self):
        return self.raw(1)[0]

    def integer(self):
        fmt = {4: "i", 8: "q"}[self.int_sz]
        return struct.unpack(self.e + fmt, self.raw(self.int_sz))[0]

    def size_t(self):
        fmt = {4: "I", 8: "Q"}[self.size_t_sz]
        return struct.unpack(self.e + fmt, self.raw(self.size_t_sz))[0]

    def string(self):
        n = self.size_t()
        if n == 0:
            return None
        return self.raw(n)[:-1].decode("latin-1")


def _proto(r):
    r.string()                        # source
    r.integer(); r.integer()          # linedefined, lastlinedefined
    r.u8(); r.u8(); r.u8(); r.u8()    # nups, numparams, is_vararg, maxstacksize
    code = [struct.unpack(r.e + "I", r.raw(4))[0] for _ in range(r.integer())]
    consts = []
    for _ in range(r.integer()):
        t = r.u8()
        if t == 0:
            consts.append(None)
        elif t == 1:
            consts.append(bool(r.u8()))
        elif t == 3:
            r.raw(r.num_sz)
            consts.append(0.0)
        elif t == 4:
            consts.append(r.string())
        else:
            raise ValueError("bad constant type %d" % t)
    protos = [_proto(r) for _ in range(r.integer())]
    for _ in range(r.integer()):      # line info
        r.integer()
    for _ in range(r.integer()):      # locals
        r.string(); r.integer(); r.integer()
    for _ in range(r.integer()):      # upvalues
        r.string()
    return {"code": code, "consts": consts, "protos": protos}


def parse_luaq(data):
    if data[:4] != b"\x1bLua" or data[4] != 0x51:
        return None
    endian = "<" if data[6] == 1 else ">"
    r = _R(data, endian, data[7], data[8], data[10])
    r.p = 12
    return _proto(r)


def _decode(ins):
    op = ins & 0x3F
    a = (ins >> 6) & 0xFF
    c = (ins >> 14) & 0x1FF
    b = (ins >> 23) & 0x1FF
    bx = ins >> 14
    return (OPNAMES[op] if op < len(OPNAMES) else "OP%d" % op), a, b, c, bx


def _kstr(p, idx):
    v = p["consts"][idx] if 0 <= idx < len(p["consts"]) else None
    return v if isinstance(v, str) else None


def scan_proto(p, set_globals, class_uses, plain_uses):
    """Record SETGLOBAL names, and GETGLOBAL names indexed as a table (-> class.method)."""
    code = p["code"]
    # reg -> global name, valid until that register is written again. A conservative, purely local
    # dataflow: enough because Lua emits GETGLOBAL immediately before its GETTABLE/SELF.
    reg_global = {}
    for ins in code:
        name, a, b, c, bx = _decode(ins)
        if name == "SETGLOBAL":
            g = _kstr(p, bx)
            if g:
                set_globals.add(g)
            reg_global.pop(a, None)
        elif name == "GETGLOBAL":
            g = _kstr(p, bx)
            reg_global[a] = g
            if g:
                plain_uses.add(g)
        elif name in ("GETTABLE", "SELF"):
            src = reg_global.get(b)
            key = _kstr(p, c - 256) if c >= 256 else None
            if src and key:
                class_uses.setdefault(src, set()).add(key)
            reg_global.pop(a, None)
            if name == "SELF":
                reg_global.pop(a + 1, None)
        else:
            reg_global.pop(a, None)   # any other write invalidates the tracked register
    for sub in p["protos"]:
        scan_proto(sub, set_globals, class_uses, plain_uses)


def load_scripts(src):
    """Yield (name, bytes) for every Lua chunk in a cooked dir or a raw .bnk."""
    if src.is_dir():
        for f in sorted(src.rglob("*.lua")):
            yield str(f.relative_to(src)).replace("\\", "/"), f.read_bytes()
        return
    sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools" / "lua_mod"))
    import bnk_repack            # noqa: E402
    import script_index          # noqa: E402
    _, entries = bnk_repack.read_bnk(str(src))
    for e in entries:
        if e["name"].lower().endswith(".lua"):
            yield e["name"], script_index.entry_bytes(e)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("scripts", type=Path, help="cooked scripts dir (or gamescripts_r.bnk)")
    ap.add_argument("--catalog", type=Path,
                    default=Path("ghidra_out/lua_natives5_catalog.tsv"),
                    help="Ghidra native catalog, for method corroboration")
    ap.add_argument("--known", type=Path, default=Path("Fable2Native/src/native_script.cpp"),
                    help="source file whose string lists count as already-known names")
    ap.add_argument("-o", "--out", type=Path, help="write the report here as well as stdout")
    ap.add_argument("--min-hits", type=int, default=3,
                    help="minimum catalog-corroborated methods to ACCEPT a class (default 3)")
    ap.add_argument("--min-ratio", type=float, default=0.5,
                    help="minimum corroborated fraction of a class's methods (default 0.5)")
    args = ap.parse_args(argv)

    set_globals = set()
    class_uses = {}
    plain_uses = set()
    n_files = 0
    for _name, data in load_scripts(args.scripts):
        p = parse_luaq(data)
        if p is None:
            continue
        n_files += 1
        scan_proto(p, set_globals, class_uses, plain_uses)

    # Native method names the EXE is known to bind (corroboration only, never the gate).
    catalog_methods = set()
    if args.catalog.exists():
        for line in args.catalog.read_text(encoding="latin-1").splitlines():
            cols = line.split("\t")
            if len(cols) >= 2:
                catalog_methods.add(cols[1].strip())

    known = set()
    if args.known.exists():
        text = args.known.read_text(encoding="utf-8")
        for chunk in text.split('"')[1::2]:
            if chunk.isidentifier():
                known.add(chunk)

    engine = {g: sorted(ms) for g, ms in class_uses.items()
              if g not in set_globals and g[:1].isupper()}

    lines = [
        "# Engine-provided script globals, derived from the game's own Lua bytecode",
        "# scripts scanned: %d   globals SET by scripts: %d" % (n_files, len(set_globals)),
        "# A global listed here is indexed as a table by some script but is never SETGLOBAL'd by",
        "# ANY script -> the engine must provide it. 'catalog' = how many of its method names the",
        "# Ghidra native catalog also knows as bound natives (independent EXE-side evidence).",
        "#",
        "# status\tclass\tmethods_used\tcatalog_hits\tsample_methods",
    ]
    # The ACCEPT gate: two independent sources must agree - (1) no script ever defines the global
    # yet some script indexes it as a table, and (2) the EXE catalog knows enough of its method
    # names as bound natives. Rejected rows are overwhelmingly script-defined quest/behaviour
    # classes whose definition this scanner's local dataflow did not see; stubbing THOSE would be
    # actively harmful (a real game table whose nil-field reads turn into stubs breaks its own
    # logic), so the gate errs toward leaving them alone.
    accepted, rejected = [], []
    for g in sorted(engine, key=lambda k: (-len(engine[k]), k)):
        methods = engine[g]
        hits = sum(1 for m in methods if m in catalog_methods)
        ok = hits >= args.min_hits and hits >= args.min_ratio * len(methods)
        status = "known" if g in known else ("ACCEPT" if ok else "reject")
        if status == "ACCEPT":
            accepted.append(g)
        elif status == "reject":
            rejected.append(g)
        lines.append("%s\t%s\t%d\t%d\t%s" % (status, g, len(methods), hits, ",".join(methods[:8])))
    lines.append("")
    lines.append("# ACCEPTED as engine-provided but MISSING from the port's native-name set (%d)."
                 % len(accepted))
    lines.append("# Gate: catalog_hits >= %d AND >= %.2f of the methods used."
                 % (args.min_hits, args.min_ratio))
    for i in range(0, len(accepted), 6):
        lines.append("    " + " ".join('"%s",' % m for m in accepted[i:i + 6]))
    lines.append("")
    lines.append("# rejected (%d): indexed-but-undefined yet NOT corroborated by the EXE catalog."
                 % len(rejected))
    lines.append("# Almost all are script-defined quest/behaviour classes. FLAGGED, left alone.")
    lines.append("# " + " ".join(rejected))
    report = "\n".join(lines)
    print(report)
    if args.out:
        args.out.write_text(report + "\n", encoding="utf-8")
        print("\n[wrote] %s" % args.out, file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
