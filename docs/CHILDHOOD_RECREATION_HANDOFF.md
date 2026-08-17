# ▶▶ CHILDHOOD LEVEL RECREATION — START HERE (handoff 2026-08-16)

Branch: **`agent/native-gameplay-core`** (Fable2Native gameplay track). Full decomp-backed gap
analysis with phased plan: **`docs/childhood_assessment_2026-08-16.json`** (workflow w5ty3r992, 8
agents, verified). Deep memory: `fable2native-stage2-control-camera-anim`,
`fable2native-gameplay-track`. Constraint (non-negotiable): **decomp-backed, NO guessing; every
stub FLAGGED**; both backends (D3D12+Vulkan) at parity; do NOT edit ENVIRONMENT-session files
(renderer, `cook_levels.py`, `native_scene.h/.cpp` render fields) — coordinate instead.

## Where we are (verdict: ~35% — STAGE ready, EXPERIENCE not)
The child hero spawns in a fully-lit, walkable **Bowerstone Slums** and is controllable + walk/run
**skeletally animated** on both backends; the gameflow reaches + runs **QC010_Childhood** (test
`test_gameflow_starts_childhood`). But the quest plays into **black-hole stubs** — the sequence
advances while nothing shows. DONE this session (Stages 1–3 + data-backed locomotion): see the git
log below.

READY: stage render (chapter2slums), hero control/camera/locomotion/nav, quest sequencing (msg bus
+ Quest→General→AI tick), scripted Physics/Navigation/Camera natives, hero anim clips resolved
**from `globals.gdb`** (Idle=id_4B706EF5, Walk=id_49220AA3, Run=id_4AB9BC89 on hero anim-set record
0x576283C7 — see `tools/gdb_anim_slots.py`). NPC locomotion is **cook-ready** (`cook_hero_anim.py
--models …`) but visible NPC animation is render-blocked (see `docs/NPC_LOCOMOTION_PLAN.md`).

## ▶ DO NEXT (in priority order)

### Phase 0 (do FIRST, ~30 min, empirical): capture the real stub-miss list
Boot the childhood with scripts and dump which MISSING/STUBBED natives QC010 actually hits most —
this drives the exact ordering. `native_script.cpp` has `__stub_log`/`stub_misses()`.
- Run `f2native_frontend.exe --scene <chapter2slums.f2scene> --start-world --game-scripts
  --game-dir <game>` (or extend a headless test to `boot_game_scripts` + `load_quest_scripts` +
  `start_new_game` + tick N frames), then read `stub_misses()`. Rank by frequency.
- The verifier already measured the hot ones from the script: **135 `PlayCutscene`**, heavy
  `GUI.Display*Box`/`GetText`, `SoundTools.Play*` (music/SFX), `GUI.FadeScreen*`, `Breadcrumber.*`,
  `Timing.SetTimeOfDay`, 26 particle `FX_*` cues. Confirm empirically, then start Phase 1.

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
