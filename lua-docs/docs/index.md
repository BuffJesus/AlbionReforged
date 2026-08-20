# Fable II Lua on Albion Reforged

Fable II's gameplay — quests, gameflow, interactables, NPC behaviour glue, the
front-end — is driven by **Lua**. The Xbox 360 game ships ~550 compiled Lua
scripts and runs them on an embedded **Lua 5.1** VM. **Albion Reforged** brings
that scripting layer to PC: it reads the game's own script bank, loads the same
bytecode into an embedded Lua 5.1 VM, and runs the game's real managers and
quests on native systems — no reimplementation of the scripts themselves.

!!! success "Milestone: the game's first chapter plays its opening"
    `QC010` — the **childhood opener**, the game's own first quest — runs its real
    `Update`, finds its real cast, **speaks the game's own dialogue** (`book.babel`
    is decoded), paces its cutscene beats on **authored** timing, and frames its
    **authored** camera. 160 miscellaneous scripts + the quest bank (QuestManager +
    27 quest modules + gameflow) load with **0 missing natives** at boot.

    What's stubbed is the *world* underneath — see the
    [honest status](status.md).

## What this documents

This site is the map of that system — both the **reverse engineering** (how
Fable II's Lua actually works) and the **native runtime** (how Albion Reforged
loads and runs it):

<div class="grid cards" markdown>

-   :material-rocket-launch: **[Getting started](getting-started/index.md)**

    Read the game's Lua (decompile / disassemble) and run it on the native
    runtime.

-   :material-lightbulb-on: **[Concepts](concepts/index.md)**

    The formats and systems: BNK container, LuaQ bytecode, the embedded VM,
    managers, quests, message events, entities, the GDB/text authored data,
    cutscenes, the auto-stub.

-   :material-book-open-variant: **[Reference](reference/native-api.md)**

    The native API surface, a source-file map, and the tooling.

-   :material-heart: **[Contributing](contributing.md)**

    Where the frontier is and how to extend the runtime.

</div>

## How it fits together

```
  gamescripts_r.bnk  (the game's script bank, on the user's disc)
        │  BnkReader — inflate v3 zlib streams
        ▼
  LuaQ bytecode  (Lua 5.1, 32-bit little-endian, lua_Number = float32)
        │  luaU_undump  (VM patched to accept the 32-bit/float header)
        ▼
  Embedded Lua 5.1 VM  (NativeScriptVM)
        │  boot_game_scripts → generalsetupscript (159 misc scripts + managers)
        │  load_quest_scripts → questsetupscript (QuestManager + 27 quests + gameflow)
        ▼
  Real managers drive real coroutines each tick
   QuestManager.Update / GeneralScriptManager.Update / AIManager.Update
        │  natives ← the runtime provides GetPlayerHero, MessageEvents, entities,
        │             GDB records, localised text, cutscene requests …
        ▼
  Quests run their bytecode: Update → WaitFor → message poll → completion
        │  the WHAT is authored data, not script: GDB beat lists + book.babel text
        ▼
  A cutscene plays: authored beats, authored timing, authored camera, real words
```

## Design stance

- **Run the game's scripts, don't rewrite them.** The quest/gameflow logic is the
  game's own compiled Lua. The runtime supplies the *natives* those scripts call,
  not the scripts.
- **Everything is grounded in the decompiled scripts and the decomp catalog.**
  Where a value or behaviour couldn't be recovered, it is **flagged** as an
  engineering stand-in rather than presented as fact.
- **Honest status.** The [status page](status.md) says exactly what runs, what is
  stubbed, and what the next frontier is.

!!! warning "Legal"
    Albion Reforged ships **no** game code or assets. It reads a copy of Fable II
    that you own. The scripts described here live in the game's data; this project
    only documents and runs them.
