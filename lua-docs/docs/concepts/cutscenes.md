# Cutscenes

The game's set-pieces — the childhood opening, every scripted conversation — are
**interactive cutscenes**: a quest asks the AI manager to play one on an entity,
then waits. The *content* isn't in the script at all; it's an authored beat list in
[GDB](authored-data.md). This is the clearest example of the split: Lua decides
**when**, data decides **what**.

## The request/finish protocol

A quest never plays a scene directly. It requests one and polls:

```lua
AIManager:RequestCutsceneOnEntity(entity, cutsceneId, opts)   -- ask
…
if ScriptFunction.HasStartedInteractiveCutscene(entity) then  -- has it begun?
```

and it is released by a **message event** carrying the scene's record id as extra
data. The two codes are four-character constants:

| Code | u32 | Meaning |
|---|---|---|
| `'ICFS'` | `0x49434653` | interactive cutscene **f**ini**s**hed |
| `'ICFU'` | `0x49434655` | interactive cutscene finished, **u**nsuccessfully |

The waiting quest matches the message's `GetExtraDataAsID()` against the scene it
requested — which is precisely why record ids must cross as
[tokens, not raw GUIDs](authored-data.md#gdb--the-game-database).

!!! danger "`self` is argument 1"
    `AIManager:RequestCutsceneOnEntity(...)` is a **method** call — the entity is
    argument **2**. Reading it from argument 1 gives the manager table, the id
    comes back `0`, and every request is dropped **silently**. This one cost a
    session; see [the VM](the-vm.md#pitfalls).

## The beat list

A cutscene record carries a `SceneElements` sub-record whose **fields, in order,
are the beats**. Repeated names are normal — `SayLine` appears once per line.
Schemas, all decoded from the game's own data:

| Beat | Fields |
|---|---|
| `SayLine` | `Character`, `CharacterToTalkTo`, `TextTag`, `WaitUntilComplete` |
| `Wait` | `TimeToWait` — authored seconds |
| `SetLookAtCamera` | `PositionEntity`, `FocusEntity` — each a marker record whose `PhysicsSimpleComponent.Position` is the camera pose |
| `PlayAnimation` | `Character`, `AnimationName`, `CharacterToFace`, `PlayIntoAndOutof` |
| `MoveToMarker` | `MarkerToMoveTo` (a marker record), `Range` (arrival radius), `WaitUntilComplete` |
| `StartLookingAtCharacter` | `Character`, `CharacterToLookAt` |
| `SetEntityMode` | `Character`, `AnimationGroup` |
| `ScriptCallback` | calls back into the quest |

Field order **is** play order — verified: the `SayLine` beats of
`QC010_JeevesGreet` enumerate as `_02, _04, _10, _20, _30`.

## How the runtime plays one

`build_cutscene_beats(record_id)` walks `SceneElements` once and resolves each beat
against authored data; `update_cutscenes(dt)` then holds the scene for each beat's
duration and advances:

- **`SayLine`** — `TextTag` resolves through [babel](authored-data.md), so the port
  speaks the game's real line. Duration is a base plus a per-character term
  (**flagged**: the retail duration comes from the VO asset's length).
- **`Wait`** — the authored `TimeToWait`, verbatim.
- **`SetLookAtCamera`** — both anchors' `PhysicsSimpleComponent.Position` become the
  camera position and focus, pushed through `CameraManager.SetCameraOverride`.
- **`PlayAnimation`** — the clip *name* resolves through the character's **own**
  `AnimationManagerComponent.Animations` (e.g. `QC010_Rose` has `RoseWarmingUp`,
  `RoseTantrum`, `Idle`). Entries come in two authored shapes: the clip key
  directly as a type-4 raw u32, or a type-6 sub-record of named slots.
- **`MoveToMarker`** — the marker's position, with the authored arrival `Range`.
  (**Flagged**: today this sets the transform; real pathing is pending.)

Beat kinds not yet staged are still **recorded with duration 0**, so the order and
count of a scene stay honest rather than silently shrinking.

!!! note "Where"
    `NativeGame::build_cutscene_beats` / `update_cutscenes` / `perform_cutscene` in
    `Fable2Native/src/native_game.cpp`; the `AIManager.*` / `ScriptFunction.*` /
    `CameraManager.*` bindings in `native_bindings.cpp`. Dump a scene with
    `tools/gdb_record_dump.py`.
