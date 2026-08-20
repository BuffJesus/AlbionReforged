# ▶▶ CHILDHOOD LEVEL RECREATION — START HERE (handoff 2026-08-16)

Branch: **`agent/native-gameplay-core`** (Fable2Native gameplay track). Full decomp-backed gap
analysis with phased plan: **`docs/childhood_assessment_2026-08-16.json`** (workflow w5ty3r992, 8
agents, verified). Deep memory: `fable2native-stage2-control-camera-anim`,
`fable2native-gameplay-track`. Constraint (non-negotiable): **decomp-backed, NO guessing; every
stub FLAGGED**; both backends (D3D12+Vulkan) at parity; do NOT edit ENVIRONMENT-session files
(renderer, `cook_levels.py`, `native_scene.h/.cpp` render fields) — coordinate instead.

## Where we are — UPDATED 2026-08-19 (read this before the older sections below)
**The childhood quest RUNS.** Its coroutine is alive and it executes its real opening (fade → crowd
setup → the `PooCam` bird-poo cold open → music → fade in), with 23 entity threads live and
`PlayCutscene` now resolving its cutscene records. The 2026-08-16 verdict below ("~35%, the quest
plays into black-hole stubs, the sequence advances while nothing shows") was written before the
root cause was found: the quest was not sequencing silently, it was **dying on frame 0** every time
(§ROOT CAUSE). Sections below this one are kept for their evidence and method, but where they
conflict with this one, this one is current.

⚠ **Two corrections that invalidate parts of the older text:**
1. **The stage is the wrong scenario.** The childhood is authored on `bwsslums/defaultscenario`,
   NOT `chapter2slums` — different heightfield, `.genv`, engine_level, models/textures and entity
   gdb/save, per the game's own `.list` manifests. See `docs/CHILDHOOD_LEVEL_EVIDENCE.md`.
   Cooking `defaultscenario` is an ENV-session task with a fully specified input list.
2. **Named entities come in two kinds** (markers with a position, and declared entities with a GDB
   record but none), and both are needed — see §DECLARED entities.

Still READY as described: hero control/camera/locomotion/nav, quest sequencing (msg bus +
Quest→General→AI tick), scripted Physics/Navigation/Camera natives, hero anim clips resolved from
`globals.gdb` (Idle=id_4B706EF5, Walk=id_49220AA3, Run=id_4AB9BC89 on record 0x576283C7 —
`tools/gdb_anim_slots.py`). NPC locomotion is cook-ready but visible NPC animation is render-blocked
(`docs/NPC_LOCOMOTION_PLAN.md`).

**Current frontier:** the interactive-cutscene machinery. `PlayCutscene` now runs through its GDB
lookup into `StartCutscene`/`IsInteractiveCutsceneWaitingForMe` and fails there on
`attempt to call method 'GetID' (a nil value)` — an entity handle the ICS path expects.

## ▶▶ TEXT + DIALOGUE (2026-08-19) — `book.babel` CRACKED, cutscene beats speak

**The codec was plain zlib.** `data/language/<locale>/text/book.babel` = an index of
`{fnv1(TextTag), blockKey, offsetInBlock}` over 333 zlib blocks of UTF-16BE strings (each with a
`u32` code-unit length prefix). The game's dialogue, quest names and UI strings are all readable,
in the player's own language. Tool: `tools/babel_text.py` (decode / `--grep` the decoded text /
`--cook` a runtime package). Runtime: `native_text.h` + the `GetText(tag)` native; an unknown tag
returns the tag, as retail does.

**Cutscene beats now perform.** `NativeGame::perform_cutscene` walks a cutscene record's
`SceneElements` and speaks each `SayLine` (Character / CharacterToTalkTo / TextTag):

```
QC010_JeevesGreet -> 5 lines; first =
  QC010_EscortGuardFairfax: "Evening, Jeeves. Here are the children Lord Lucien asked for."
```

Lines land in `NativeGame::spoken_lines` for a subtitle renderer; the census has a DIALOGUE section.
⚠ FLAGGED: no timing, camera or animation — a beat is emitted instantly.

⚠ **Measured limit:** the childhood's OWN cutscenes still never reach the request stage (0 lines in
a live run). They park in `PlayCutscene`'s pre-start in-range wait, whose next dependencies the
census now names: `GroupMindManager.GetCutsceneGroupMind` (22800 calls) and
`IsDistanceBetweenThingsOver` (10791). Dialogue is proven to work when a cutscene performs; making
the childhood's cutscenes START is the next chain.

