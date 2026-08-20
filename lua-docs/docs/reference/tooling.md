# Tooling

Everything you need to read, disassemble, extract, and regenerate the game's Lua.

## `script_index.py` — list / search / extract

A semantics-free helper around the BNK reader.

```bash
# List every entry
python tools/lua_mod/script_index.py --bnk <bnk> list

# Search entry names / contents
python tools/lua_mod/script_index.py --bnk <bnk> search <term>

# Extract one entry (raw LuaQ) to a directory, preserving its path
python tools/lua_mod/script_index.py --bnk <bnk> extract --name <substr> --out <dir>

# Disassemble via the built-in disassembler
python tools/lua_mod/script_index.py --bnk <bnk> disasm --name <substr>
```

`entry_bytes(entry)` is the canonical inflate — the reference the C++ `BnkReader`
was validated against.

## `f2lua_decompile.exe` — decompile to source

```bash
Fable2AssetBrowser/source/build_lua_cli/f2lua_decompile.exe <raw.lua> > out.lua
```

Takes a raw LuaQ chunk (extract it first) and prints readable Lua. Best for reading
data-heavy scripts; for control-flow-heavy functions cross-check with the
disassembler.

## `luadis51.py` — disassemble

```bash
python tools/lua_mod/luadis51.py <bnk> <entry-substring> [out.txt]
```

A full Lua 5.1 disassembler for the 360's little-endian, `float32`-number chunks:
recursive over nested prototypes, `RK` operands resolved to constants. Trust it over
the decompiler for loop/branch structure.

## `decompile_all.py` — regenerate the whole tree

```bash
python tools/lua_mod/decompile_all.py <bnk> <out_dir>
```

Inflates every entry, decompiles the LuaQ ones, copies the already-source ones
verbatim, and keeps any that won't decompile as raw `.luaq` — so nothing is lost.
Used to produce `lua-decompiled/`.

## `bnk_repack.py` — read / repack

```bash
python tools/lua_mod/bnk_repack.py <bnk>                  # summarize
python tools/lua_mod/bnk_repack.py <bnk> --roundtrip out.bnk
```

Reads all entries and can rebuild the container (replacing/adding entries),
recompressing only what changed — the basis for script mods.

## Reading the authored data

These live in `Fable2Native/tools/` and turn the game's data tables back into text.
See [Authored data](../concepts/authored-data.md).

```bash
# Any GDB record by name, with readable field names and nested sub-records
python Fable2Native/tools/gdb_record_dump.py <level.gdb> QC010_JeevesGreet

# A named level entity's record, component fields resolved to component names
python Fable2Native/tools/gdb_entity_dump.py <level.save> <level.gdb> QC010_Rose

# Localised text: tag -> the real in-game string (the codec is plain zlib)
python Fable2Native/tools/babel_text.py <book.babel> --tag TEXT_LEVEL_FAIRFAX_CASTLE
python Fable2Native/tools/babel_text.py <book.babel> --grep FAIRFAX     # search decoded text
python Fable2Native/tools/babel_text.py <book.babel> --cook out.f2text  # runtime lookup package

# Cook a level's named quest entities/markers into the .f2names sidecar
python Fable2Native/tools/cook_quest_markers.py <level> --out <level>.f2names

# Which animation slots a character's record actually declares
python Fable2Native/tools/gdb_anim_slots.py <level.gdb> QC010_Rose
```

## Diagnosing scripts

```bash
# Derive the ENGINE-PROVIDED script globals from the game's own bytecode
# (what the Ghidra native catalog alone couldn't resolve)
python Fable2Native/tools/script_engine_globals.py <bnk>

# A traceback says "<?:764>" — which script defines that function?
python Fable2Native/tools/find_proto_by_line.py <bnk> 764
```

`find_proto_by_line.py` exists because a **stripped** LuaQ proto still keeps
`linedefined` / `lastlinedefined`. That makes a traceback's `<?:N>` an exact
fingerprint, and it is the fastest way to attribute a
[silently swallowed coroutine error](../concepts/quests.md#errors-inside-a-quest-coroutine-are-swallowed).

!!! warning "…and then read the function"
    `f@764` looked like a quest predicate; it is `questmanager.lua`'s internal
    `WaitFor` yield helper. The fingerprint tells you *where*, not *what*.

## Which banks?

| Bank | Contents |
|---|---|
| `data/gamescripts_r.bnk` | gameplay: quests, gameflow, managers, interactables, enums (553 entries) |
| `data/guiscripts.bnk` | the front-end / GUI scripts (114 entries) |

!!! tip "The LuaQ header, at a glance"
    `1B 4C 75 61 51` = a valid chunk. If your extraction doesn't start with that,
    the inflate is wrong — see [BNK format](../concepts/bnk-format.md).
