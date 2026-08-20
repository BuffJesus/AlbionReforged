# Native API

The natives the runtime implements **for real** (everything else falls through to
the [auto-stub](../concepts/auto-stub.md)). Bound in
`Fable2Native/src/native_bindings.cpp` unless noted.

!!! danger "Two rules before you add one"
    Scripts call natives **method-style** (`self` is argument **1**), and
    `lua_Number` is **`float32`** (32-bit ids must cross as handles or tokens).
    Both failure modes are **silent**. See [pitfalls](../concepts/the-vm.md#pitfalls).

## Manager registration

The game's managers register themselves; the runtime captures and drives them each
tick in the retail order **Quest → General → AI**.

| Native | Effect |
|---|---|
| `SetQuestUpdateFunction(fn)` | capture `fn`, call it each tick |
| `SetGeneralScriptManager(tbl)` | capture `tbl.Update`, call it each tick |
| `SetAIManager(tbl)` | capture `tbl.Update`, call it each tick |

## Hero & entities

| Native | Returns / effect |
|---|---|
| `GetPlayerHero()` | the hero entity object |
| `GetIDFromEntity(e)` | the entity's numeric id |
| `Debug.CreateEntityAt(class, name, pos)` | spawn an entity (3rd arg may be a `CVector3` or three scalars), return its handle |
| `EntityManager.GetEntityFromRecordID(id)` | the entity for a GDB record token |
| `entity:GetPosition()` | a **`CVector3`** |
| `entity:GetPositionXYZ()` | the same, as three numbers |
| `entity:SetPosition(x, y, z)` | move the entity |
| `entity:GetName()` / `GetID()` / `GetTableKey()` | identity |
| `entity:IsAlive()` / `Kill()` / `GetCorpse()` | life state |
| `entity:SetAsLevelSaving(b)` | level-save flag |

## World queries

| Native | Today |
|---|---|
| `SearchTools.StartNewSearch(area)` | a handle (**area ignored — flagged**) |
| `SearchTools.FilterWithName(s, name)` | **real** — matches the world's named entities |
| `SearchTools.FilterWithScriptFilter(s, fn)` | no-op (**flagged**: a narrowing filter, so a no-op keeps the set) |
| `SearchTools.GetSearchResults(s)` | the matched entity handles |

## Message events

| Native | Returns |
|---|---|
| `MessageEvents.GetMostRecentMessageID()` | tail id |
| `MessageEvents.IsMessagePosted(type, afterId)` | newest matching `Event` or `nil` |
| `MessageEvents.IsMessageSentTo(type, entity, afterId)` | …to a recipient |
| `MessageEvents.IsMessageSentBy(type, entity, afterId)` | …from a sender |
| `MessageEvents.GetAllMessages(...)` | every matching message, not just the newest |
| `MessageEvents.GetActivatedEntityMessages(...)` | activation messages for an entity |
| `MessageEvents.GetDestroyedMessageFromEntity(e)` | the entity's destroy message |
| `MessageEvents.PostMessage(type, extra, sentBy, sentTo)` | post a message |
| `event:GetID()` | message id |
| `event:GetExtraDataAsNumber()` | numeric payload |
| `event:GetExtraDataAsID()` | the payload read as a record id (a token) |
| `event:GetEntitySentBy()` | sender entity object |

## Authored data — GDB & text

See [authored data](../concepts/authored-data.md).

| Native | Returns |
|---|---|
| `GDB.RecordExists(name)` | `true` if the name table resolves |
| `GDB.GetRecord(name)` | a `GdbRecord` object |
| `rec:GetBool/GetInt/GetFloat/GetString(field)` | typed field reads |
| `rec:GetRecord(field)` | a sub-record — **a null record, never `nil`, when missing** |
| `rec:GetID()` | the record's token (not its raw GUID) |
| `GetText(tag)` / `Text.GetText(tag)` | the game's own localised string for a `TextTag` |

## Cutscenes & camera

See [cutscenes](../concepts/cutscenes.md).

| Native | Effect |
|---|---|
| `AIManager:RequestCutsceneOnEntity(e, id, opts)` | queue a scene (**`self` is arg 1**) |
| `AIManager:GetRequestedCutsceneOnEntity(e)` | the pending scene id |
| `AIManager:ClearCutsceneOnEntity(e)` | drop the request |
| `ScriptFunction.HasStartedInteractiveCutscene(e)` | has the scene begun |
| `ScriptFunction.StopAnyInteractiveCutscene(e)` | stop it |
| `GroupMindManager.GetCutsceneGroupMindContainingEntity(e)` | the scene's group mind |
| `CameraManager.SetCameraOverride(...)` / `ClearCameraOverride()` | scripted camera |
| `Camera.GetPosition/SetAngles/GetAngles/GetFOV/SetFOV` | camera state |
| `AddCameraScriptFile(name)` | register an engine-loaded camera script |

## Movement & physics

The public forms take `CVector3` objects; a boot-installed Lua shim unpacks them and
calls the `__`-prefixed scalar primitives.

| Native | Effect |
|---|---|
| `Navigation.MoveTo(e, pos, opts)` → `__MoveTo` | walk to a point with an arrival radius |
| `Navigation.StopMoving(e)` / `GetCurrentSpeed(e)` / `GetMovementPaused(e)` | motor state |
| `Physics.GetFacingVector(e)` / `GetVelocity(e)` | via `__GetFacingRaw` / `__GetVelocityRaw` |
| `Physics.SetFacing(e, v)` / `Teleport(e, v)` | via `__SetFacing` / `__Teleport` |
| `Physics.IsAvailable(e)` | does the entity have a physics body |

## Boot / timing / misc

| Native | Returns / effect |
|---|---|
| `RunScript(name)` | inflate + run a named chunk from the bank (deduped) |
| `__bnk_chunk(name)` | the raw chunk, for `require` |
| `GetPlatform()` | a value equal to `Platform.Win32` (self-consistent) |
| `FNVHash(str)` | the game's FNV-1 hash (basis `0x811C9DC5`), a real hash |
| `IsToStartGameflow()` | `true` (fresh-game path) |
| `GetRandomNumber(n)` | an integer in **`[1, n]`** (the game's convention) |
| `IsDistanceBetweenThingsUnder/Over(a, b, d)` | real distance tests |
| `Timing.GetWorldFrame()` / `GetTickRate()` | elapsed frames / `60` |
| `Timing.GetDayCount()` / `SetDayCount(n)` / `AdvanceDayCount()` | in-game day counter |
| `Debug.Log(...)` / `Debug.Error(...)` | logged (and surfaced by the census) |
| `Debug.GetRandomFloat/GetRandomNumber` | RNG |
| `print(...)` | captured into `game.script_log` |

## Core gameplay layer

Bound by `register_native_api` for the standalone gameplay layer (independent of the
game scripts): `Game.Elapsed/GetTimeStep/SetChapter/GetChapter`,
`Player.GetPosition/SetPosition`,
`World.NpcCount/NpcPosition/DamageNpc/GetNpcHealth/SetNpcHealth`, and
`Quest.SetComplete/IsComplete/CountComplete` (a 150-bit completion bitset).

!!! tip "Finding what to implement next"
    After a boot + quest run, `vm.stub_misses()` is the ranked list of natives the
    loaded scripts actually called but the runtime doesn't implement — the
    highest-value work items, straight from the game's own usage. The
    [stub census](../getting-started/run-the-scripts.md#the-stub-census-the-measurement-harness)
    is the fuller version.