**Quest display names — use the game's chain, there is NO tag convention:**
`QuestTracker.Register(hero, questName, 'Quest_<Name>')` → a GDB record in `globals.gdb` →
`NameTag`/`DescriptionTag` → babel. e.g. `Quest_QC010_Childhood` → `TEXT_QUEST_QC010_NAME` →
**"Childhood"**. The mod menu uses exactly this.

## ▶▶ ROOT CAUSE FOUND + FIXED (2026-08-19): `Gameflow:Init()` ran on the INACTIVE branch

The childhood "started" yet nothing ever played because **`NativeGame::start_new_game` called
`Gameflow:Init()` BEFORE setting `Gameflow.GameflowMode`**, so Init took the game's own
debug/sandbox branch and never built the state the quest reads.

**Ground truth — the BNK ships the ORIGINAL LUA SOURCE of this file** as
`scripts/quests/gameflow.txt` (188 KB, `f2tool extract gamescripts_r.bnk "scripts\quests\gameflow.txt"`).
No disassembly guesswork needed:

```
2037  if QuestTracker.IsToStartGameflow(QuestManager.HeroEntity) then
2038      Gameflow.GameflowMode = true
2250  if not Gameflow.GameflowMode then
2251      print("Gameflow INACTIVE")        -- debug kit: dog, default weapons, potions
2289  else print("Gameflow ACTIVE")         -- the REAL new-game setup
2383      Gameflow.WeaponNames = {}
2384      Gameflow.WeaponNames.ChildMelee = "ChildSwordWooden"
```

QC010's `Update` dies on its FIRST frame at `Gameflow.WeaponNames.ChildMelee` (nil) — which is
exactly the census's `2x attempt to index field 'WeaponNames'` plus `Inventory.AddItemOfType`
listed as *referenced but never called*. The gameflow's `WaitForQuestToFinish` then sees a dead
coroutine, ends the quest and walks on to QC060 → QC070_Thag — which is why Thag was the only live
quest thread in every stall dump.

**Fix** (`native_game.cpp`): set `GameflowMode` before `Gameflow:Init()`, and report Init's error
via `Debug.Error` instead of swallowing it in a bare `pcall` — that swallow is what hid this.
⚠ FLAGGED: pre-setting the flag stands in for `QuestTracker.IsToStartGameflow` (line 2037), which
the port doesn't implement — the auto-stub answers `false` to any `Is*` predicate. Binding that
native properly is the durable follow-up.

**Measured after the fix:** `Gameflow:Init` branch = **ACTIVE** (was INACTIVE) · `WeaponNames` =
table (was nil) · natives the childhood reaches **77 → 99** · the timeline now extends to frame 49
with real beats (`TutorialManager.DisplayTutorial`, `GetBreadcrumbEntity`,
`Navigation.SetMovementPaused`, `GuildMessages.IsAvailable`).

**Regression guard:** `test_gameflow_starts_childhood` now asserts the game's own "Gameflow ACTIVE"
print (and absence of "INACTIVE") plus `Gameflow.WeaponNames.ChildMelee == 'ChildSwordWooden'`.
⚠ The old `"QC010_Childhood Starting"` assertion is WEAK and demoted to a precondition: the game
prints it after `:new()` but before the coroutine is ever resumed, so it passed throughout the
entire period the childhood was dying on frame 0.

### ▶ Follow-on fixes (2026-08-19, each measured then verified)
Chasing the childhood one death at a time. Every step: read the error, ground the contract in the
game's own source/usage, fix, re-measure.

1. **`Timing.GetDayCount`/`SetDayCount`/`AdvanceDayCount`** — `GameflowDayChecker:Update`
   (`gameflow.txt:1828`) samples the day count and *compares/subtracts* it, so the black-hole stub
   raised "attempt to compare two table values" and killed the checker. Only the DIFFERENCE is ever
   consumed, so the absolute value needs no retail grounding (⚠ flagged as chosen, not evidenced).
2. **`GetRandomNumber(n)` → integer in [1, n]** — `GameflowQuestUnlocker:Update`
   (`gameflow.txt:1666`) does `GetRandomNumber(100)` then `> 50`. The RANGE is pinned by the game's
   own idiom `GetRandomNumber(GetTableSize(t)+1)` with an explicit `== size+1` "none" sentinel
   (`gameflow.txt:304-306`), which only works for inclusive 1..n. ⚠ Retail's generator is not RE'd,
   so draws are not sequence-identical.
