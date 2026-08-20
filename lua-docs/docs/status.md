# Honest status

*Last updated: 2026-08-19.*

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
- [x] **The real first quest runs.** `QC010` (the childhood opener) runs its own
  `Update`, reaches its beats, and drives the opening — not a demo quest, the
  game's own chapter one.
- [x] **Entity search is real.** `SearchTools.FilterWithName` resolves against the
  world's named entities, so `StartNewEntityThread` spawns a thread per real match.
  See [Entities](concepts/entities.md#entity-search--now-real).
- [x] **Authored data is readable.** The **GDB** database (records, name table,
  string table, `parent` inheritance) and **`book.babel`** localised text (the codec
  is plain zlib) are both decoded and bound to Lua. See
  [Authored data](concepts/authored-data.md).
- [x] **The port speaks the game's words.** A cutscene's `SayLine` resolves its
  `TextTag` through babel, so dialogue is the shipped English, not placeholders.
- [x] **Cutscenes play on authored timing.** The `SceneElements` beat list is read
  in authored order and paced by authored durations; `SetLookAtCamera` frames the
  authored camera, and `PlayAnimation` / `MoveToMarker` are staged on the named
  character. See [Cutscenes](concepts/cutscenes.md).

## Deliberate stand-ins (flagged)

These are **not bugs** — they are subsystems that aren't wired yet, replaced by
honest placeholders so the parts that *do* work can be exercised. Each is flagged
in the source.

| Stand-in | Why | Replaced by |
|---|---|---|
| Runtime-spawned entities have no archetype | `Debug.CreateEntityAt` makes a transform, not a GDB-built entity, so there is no mesh/AI/animation | Archetype instantiation from GDB (`ghidra_out/gdb_instantiation_re.txt`) |
| `SearchTools` area + script filters are no-ops | Only the name filter is backed; both others would only *narrow* a set | Real spatial search + predicate filters |
| `MoveToMarker` sets the transform directly | No navmesh pathing yet | Real pathing on the nav motor |
| `SayLine` duration is base + per-character | The retail duration comes from the VO asset's length | Wiring the dialogue audio |
| Staged NPC animation is recorded, not visible | Character mesh + clip cook is owned by the environment track | The ENV-owned character cook |
| The cooked stage is `chapter2slums` | The childhood is authored on `bwsslums/defaultscenario` (different heightfield / `.genv` / models / entity GDB, per the game's own `.list`) | Cooking `bwsslums/defaultscenario` (ENV) |
| Save / permanents registration neutralized | `AddQuestToPermanentsTables` reaches into the save subsystem | Wiring the save/permanents system |
| `GetPlatform()` / `Platform` constants | Arbitrary but self-consistent | A real platform identity if ever needed |

The quest logic itself — `Update`, `WaitFor`, the beat sequencing, the dialogue
choice — is **100% the game's real code and the game's real data**. What is stubbed
is the *world* underneath it.

## Next frontier

Ranked — biggest structural gap first:

1. [ ] **Archetype instantiation from GDB** — anything spawned at runtime
   (`Debug.CreateEntityAt`, e.g. `CreatureDogHero`) has no authored record, so no
   mesh, no AI, no resolvable animation. The largest remaining item.
2. [ ] **Make a staged NPC animation visible** — needs the ENV-owned character-mesh
   + clip cook.
3. [ ] **Real pathing for `MoveToMarker`**.
4. [ ] **Cook `bwsslums/defaultscenario`** so the childhood plays on its own stage.
5. [ ] **The last silent errors** — `DummyObjects` enum *values* need RE, two
   natives must return real numbers, and one nil index is still unattributed.
6. [ ] **The save/permanents subsystem** so quest state persists.

## How claims are verified

- **Boot + quest run:** `f2native_core_tests` (`test_boot_game_scripts`,
  `test_run_real_quest`) — `EXIT=0`, asserts the quest's completion print. The suite
  is green both with and without a loaded world.
- **The real quest:** `test_childhood_stub_census` runs `QC010` for a frame horizon
  and writes `docs/childhood_stub_census.txt` — dialogue spoken, staged actions,
  entity-thread match counts, and any **silent** coroutine error. Every quest bug on
  this track was found there. See
  [the census](getting-started/run-the-scripts.md#the-stub-census--the-measurement-harness).
- **App stability:** both the D3D12 and Vulkan front-ends launch with
  `--scripting --game-scripts` and stay alive.
- **Ground truth:** every script referenced here was decompiled from the game's own
  bank with the project's tooling (see [Tooling](reference/tooling.md)).

!!! note "A word on test hygiene"
    The test suite builds under `RelWithDebInfo`, which defines `NDEBUG` and strips
    `assert()`. The Lua/runtime tests therefore use an `F2_CHECK` macro that fires
    in every configuration — so a green run means the assertions actually ran, not
    just that nothing crashed.
