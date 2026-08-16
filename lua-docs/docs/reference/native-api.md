# Native API

The natives the runtime implements for real (everything else falls through to the
[auto-stub](../concepts/auto-stub.md)). Bound in `Fable2Native/src/native_bindings.cpp`
unless noted.

## Manager registration

The game's managers register themselves; the runtime captures and drives them.

| Native | Effect |
|---|---|
| `SetGeneralScriptManager(tbl)` | capture `tbl.Update`, call it each tick |
| `SetQuestUpdateFunction(fn)` | capture `fn`, call it each tick |
| `SetAIManager(tbl)` | capture `tbl.Update`, call it each tick |

## Hero & entities

| Native | Returns / effect |
|---|---|
| `GetPlayerHero()` | the hero entity object |
| `GetIDFromEntity(e)` | the entity's numeric id |
| `Debug.CreateEntityAt(class, name, x, y, z)` | spawn an entity, return its handle |
| `entity:GetPosition()` | `x, y, z` |
| `entity:SetPosition(x, y, z)` | move the entity |
| `entity:GetName()` | the entity's name |
| `entity:GetID()` | the entity's uid |
| `entity:IsAlive()` | `true` unless killed |
| `entity:Kill()` | set health to zero |

## Message events

| Native | Returns |
|---|---|
| `MessageEvents.GetMostRecentMessageID()` | tail id |
| `MessageEvents.IsMessagePosted(type, afterId)` | newest matching `Event` or `nil` |
| `MessageEvents.IsMessageSentTo(type, entity, afterId)` | …to a recipient |
| `MessageEvents.IsMessageSentBy(type, entity, afterId)` | …from a sender |
| `MessageEvents.PostMessage(type, extra, sentBy, sentTo)` | post a message |
| `event:GetID()` | message id |
| `event:GetExtraDataAsNumber()` | numeric payload |
| `event:GetEntitySentBy()` | sender entity object |

## World queries (stand-ins)

| Native | Today | Eventual |
|---|---|---|
| `SearchTools.StartNewSearch(area)` | opaque handle | real spatial search |
| `SearchTools.FilterWithName(s, name)` | no-op | filter by name |
| `SearchTools.FilterWithScriptFilter(s, fn)` | no-op | filter by predicate |
| `SearchTools.GetSearchResults(s)` | empty `{}` | matched entities |

## Boot / platform / misc

| Native | Returns / effect |
|---|---|
| `RunScript(name)` | inflate + run a named chunk from the bank (deduped) |
| `GetPlatform()` | a value equal to `Platform.Win32` (self-consistent) |
| `FNVHash(str)` | the game's FNV-1 hash (basis `0x811C9DC5`), a real hash |
| `IsToStartGameflow()` | `true` (fresh-game path) |
| `Timing.GetWorldFrame()` | elapsed frames at 60 Hz |
| `Timing.GetTickRate()` | `60` |
| `print(...)` | captured into `game.script_log` |

## Also available (core gameplay natives)

Bound by `register_native_api` for the standalone gameplay layer (independent of
the game scripts): `Debug.Log`, `Debug.GetRandomFloat/Number`, `Game.Elapsed/
GetTimeStep/SetChapter/GetChapter`, `Player.GetPosition/SetPosition`,
`World.NpcCount/NpcPosition/DamageNpc/GetNpcHealth/SetNpcHealth`,
`Camera.GetPosition`, and `Quest.SetComplete/IsComplete/CountComplete` (a 150-bit
completion bitset).

!!! tip "Finding what to implement next"
    After a boot + quest run, `vm.stub_misses()` is the ranked list of natives the
    loaded scripts actually called but the runtime doesn't implement — the
    highest-value work items, straight from the game's own usage.
