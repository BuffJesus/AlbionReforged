# Honest status

*Last updated: 2026-08-16.*

This page says exactly what works, what is a deliberate stand-in, and what the
next frontier is. Nothing here is aspirational — if it's listed as working, there
is a test or a repro behind it.

## What runs today

- [x] **The script bank decompresses.** `BnkReader` reads `gamescripts_r.bnk` (v3,
  553 entries) and inflates every entry — the small sequential-stream entries and
  the large 32 KB-block entries. See [BNK format](concepts/bnk-format.md).
- [x] **The game's LuaQ loads.** The embedded Lua 5.1 VM accepts the game's
  32-bit, little-endian, `float32`-`lua_Number` bytecode. See
  [LuaQ bytecode](concepts/luaq-bytecode.md).
- [x] **The boot chain runs.** `generalsetupscript` loads 159 miscellaneous
  scripts + the real `GeneralScriptManager`; `questsetupscript` loads
  `QuestManager` + 27 quest modules + gameflow. **0 missing natives** during the
  boot ticks.
- [x] **The real managers drive real coroutines.** The game's own
  `QuestManager.Update` / `GeneralScriptManager.Update` run each tick and resume
  their quests' coroutines. See [Managers](concepts/managers.md).
- [x] **A real quest completes.** `MyFirstQuest` runs its actual `Update`
  coroutine, yields at its `WaitFor`, and — once its completion flag is set —
  resumes and prints its own `"Terminating quest now"`. Covered by the
  `test_run_real_quest` unit test.
- [x] **The message-event bus.** Quests can poll `MessageEvents.IsMessagePosted` /
  `IsMessageSentTo` / `IsMessageSentBy`, read `Event` payloads, and be released by
  a posted message. See [Message events](concepts/message-events.md).

## Deliberate stand-ins (flagged)

These are **not bugs** — they are subsystems that aren't wired yet, replaced by
honest placeholders so the parts that *do* work can be exercised. Each is flagged
in the source.

| Stand-in | Why | Replaced by |
|---|---|---|
| `StartNewEntityThread` neutralized in the quest demo | Entity threads need the world entity-search / streaming system | Real `SearchTools` results once world streaming lands |
| Save / permanents registration neutralized | `AddQuestToPermanentsTables` reaches into the save subsystem | Wiring the save/permanents system |
| `SearchTools.*` returns an empty result set | No named world entities are streamed yet | Entity streaming + a real search index |
| `GetPlatform()` / `Platform` constants | Arbitrary but self-consistent | A real platform identity if ever needed |

The quest's own `Update` / `WaitFor` / completion is **100% the game's real code**
— only the two dependencies above are stubbed.

## Next frontier

- [ ] **Entity search + streaming** so `StartNewEntityThread` spawns real entity
  threads (the QuestGiver/EvilTwin pattern in `MyFirstQuest`).
- [ ] **The `GeneralScriptManager` boot-coroutine path** (a boot script schedules a
  coroutine the general manager can't yet resume cleanly).
- [ ] **The childhood chapter (`QC010_Childhood`)** end-to-end — the real first
  quest, which needs GUI message boxes, gameflow position, and the interaction
  chain.
- [ ] **The save/permanents subsystem** so quest state persists.

## How claims are verified

- **Boot + quest run:** `f2native_core_tests` (`test_boot_game_scripts`,
  `test_run_real_quest`) — `EXIT=0`, asserts the quest's completion print.
- **App stability:** both the D3D12 and Vulkan front-ends launch with
  `--scripting --game-scripts` and stay alive.
- **Ground truth:** every script referenced here was decompiled from the game's own
  bank with the project's tooling (see [Tooling](reference/tooling.md)).

!!! note "A word on test hygiene"
    The test suite builds under `RelWithDebInfo`, which defines `NDEBUG` and strips
    `assert()`. The Lua/runtime tests therefore use an `F2_CHECK` macro that fires
    in every configuration — so a green run means the assertions actually ran, not
    just that nothing crashed.
