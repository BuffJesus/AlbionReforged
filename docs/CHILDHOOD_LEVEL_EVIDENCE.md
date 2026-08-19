# Which level/scenario is the childhood? — evidence file

Written 2026-08-19 (branch `agent/native-gameplay-core`). Every line below is either a MEASURED
command output or a cited instruction index in the game's own Lua bytecode. Anything not pinned by
those is marked ⚠ UNPINNED and must not be built on.

Disassembly is reproducible with:

```
python tools/lua_mod/luadis51.py Fable2Recomp/assets/game/data/gamescripts_r.bnk <substr> <out.txt>
```

Level/entity data is reproducible with `f2tool` + `npc_markerdump` (see
`Fable2Native/tools/cook_quest_markers.py`, which wraps both).

---

## 1. The childhood's level is `BWSSlums` — PINNED

`quests/gameflow.lua`, main chunk instr **274-281**:

```
274 SELF     R1 := R0['RegisterDebugQuest']
276 GETTABLE R3 := ScriptEnum['DebugQC010']
277 LOADK    R4 := 'QC010_Childhood'
278 LOADK    R5 := 'Childhood'
279 LOADK    R6 := 'BWSSlums'
280 LOADK    R7 := 'QC010_ChildhoodStart'
281 CALL     R1(args=6, ret=0)
```

So the registration is `RegisterDebugQuest(DebugQC010, 'QC010_Childhood', 'Childhood', 'BWSSlums',
'QC010_ChildhoodStart')`: **level = `BWSSlums`, start marker = `QC010_ChildhoodStart`**. No
scenario is named at this site.

**Consequence for the runtime probe:** `Gameflow.DebugQuestStartTable` is written by
`RegisterDebugQuest`, so finding `QC010_Childhood` in it with no coroutine means only that the
quest is REGISTERED. It is **not** evidence that instantiation failed. ⚠ An earlier reading in
this session drew that stronger conclusion; it is withdrawn. Whether a childhood quest THREAD is
created at New Game is still open (see §4).

## 2. The childhood's cast and markers live in `bwsslums/defaultscenario` — PINNED

Named-entity registries (`<level>.save` is an XML `<Entity name="X">0xHASH</Entity>` table):

| scenario | total named entities | `QC010_*` names |
|---|---|---|
| `worlds/albion/bwsslums/chapter2slums` | 2109 | **2** |
| `worlds/albion/bwsslums/defaultscenario` | 1461 | **95** |

The 2 in `chapter2slums` are `QC010_ChildhoodStart` and `QC010_RoseDiaryDigSpot_Slums`. Everything
the quest script actually resolves by name — `QC010_Theresa`, `QC010_Rose`,
`QC010_RoseInCrowdMarker`, `QC010_HeroInCrowdMarker`, `QC010_MurgoCrowd`, `QC010_ShackTrigger`,
`FF_Door_Luciens`, … — is **absent from `chapter2slums` and present in `defaultscenario`**
(measured: `grep -c` over the extracted registries).

Of `defaultscenario`'s named entities, **264 carry a `SimpleTransformComponent` (0x619F96CF)** and
so have a decodable position; 98 of those are quest-prefixed. The 1197 without one are mostly
CREATURES — they are spawned by script, not statically placed, so they need the GDB archetype
path, not the marker path.

## 3. `Chapter2Slums` is coupled to the childhood's RESOLUTION — PINNED (but see the caveat)

`quests/gameflow.lua`, main chunk instr **672-714** (inside the SKIP-THROUGH path, gated on
`SkipToPositionInGameflow >= ScriptEnum.DebugQC140` at 665-668 and
`Gameflow.SkippingThroughGameflowFlag` at 673-675):

```
677 GETTABLE R2 := Layers['GetActiveScenarioForLevel']
678 LOADK    R3 := 'Albion'
679 LOADK    R4 := 'BWSSlums'
683 EQ       if (R2 EQ 'chapter2slums') ...        -- infers the morality branch FROM the scenario
687 SETTABLE R3['ChildhoodResolutionEvil'] := True
701 GETTABLE R2 := Layers['ActivateScenario']
704 LOADK    R5 := 'Chapter2Slums'                 -- evil branch
711 LOADK    R5 := 'Chapter2Posh'                  -- good branch
```

The gameflow *derives* `ChildhoodResolutionEvil` from whether the active BWSSlums scenario is
`chapter2slums`, and re-activates `Chapter2Slums`/`Chapter2Posh` for that branch. So
`Chapter2Slums` is the **post-childhood (chapter 2) state of the slums**, selected by the
childhood's good/evil outcome.

⚠ CAVEAT (do not overstate): this specific block is the debug **skip-ahead** path. It pins the
*association* between the scenario pair and the childhood's resolution. It does **not** by itself
prove what the normal (non-skipping) flow activates, and I have not pinned which scenario is
active DURING the childhood. The entity data in §2 is the strong evidence that the childhood is
authored on `defaultscenario`; treat "the normal flow leaves BWSSlums on its default scenario"
as ⚠ UNPINNED until a `Layers.ActivateScenario`/scenario-default trace confirms it.

## 4. What this means for the recreation plan

- The cooked stage the track has been using is **`chapter2slums`** — the level's post-childhood
  state. Its geometry is the same map, but its entity layer is the wrong era, and it is missing 93
  of the 95 `QC010_*` names the quest asks for. Any beat that resolves an entity by name cannot
  work on it.
- `Fable2Native/tools/cook_quest_markers.py` cooks `defaultscenario`'s named entities into a
  `.f2names` sidecar (264 records) and `NativeGame::load_named_entities` seeds them, so
  `GetEntityWithName` resolves. Verified end-to-end by `test_named_entity_sidecar`.
- ⚠ Seeding those markers did NOT move the census (77 natives reached, identical spin set), so
  named entities were **not** the only gate. What the childhood is actually parked on is under
  investigation — do not assume.
