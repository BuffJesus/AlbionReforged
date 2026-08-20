# Read the scripts

The game's Lua is compiled, but it decompiles cleanly. This is the ground truth
for every system documented on this site — when in doubt, decompile the script and
read what it actually does.

## List what's in the bank

`script_index.py` is a thin, semantics-free helper around the BNK reader. Use it to
list, search, and extract entries.

```bash
# List every entry
python tools/lua_mod/script_index.py \
  --bnk Fable2Recomp/assets/game/data/gamescripts_r.bnk list

# Find quests
python tools/lua_mod/script_index.py --bnk <bnk> list | grep -i quest
```

Entry names are backslash-joined and lowercase, e.g.
`scripts\quests\qc010_childhood.lua`.

## Decompile to readable source

`f2lua_decompile.exe` turns a raw LuaQ chunk into readable Lua source. Extract an
entry, then decompile it:

```bash
# Extract one entry to a file (preserves its path under an output dir)
python tools/lua_mod/script_index.py --bnk <bnk> extract \
  --name questmanager --out /tmp/out

# Decompile it
Fable2AssetBrowser/source/build_lua_cli/f2lua_decompile.exe \
  /tmp/out/scripts/quests/questmanager.lua > questmanager.lua
```

!!! tip "Some entries ship as source already"
    A few entries (e.g. `scripts/quests/gameflow.txt`) are readable original source
    rather than compiled LuaQ — no decompilation needed.

## Disassemble (when the decompiler struggles)

The decompiler occasionally produces register-soup for control-flow-heavy
functions (nested `while`/`yield` loops). When a decompiled function looks wrong,
disassemble the bytecode instead — the opcodes don't lie:

```bash
python tools/lua_mod/luadis51.py <bnk> questmanager questmanager.dis.txt
```

`luadis51.py` is a full Lua 5.1 disassembler for the 360's little-endian,
`float32`-number chunks, recursive over nested prototypes with `RK` operands
resolved to constants.

!!! warning "Trust the disassembly over the decompilation for control flow"
    Example: `QuestThreadBase.WaitFor` **decompiles** to a `while true do … end`
    with no visible exit, but the real bytecode is `while not pred() do yield()
    end` — it exits when the predicate becomes true. The decompiler dropped the
    loop condition; the disassembly shows it.

## Regenerating the whole tree

Because the decompiler round-trips the whole bank, you can regenerate a complete
readable Lua tree for reference (the community's old `fable2-decompiled-lua`
mirror is gone — this recreates it):

```bash
# Sketch: iterate every .lua entry, extract, decompile.
python tools/lua_mod/script_index.py --bnk <bnk> list \
  | while read name; do
      python tools/lua_mod/script_index.py --bnk <bnk> extract --name "$name" --out out_raw
      Fable2AssetBrowser/source/build_lua_cli/f2lua_decompile.exe \
        "out_raw/$name" > "out_src/$name" 2>/dev/null
    done
```

## What to read first

| To understand… | Decompile… |
|---|---|
| Boot order | `miscellaneous/generalsetupscript.lua`, `quests/questsetupscript.lua` |
| The scheduler | `miscellaneous/generalscriptmanager.lua`, `quests/questmanager.lua` |
| A minimal quest | `quests/myfirstquest.lua` |
| The first real chapter | `quests/qc010_childhood.lua` |
| Story progression | `quests/gameflow.txt` |
| Event constants | `miscellaneous/messageeventenum.lua` |