3. **`Entity:GetPosition()` returns a CVector3, not 3 scalars** — the game does
   `QuestManager.HeroEntity:GetPosition() + CVector3(0,0,24)` (qc010 `PooCam`), which only works if
   the left operand is a vector; and `gameflow.txt` passes positions straight into vector params
   (`Debug.CreateEntityAt("ObjectLimboInventory", "", CVector3(0,0,0))`). The port's 3-scalar
   assumption was documented in a comment and is now disproven. Added `push_vector3`/`arg_vector3`
   to the VM, a scalar `GetPositionXYZ` for the port's own use, and made `Debug.CreateEntityAt`
   take the vector form.
4. **Camera scripts are ENGINE-loaded** — `camera/camerasetupscript.lua` is a list of
   `AddCameraScriptFile(...)` calls and *nothing in the 552 shipped scripts runs it*; the
   hot-reload dispatcher routes a changed `camera/` file to the native `Debug.ReloadCameras`
   (generalsetupscript `main.proto[0]` instr 20-26). Implemented `AddCameraScriptFile` +
   run the setup script at boot, so `CameraFunctions` (which builds every scripted-cutscene camera
   cage) is real. ⚠ The engine's exact camera-boot entry point is not RE'd — this mirrors the
   observed mechanism.

**Measured effect (A/B harness, world + markers loaded):** silent coroutine errors **6 → 2**; both
`DummyObjects` failures gone; the childhood now runs past the crowd setup into its `PooCam`
cold-open.

### ▶▶ THE CHILDHOOD NOW RUNS (2026-08-19) — its thread is ALIVE and parked on a real wait
`Gameflow.Childhood ... co=suspended` (was: dead, then absent). The census timeline shows the
actual opening executing in order: `GUI.FadeScreenOut` + `LockScreenFade` → crowd setup →
`PooCam` (the bird-poo cold open) → `SoundTools.PlayMusicAndAtmosForLevel` → `GUI.UnlockScreenFade`
→ **`GUI.FadeScreenIn`** — then it parks at `QC010_Childhood :WaitFor`. Natives reached: **133**.

**What unblocked it: DECLARED entities.** The `.save` registry holds two kinds of name → GUID
entry, and the cook was only emitting one:
- **markers** — carry a transform-bearing component, position readable (264 in defaultscenario);
- **declared entities** — a GDB record but NO position, because they are placed by script. Their
  records live in **`globals.gdb`**, not the level GDB. Dumped with the new
  `tools/gdb_entity_dump.py`:
  `QC010_VillagerA` → GraphicAppearanceMorph + PhysicsSimulationCharacterNavigator ·
  `QC010_Rose` → PhysicsSimulationCharacterNavigator + AIBrain · `QC010_Theresa` → the same navigator.
  902 of them in defaultscenario. `cook_quest_markers.py` now emits both kinds (`kind` column).

⚠ FLAGGED: a declared entity is a **handle**, not a character — seeded at the origin (there is no
position to read; the script teleports it) with no mesh/AI/appearance. Making them visible still
needs the GDB archetype instantiation chain (`ghidra_out/gdb_instantiation_re.txt`).

**Naming correction, verified:** field hash `0x619F96CF` is FNV-1(`"PhysicsSimpleComponent"`)
*exactly* — `CECPhysicsSimple` in `ghidra_out/gdb_component_registry.txt`, typeId 2. The project
has been calling it "SimpleTransformComponent" (e.g. `npc_spawn_re.txt`, memory notes). The decoded
CHAIN is validated either way; only the name was wrong.

### ▶▶ THE GATE NOW: `PlayCutscene` never returns — MEASURED
With entity threads probed (they derive from `QuestEntityThreadBase`, not `QuestThreadBase` — the
earlier probe only wrapped the latter, which is why every entity-thread wait was invisible), the
census shows the childhood's beats calling `PlayCutscene` with real names —
`QC010_SetRoseMode`, `QC010_JeevesGreet`, `QC010_GuardMorning`, `QC055_MagpieIntoSleep` — and
**not one of them ever returns**. The main thread is parked in `WaitFor` on a predicate
(`qc010_childhood.lua main.proto[17].proto[0]`, linedefined 635) that reads
`ParentQuest.StartHeroPooScene`, which an entity thread only sets *after*
`PlayCutscene{Cutscene="QC010_SetRoseMode"}` returns.

So the pre-Phase-0 estimate ("135 PlayCutscene calls") turns out to be right — but it is now
*reached and confirmed by measurement* rather than counted by grep, which is the difference
between a guess and a diagnosis.

