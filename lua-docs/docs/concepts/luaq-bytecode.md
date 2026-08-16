# LuaQ bytecode

Fable II's scripts are **LuaQ** — precompiled Lua 5.1 chunks. Getting a stock Lua
5.1 VM to *load* them on a 64-bit PC took two targeted patches, because the game's
bytecode was compiled for the 32-bit Xbox 360 with a non-default number type.

## The header

Every chunk begins with a 12-byte header:

```
1B 4C 75 61 51   00     01       04        04         04            04           00
└ signature ┘   ver=51 format  endian=LE  sizeof     sizeof         sizeof       integral
  "\x1bLuaQ"                              (int)=4   (size_t)=4    (Instruction)  (lua_Number)=4  flag=0
```

Two fields are unusual for a modern host build:

- **`sizeof(size_t) = 4`** — the game is a **32-bit** build. On a 64-bit host, a
  stock VM expects `8` here and rejects the header.
- **`sizeof(lua_Number) = 4`** — the game uses a **`float32`** number type, not the
  default `double`. A stock VM expects `8`.
- **`endian = 1`** — little-endian. Even though the 360 CPU is big-endian, the
  scripts were cross-compiled little-endian on the PC toolchain, so no byte-swapping
  is needed on a little-endian host.

## Patch 1 — `lua_Number = float`

`luaconf.h` is patched so the embedded VM's number type is `float`:

```c
#define LUA_NUMBER   float          /* was double */
/* LUA_NUMBER_DOUBLE left undefined; LUA_NUMBER_FMT = "%.7g" */
```

This makes `sizeof(lua_Number)` in the built VM equal `4`, matching the header.

!!! warning "float32 numbers are a real constraint"
    Because Lua numbers are 32-bit floats, integers above 2²⁴ lose precision. This
    is faithful to the game (its VM ran the same way), but it means, e.g., a 32-bit
    FNV hash used as a table key is stored as a truncated float — consistent, but
    not exact. Handles that must be exact are represented as **lightuserdata**, not
    numbers (see [The embedded VM](the-vm.md)).

## Patch 2 — 32-bit `size_t` in the undumper

The `float` patch is **necessary but not sufficient**. The loader (`lundump.c`)
still reads `size_t`-typed fields (string lengths) at the host's width — 8 bytes on
x64 — while the game wrote 4. That desyncs the stream and yields
`"bad header in precompiled chunk"`.

`lundump.c` is patched so the loader treats on-disk `size_t` as a fixed **4-byte
little-endian** value, and advertises `sizeof(size_t) = 4` in the header it
compares against:

```c
/* Read a 4-byte size regardless of host sizeof(size_t) (8 on x64). */
static size_t LoadSize(LoadState* S) {
  unsigned int x;        /* 4 bytes on every target */
  LoadVar(S, x);
  return (size_t)x;
}
/* … and in luaU_header: advertise sizeof(size_t) = 4 */
```

Only `LoadString` reads a `size_t` in a chunk, so these two edits are the whole fix.
`int`, `Instruction`, and (post-patch) `lua_Number` are already 4 bytes on the host
and match the game.

## Result

With both patches, the VM undumps the game's chunks in full — functions, constants,
debug info, and nested prototypes. All 553 entries load; the ~160 boot scripts and
27 quest modules run.

!!! note "Where"
    `Fable2Native/third_party/lua-5.1.5/luaconf.h` (number type) and
    `Fable2Native/third_party/lua-5.1.5/lundump.c` (`LoadSize` + header). Both
    changes are tagged `FABLE2NATIVE PATCH`.
