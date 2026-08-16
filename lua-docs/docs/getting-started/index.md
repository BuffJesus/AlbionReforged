# Getting started

There are two things you might want to do with Fable II's Lua, and this section
covers both:

<div class="grid cards" markdown>

-   :material-file-search: **[Read the scripts](read-the-scripts.md)**

    Decompile or disassemble the game's compiled Lua back into readable source —
    the ground truth for how any system works.

-   :material-play: **[Run the scripts](run-the-scripts.md)**

    Boot the game's script bank on the Albion Reforged native runtime and drive a
    real quest.

</div>

## Prerequisites

- **A legally-owned copy of Fable II**, extracted so the runtime can read its data
  directory. The scripts live in `data/gamescripts_r.bnk` (gameplay) and
  `data/guiscripts.bnk` (front-end). Albion Reforged ships none of this.
- **The Albion Reforged tree** (this repository) for the runtime and tooling.
- **Python 3** for the RE tooling (`tools/lua_mod/*`).
- A **C++ toolchain** (CMake + a recent MSVC/Clang) to build the native runtime.

## The 30-second mental model

Fable II compiles its Lua to **LuaQ** (Lua 5.1 bytecode) and packs it into a
**BNK** container. To do anything with it you either:

1. **Inflate + undump** the bytecode and hand it to a Lua 5.1 VM to *run* it, or
2. **Inflate + decompile** it back to source to *read* it.

Both start from the same place — pulling an entry out of the BNK and inflating its
zlib streams. From there, the runtime path and the reading path diverge.

```
             ┌── run  →  embedded Lua 5.1 VM (Albion Reforged runtime)
BNK entry ──▶┤
             └── read →  f2lua_decompile.exe  /  luadis51.py
```

Start with **[Read the scripts](read-the-scripts.md)** if you want to understand a
system, or **[Run the scripts](run-the-scripts.md)** if you want to see them
execute.