**The contract, from the decompiled `QuestEntityThreadBase.PlayCutscene`**
(`lua-decompiled/gamescripts/scripts/quests/questmanager.lua:2103`):
1. `MessageEvents.GetMostRecentMessageID()` (twice — brackets the messages the cutscene posts)
2. `opts.Entity = self.Entity`
3. **`GDB.RecordExists(name)` → `GDB.GetRecord(name)`** — a cutscene is a GDB RECORD; if it is
   missing the game itself calls `Debug.Error("Unable to find a cutscene called " .. name)`
4. `record:GetFloat("MaxRangeFromPlayer")`
5. loops on `self.ShouldCutsceneTerminate`

So the next implementable step is `GDB.RecordExists` / `GDB.GetRecord` (+ `:GetFloat`) over
`data/interactivecutscenes/interactivecutscenes.gdb`, then a minimal beat-runner that satisfies the
loop and returns.

⚠ **But PlayCutscene is NOT the only thing holding the main thread — measured.** The census has a
diagnostic arm, `FABLE2NATIVE_CENSUS_SKIP_CUTSCENES=1`, which makes `PlayCutscene` return
immediately (never on by default; it is not faithful — no camera, no dialogue, no timing, and any
state the cutscene should have changed is skipped — it exists only to see what lies beyond).
With it on, other threads advance (`QC010_JeevesGreet`, `QC010_GuardMorning`, `QC010_JeevesToStudy`,
`QC055_MagpieIntoSleep` are skipped in sequence).

⚠ **CORRECTION (measured after deepening the coroutine scan):** an earlier note here claimed the
Rose thread "never reaches its first beat". That was an artifact of the stall scan's depth limit —
entity threads live at `Gameflow.ChildThreads[N].ChildThreads[M]`, one level past where it stopped
looking, so they were simply invisible. With depth 9 and per-frame `linedefined` reporting, the
real picture is:

```
ChildThreads.12.co_update                    QC010_Childhood   yield <- f@764 <- WaitFor <- ?@401
ChildThreads.12.ChildThreads.1.custom_update state=20          yield <- f@1382 <- PlayCutscene <- ?@1411
```

`?@401` is QC010's `Update`, `?@1411` is `QC010_Rose.CustomUpdate`. So the **Rose thread is alive
and has advanced to state 20-21**, and 23 entity threads are live with real states.

⚠ Read stack frames carefully: `f@764` is **`questmanager.lua`'s `WaitFor` internal yield helper**
(`main.proto[50]`), NOT a quest predicate — an intermediate note here misread it as one and claimed
the main thread had moved on. It has not: the wait log records exactly ONE `WaitFor` entry for
`QC010_Childhood`, at frame 1, predicate `635` (`StartHeroPooScene`), with no matching RETURN even
after 100 simulated seconds. The predicate itself is held as an upvalue and is not visible on the
stack; only the entry log names it.

⚠ **OPEN, do not paper over:** the Rose thread's `CurrentState` is 20-21, yet the branch that sets
`ParentQuest.StartHeroPooScene` sits under `CurrentState == 0`, and the main thread's wait on that
flag has never fired. Either that branch was bypassed (a message-driven jump, or a different state
variable than the probe reads) or the flag is being set on a different table than the predicate
reads. That inconsistency is the next thing to measure — do not assume which.

**Scope datum:** with `SKIP_CUTSCENES=1` over 6000 frames (100 s), only **4 distinct cutscenes**
are ever requested (`QC010_JeevesGreet`, `QC010_GuardMorning`, `QC010_JeevesToStudy`,
`QC055_MagpieIntoSleep`). So removing the cutscene gate alone does NOT cascade the quest forward —
the remaining blocks are ordinary `WaitFor` predicates needing real world state, not one missing
system. `FABLE2NATIVE_CENSUS_FRAMES` sets the horizon.

### ✅ SOLVED (2026-08-19): how `GDB.GetRecord(name)` resolves a name — it is a TWO-STEP lookup
A GDB record key is an **editor-assigned opaque GUID**, not a hash of anything — which is why
hashing a name and searching the record table always fails (I tested 9 hash variants over all
33,886 script string constants: chance-level everywhere). The indirection is a separate **NAME
TABLE** at the end of each `.gdb`:

```
0x10        name_count                   (NOT equal to `count` in any shipped file)
hash_base   = schema_base + size_b       count * u32 record GUIDs (sorted)
offset_base = hash_base + count*4        count * u16
name_base   = align4(offset_base + count*2)
NAME TABLE  = name_count * { u32 fnv1(name), u32 recordGUID }, sorted ascending by hash
```

