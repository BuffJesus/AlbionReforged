# Concepts

The systems that make Fable II's Lua work, from the bytes on disc up to a running
quest. Read them in order for the full picture, or jump to what you need.

<div class="grid cards" markdown>

-   **[BNK container format](bnk-format.md)** — how scripts are packed and
    compressed in `gamescripts_r.bnk`.

-   **[LuaQ bytecode](luaq-bytecode.md)** — the 32-bit, little-endian, float-number
    Lua 5.1 bytecode, and the two patches that make it loadable on a 64-bit host.

-   **[The embedded VM](the-vm.md)** — `NativeScriptVM`: the Lua-free C++ surface,
    the object/handle bridge, and coroutine-safe natives.

-   **[Script managers](managers.md)** — QuestManager / GeneralScriptManager /
    AIManager, and the coroutine scheduler the game owns.

-   **[Quests](quests.md)** — `QuestThreadBase`, quest lifecycle, `WaitFor`, and the
    poll-and-yield idiom.

-   **[Message events](message-events.md)** — the event bus quests block on, and how
    a posted message releases a waiting quest.

-   **[Entities & the object model](entities.md)** — how entities/quests/events cross
    the C++↔Lua boundary as first-class objects.

-   **[The auto-stub](auto-stub.md)** — how unimplemented natives are absorbed so the
    game's scripts run, without breaking the game's own globals.

</div>

## The layered picture

```
 Auto-stub ─────────────────────────────────────────────┐ (absorbs unimplemented natives)
 Quests / gameflow  (the game's Lua)                     │
 QuestManager · GeneralScriptManager · AIManager  (Lua)  │
 Native API  (GetPlayerHero, MessageEvents, entities …)  │  ← the C++ boundary
 NativeScriptVM  (embedded Lua 5.1, object bridge)       │
 LuaQ undump  (32-bit / float patches)                   │
 BnkReader  (v3 zlib inflate)                            │
 gamescripts_r.bnk  (on the user's disc)  ──────────────┘
```
