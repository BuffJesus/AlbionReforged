# Quests

A quest is a Lua table with an `Update` method, run as a coroutine by
[`QuestManager`](managers.md). All quests inherit from **`QuestThreadBase`**, which
provides the lifecycle, the wait helpers, and entity/objective utilities.

## Anatomy of a quest

The shipped example `quests/myfirstquest.lua` is the clearest specimen:

```lua
QuestManager.NewQuestThread("MyFirstQuest")     -- register the quest TYPE
function MyFirstQuest.Update(arg0)              -- arg0 = the quest INSTANCE
  arg0.QuestOver  = false
  arg0.KilledTwin = false
  arg0:StartNewEntityThread("QuestGiver", VillagerWithQuest)
  arg0:StartNewEntityThread("EvilTwin",   EnemyToKill)
  arg0:WaitFor(function() return MyFirstQuest.QuestOver end)
  print("Terminating quest now")
end
```

Three things to notice:

1. **Type vs. instance.** `NewQuestThread` creates the quest *type* (a global table).
   `MyFirstQuest:new()` makes an *instance*; the instance's metatable indexes back to
   the type, so instance methods resolve to the type's functions.
2. **Entity threads.** `StartNewEntityThread(name, subtype)` searches the world for
   entities with `name` and spawns a sub-thread per match — **one thread per match,
   and nothing at all when there are none**. It now resolves against real named
   world entities (see [entities](entities.md#entity-search--now-real)).
3. **The wait.** `WaitFor(pred)` yields the coroutine every frame until `pred()`
   becomes truthy, then falls through. Here `pred` reads `MyFirstQuest.QuestOver` —
   the **type's** field, not the instance's.

## Lifecycle

```
NewQuestThread(name)          -- QuestManager creates the type via _NewQuestType
   │
MyFirstQuest:new()            -- QuestThreadBase.new → instance w/ metatable
   │
QuestManager.AddQuestThread(q)-- wraps q.Update in a coroutine, links it in
   │
QuestManager.Update()  ×N     -- resume the coroutine each tick
   │   ├─ Update runs to its first WaitFor → yields (suspended)
   │   ├─ … condition becomes true …
   │   └─ WaitFor's predicate passes → Update continues → returns (dead)
```

`AddQuestThread` validates the instance: it must have been through `:new()` (no raw
`_Name`) and must expose an `Update` function — otherwise it errors with
*"A quest without an Update function has been passed to AddQuestThread"*. Seeing
that error from the game's own code is a good sign: it means `QuestManager` is real
and running.

## The wait-and-poll idiom

Every quest wait is the same shape — poll something, `yield` if not satisfied:

```lua
function QuestThreadBase.WaitFor(self, pred)
  while not pred() do coroutine.yield() end
end
```

The wait helpers layer on top of this and the [message bus](message-events.md):

| Helper | Waits for |
|---|---|
| `WaitFor(pred)` | an arbitrary predicate |
| `WaitForMessage(...)` | a `MESSAGE_EVENT_HIT` sent to the entity |
| `WaitForTriggerToFire(trigger)` | a `MESSAGE_EVENT_TRIGGERED` from a trigger |
| `WaitForTimeInSeconds(t)` | a timer |
| `WaitForLevelToLoad()` | a `MESSAGE_EVENT_LEVEL_LOADED` |
| `DisplayMessageBox(tag)` | `GUI.DisplayMessageBox` then a `MESSAGE_EVENT_INFOBOX` |

!!! warning "Read the disassembly for wait loops"
    The decompiler tends to mangle these `while … yield` loops (it can drop the exit
    condition). If a wait helper looks like an infinite loop in the decompiled
    source, check the [disassembly](../getting-started/read-the-scripts.md#disassemble-when-the-decompiler-struggles).

## Registration gotcha

`NewQuestThread` checks whether a quest of that name is *already* registered by
reading the global. If your native shim fabricates a value for every unknown global
(a naïve auto-stub), that check sees a phantom and refuses to register the quest.
This is exactly why the [auto-stub](auto-stub.md) must return `nil` for names that
aren't real natives — so the game's own globals can be defined.

## Debugging a quest that does nothing

A real quest — the childhood opener `QC010` — exposed the two failure modes that
matter, and neither one produces an error message.

### Errors inside a quest coroutine are **swallowed**

The managers call `coroutine.resume` and **discard its error return**. A quest whose
`Update` dies on frame 0 looks exactly like a quest that is quietly waiting: no
traceback, no log line, nothing. `QC010` sat "stalled" for a long time this way.

The root cause was ordering, not logic: `start_new_game` called `Gameflow:Init()`
**before** setting `Gameflow.GameflowMode`, so `Init` took the game's debug/sandbox
branch, never built `Gameflow.WeaponNames`, and the quest's first line indexed a
`nil`. Ground truth here is unusually good — the bank ships this file's **original
Lua source** as `scripts/quests/gameflow.txt`, so the branch is readable, not
inferred.

The lesson generalises: **when a quest "does nothing", suspect a dead coroutine
before suspecting a wait.**

### Measure, don't read

Static reading of decompiled quest loops sent us down the wrong branch twice. The
[stub census](../getting-started/run-the-scripts.md) answers the same questions with
evidence — call ranking, first-call timeline, the spin set, **silent coroutine
errors with traceback and the last stubbed native**, waits entered, entity threads
with their match counts, dialogue actually spoken, staged actions, entity positions,
and the game's own `Debug.Error` calls.

Every quest bug on this track was found by that harness.

!!! tip "Registration tables are not run lists"
    `DebugQuestStartTable` lists quests that *can* be started from the debug menu.
    An entry there is **registration, not instantiation** — reading it as "this quest
    never runs" is a mistake we made and had to withdraw.