So: `guid = name_table[fnv1(name)]`, then the existing `lookup(guid)` binary search.

**Verified end to end, independently of the agents that proposed it:**
`fnv1("QC010_SetRoseMode")` → GUID `0x94FA26B1` → record `@0x2A318` → `MaxRangeFromPlayer` =
**10.0** — exactly the field `PlayCutscene` reads. `QC010_JeevesGreet` likewise. The pairs are
strictly ascending in every shipped file and every GUID column resolves as a record in the same
file; 47 of qc010's 122 `QC010_*` string constants resolve as cutscene records (the rest are
entity/marker/layer names, as expected).

⚠ Corrections this produced: `ghidra_out/gdb_entity_spec.txt` line 27 says `name_count (== count)`
— that is **wrong** in every real file (interactivecutscenes 14692/2886, globals 71847/9993).
And `Fable2AssetBrowser/source/src/Level/GdbEdit.cpp:79-167` already parsed this layout; the
gameplay track simply did not know.

Shipped: `GdbView.guid_for_name()` / `record_for_name()` in `Fable2Native/tools/gdb_anim_slots.py`
(the existing anim-slot path is unregressed — hero `Idle` still resolves to `id_4B706EF5`).

**DONE (commit b11c3e7):** `native_gdb.h/.cpp` ports the reader to C++ (`GdbFile` + `GdbDatabase`,
name table + kHashParent inheritance), `boot_game_scripts` opens `globals.gdb` +
`interactivecutscenes.gdb`, and the natives are registered: `GDB.RecordExists`, `GDB.GetRecord`
(returns a record handle), `rec:GetFloat/GetInt/GetBool`. `test_gdb_record_lookup` asserts the
chain both in C++ and through Lua, with GUID `0x94FA26B1` byte-exact and a negative control.

**Measured:** `"Unable to find a cutscene called ..."` no longer fires at all, and `PlayCutscene`
now runs THROUGH the lookup into `StartCutscene` / `IsInteractiveCutsceneWaitingForMe`
(`AIManager.GetRequestedCutsceneOnEntity` and `ClearCutsceneOnEntity` are now reached). Distinct
silent errors rose 5 → 12 because more code executes — the new ones are inside the cutscene
machinery (`attempt to call method 'GetID' (a nil value)`, an entity handle the ICS path expects).
That is the next gap.

⚠ FLAGGED: no string getter yet (needs the file's string table); level `.gdb` files are not opened
(add them BEFORE globals when a level cook provides one, so a level override wins).

### ▶ (history) The gate this replaced: spawned CREATURES — PROVEN BY A/B
With the real marker set the childhood dies at frame 0 right after
`GroupEvent.CreateCrowdControl("QC010_MurgoCrowd")`, on "attempt to index a nil value". The next
thing Update does is `GetEntityWithName("QC010_VillagerA")` / `"QC010_VillagerB"` and index the
result — and those are **exactly the two crowd entities in `defaultscenario.save` that carry no
`SimpleTransformComponent`** (the other 10 `QC010_Villager*` are static markers and cook fine).
They are creatures: spawned from a GDB archetype, not statically placed.

**A/B proof (not inference):** appending fake `QC010_VillagerA`/`B` rows to the `.f2names` sidecar
makes that death disappear and the quest advance into `PooCam`; removing them brings it back.

So the next real work is **creature instantiation from the GDB archetype** (decomp already done —
`ghidra_out/gdb_instantiation_re.txt`, memory `fable2-modding-systems-analysis`), wired into
`GetEntityWithName`/`Debug.CreateEntityAt`. That is the gate for the childhood's whole cast.

### ▶ REMAINING known errors (both "stub returns a table where a number is needed")
- `<?:239>` (`quests/qr_communityservice.lua main.proto[6]`) after `CommunityService.GetCurrentStage`
- `<?:4309>` after `Inventory.GetNumberOfItemsOfCategory`
Both need the native to return a REAL number; the stub deliberately refuses to fake ordering.

### ▶ THE OLD GATE (superseded, kept for the method)
The childhood still terminates — `Gameflow.Childhood` is nil by the end of the run and QC070_Thag
runs — but now on **four later silent coroutine errors**, which are the next work item:
1. `attempt to index global 'DummyObjects' (a nil value)` — an engine-provided ENUM table
   (`CHEST`, `HAND_LEFT`, `PROP_POINT`, …). `script_engine_globals.py` REJECTED it (0 catalog
   hits) because its members are constants, not natives, so the evidence gate could not see it.
   ⚠ Its VALUES matter (they are passed as arguments), so a black-hole stub is not good enough —
   this needs the real enum values from the exe. Genuine RE task.
2. `attempt to compare number with table` / `attempt to compare two table values` — a stubbed
   getter's black-hole value reaching a numeric comparison. The stub deliberately omits
   `__lt`/`__le`/`__eq` (faking ordering would corrupt real comparisons), so these name natives
   that must return REAL numbers.
3. `attempt to index a nil value` — unattributed; needs the same treatment as WeaponNames
   (find the producer in the shipped source).

### Phase 0 ✅ DONE (2026-08-19) — the census EXISTS, and it found a bigger blocker than text
Artifact: **`docs/childhood_stub_census.txt`**, regenerated by
`f2native_core_tests` → `test_childhood_stub_census` (boot + `load_quest_scripts` +
`start_new_game` + 600 frames of `QuestManager.Update()` + `ScriptSystems::tick`). It reports four
things, each measured: a **call-frequency ranking** (new `stub_call_counts()`, counted per
INVOCATION — `stub_misses()` only ever logged each name once, so it could not rank anything), a
**timeline** (first-call frame per native → where the sequence stops), the **spin set** (calls in
the last 60 frames → what the stalled quest is polling), and **silent coroutine errors**.

**The finding that changes the plan:** the childhood was not "silently sequencing" — it was
**DEAD two frames in**. The script managers resume with `coroutine.resume` and discard the
`(false, err)` result, so the failure was invisible. The error: `attempt to index global
'AmbientPopulationManager' (a nil value)`. The auto-stub only fabricates a stub for names it
believes are natives (`__is_native`), and the Ghidra catalog left many classes' names unresolved
(their rows carry class `?`), so those globals read as `nil` and killed the coroutine on contact.

**Fix (data-backed, not hand-listed):** `Fable2Native/tools/script_engine_globals.py` derives the
engine-provided class set from the game's own bytecode — every Capitalized global that some script
indexes as a table (`GETGLOBAL` → `GETTABLE`/`SELF`) but that **no** script ever `SETGLOBAL`s must
come from the engine — and then requires independent EXE-side corroboration (≥3 of its methods, and
≥50% of them, appear as bound natives in `lua_natives5_catalog.tsv`). **121 classes** accepted
(`Dog`, `Camera`, `Navigation`, `Action`, `Trigger`, `SoundTools`, `AmbientPopulationManager`, …);
the 349 rejected are script-defined quest/behaviour classes and are deliberately left alone —
stubbing a real game table would turn its nil-field reads into stubs and break its own logic.
Report: `docs/script_engine_globals.txt`; the list is wired into `kDerivedEngineClassNames`.

**Measured before → after:** silent coroutine deaths 3 → 1; distinct natives the childhood reaches
45 → **73**; `Gameflow.co_update` **dead → suspended**, now with 6 live `ChildThreads`; and the
startup runs all the way through layer/age/script-rule setup to `SoundTools.PlayMusic`.

**The spin set (what the quest is now parked on):** `IsLevelLoaded()` 32/frame ·
`SearchTools.FilterWithEC` + `Villager.GetECType` 10/frame each (the population scan) ·
`GetPlayerHenchman()` 2/frame. Everything else has gone quiet, so the remaining beats are gated on
**world state**, not on more class stubs.
- ⚠ Harness caveat (stated in the file): this run loads **no scene**, so entity searches come back
  empty and world-gated beats can't advance — the ranking is a LOWER BOUND weighted to the opening.
  Re-running the census with a cooked `chapter2slums` scene prepared is the cheap next measurement.
- ⚠ Residual silent error: `attempt to index field 'WeaponNames'`. `Gameflow.WeaponNames` is set by
  a gameflow init routine our sequence hasn't reached (disasm `gameflow.lua` ~1353) — an ORDERING
  artifact, not a missing native.
- The pre-Phase-0 estimate (135 `PlayCutscene`, heavy `GUI.Display*Box`/`GetText`) is a *script
  grep*, and none of it appears in the measured run yet — those beats live past the current gate.

### ⚠ Phase 0 follow-up: THE STAGE IS THE WRONG SCENARIO — see `docs/CHILDHOOD_LEVEL_EVIDENCE.md`
Pinned from the game's own bytecode + level data: `gameflow.lua` instr 274-281 registers the
childhood as `RegisterDebugQuest(DebugQC010, 'QC010_Childhood', 'Childhood', 'BWSSlums',
'QC010_ChildhoodStart')`, and the quest's cast/markers live in **`bwsslums/defaultscenario`
(95 `QC010_*` names)**, not in **`chapter2slums` (2)** — which the gameflow couples to the
childhood's good/evil RESOLUTION (instr 672-714), i.e. the POST-childhood state of the same map.
The cooked stage this track has been building on is `chapter2slums`. Same geometry, wrong era,
missing 93 of the 95 names the quest resolves.
- **Shipped:** `Fable2Native/tools/cook_quest_markers.py` cooks a level's named entities into a
  `.f2names` sidecar (264 records for defaultscenario, 98 quest-prefixed) — a SIDECAR on purpose,
  so the ENV-owned F2SCENE schema needs no negotiation. `NativeGame::load_named_entities` seeds
  them so `GetEntityWithName` resolves; proven by `test_named_entity_sidecar`.
- **Measured, and important:** seeding the markers did NOT move the census (still 77 natives, same
  spin set). Named entities were **not** the only gate — do not assume text or cutscenes are next
  either. The census now takes `FABLE2NATIVE_CENSUS_SCENE` + `FABLE2NATIVE_CENSUS_NAMES` so this
  stays measurable.
- ⚠ WITHDRAWN: an intermediate reading of this session claimed QC010 "is never instantiated"
  because the live-VM probe found it in `Gameflow.DebugQuestStartTable` with no coroutine. That
  table is written by `RegisterDebugQuest` (instr 274-281) — it is a REGISTRATION, so the probe
  proves nothing about instantiation. Where the childhood thread actually parks is still OPEN.

### Phase 1 — TEXT IS VISIBLE (highest leverage, fully in-track, decomp DONE) ★ recommended first build
Goal: every childhood text beat renders — hold-A prompts, quest questions, narration, subtitles,
gold/warrant counters. Converts "silent sequencing" into a readable opening + unblocks cutscene
subtitles later.
- **`GetText(tag)` weak-override** feeding **cooked UTF-16 strings** (native_bindings/native_script,
  pure gameplay-track). Decomp: `GetText = sub_822F3D08`, FNV-1 tag hash, red-black tree
  (memory `fable2-babel-text-system`). ⚠FLAGGED: `book.babel` bulk-text compression codec is
  UNKNOWN → strings must be COOKED/overridden, not parsed from file, until the codec is cracked.
- **Real `GUI` native class**: `DisplayInfoBox`/`DisplayInfoBoxParams` (DBS_QUEST_ACCEPTANCE hold-A,
  round-trips through `MessageEvents.IsMessagePosted`), `RemoveDisplayBox`, `SetCounter`/
  `RemoveCounter`, `SaySimLine`, `SetNarratorTag`.
- **Native text-box + counter renderer** on the existing ImGui-free `NativeUiRenderer` (both
  backends — this is the one Phase-1 piece that touches the frontend UI stack; keep it symmetric).
- Test headlessly: boot childhood, assert the display-box queue receives the right tags + the hold-A
  round-trip posts the acceptance message.

### Phases 2–6 (grounded, later; env-session deps flagged)
- **P2 named entities/markers + triggers** — decode the 56+ `QC010_*` markers/entities from
  `chapter2slums.save` via the validated `0x619F96CF` SimpleTransformComponent→Position chain
  (`npc_spawn_re.txt`); back `GetEntityWithName`/`GetPositionOfEntity`. ⚠HARD-BLOCKED on ENV: F2SCENE
  + `native_scene.h` have NO named-entity/marker/volume field — agree the schema with the env session
  first. Trigger volumes = distance-radius approximation (on-disk box/sphere EXTENT record un-RE'd,
  FLAGGED); but the quest uses real `IsSpecificTriggerEntityInsideTriggerVolume` membership, so flag
  the approximation.
- **P3 cutscene staging** — make `CameraManager.SetCameraOverride` real (evaluate PositionFunction/
  FocusFunction cages per frame + gate follow-cam), an `Action`/PLAY_ANIMATION native (play a named
  clip on hero/entity from the cooked anim package), a bespoke **ICS beat-runner** (`cutscene_ics_system.txt`).
  ⚠SCOPE: 135 PlayCutscene / ~129 distinct assets (bigger than "bounded"); IC asset CONTENT lives in
  cutscene/anim banks, referenced by name only → FLAGGED until decoded.
- **P4 Rose follows + GDB entity resolution** — `Follow.FollowEntity`/`StopFollowing` +
  `Navigation.MoveToEntity` (goal = hero pos each tick onto the READY nav motor; `npc_ai_brain_system.txt`
  Follow@0x825E33D0); fix non-hero velocity readback; `Debug.CreateEntityAt` class-name→cooked
  mesh+components (`gdb_instantiation_re.txt`). ⚠ENV: animated NPC character-mesh cook (current NPC
  cook is static bind-pose) + both-backends consumption of skinned NPC instances.
- **P5 layers + streaming + castle handoff** — cook per-entity load-state + `Layers.Activate/Deactivate`
  toggling instance visibility; couple `LoadLevel`→`NativeGame::load_scene`; cook FairfaxCastleGardens;
  fall `.bik` via the native video path + `Age.SetAgeGroup(ADULT)`. ⚠ENV: second-level + layer cook.
- **P6 sub-quest gold loop + endings** — Inventory/Money/QuestTracker real; 6 gold sub-quests +
  counter; music-box purchase; wish; sleep→morning; good/evil branch. ⚠ORDERING: main quest reads
  `Gameflow.ChildhoodVars.*Completed` flags, so sub-quests aren't cleanly "last"; and sub-quest
  INTERNAL minigames (QC040 Rex-fight, QC050 warrant stealth, QC055 booze) not exhaustively traced.

### Systems the first-pass plan MISSED (fold into phases; verifier-confirmed)
Music/SFX (`SoundTools.PlayMusic/PlaySound`, load-bearing) · screen **fade** (`GUI.FadeScreen*`, every
transition + sleep/wake) · **Breadcrumb** objective trail (`Breadcrumber.*`) · **time-of-day** +
sleep→morning (`Timing.SetTimeOfDay/SetTimeAsStopped`) · **26 particle FX cues** (incl. the
`FX_Bird_Poo_Splat` cold-open).

## Track boundary (who owns what)
- **This track (gameplay) CAN do:** all runtime natives (GUI/GetText/Action/Follow/ICS/CameraManager/
  LoadLevel), the `.save` marker/entity DECODE (a cook script under `Fable2Native/tools/`), runtime
  wiring, tests. Files: `native_bindings.cpp`, `native_script.cpp`, `native_game.cpp`, `native_world.*`,
  `Fable2Native/tools/*`.
- **ENV session owns (coordinate):** F2SCENE schema + `native_scene.h/.cpp` render fields,
  `cook_levels.py`, both world renderers + shaders, the NativeUiRenderer internals, animated
  NPC/character-mesh cook, second-level cook.

## Resume commands
```
cd D:/Documents/Fable2RE ; git branch --show-current   # agent/native-gameplay-core
# build + test:
cmake --build Fable2Native/build --target f2native_core_tests --config RelWithDebInfo
Fable2Native/build/RelWithDebInfo/f2native_core_tests.exe            # EXIT=0 green
# re-cook hero anim (GDB-resolved) if game data changed:
python Fable2Native/tools/cook_hero_anim.py Fable2Recomp/assets/game -o Fable2Recomp/assets/game/cooked/hero.heroanim
# resolve/list anim slots from globals.gdb:
python Fable2Native/tools/gdb_anim_slots.py Fable2Recomp/assets/game/data/Globals/globals.gdb resolve 576283C7 Idle Walk Run
python Fable2Native/tools/gdb_anim_slots.py Fable2Recomp/assets/game/data/Globals/globals.gdb list
# screenshot harness (both backends): scratchpad/shot_hero.ps1 (--backend vulkan), --auto-walk + --start-world --gameplay --hero-anim
```

## Session commits (2026-08-16, newest first)
037c25b New Game→gameflow reaches childhood (Stage 1) · 81be168 offline script cooker + open_cooked ·
3a8e726/74711a5/6f9bfc6/57dfa62 Stage 2 slices (Physics+CVector3 / Navigation / live AnimationPlayer
+velocity fix / camera override) · ac77ff8/baae536 Stage 3 (cook+load AnimClips bit-exact / both-backends
skinned pose forward) · c6fde46 fix frozen-frame (set_clip reset) · 4ff4ba5 ground speed to root motion ·
bee3f4a/420e1fe/be7d086 walk/run clip identification (events) — SUPERSEDED by · aa03983/d5952cf GDB
resolution (definitive, from globals.gdb) · 4d6799d NPC cook (multi-part) + render plan.
