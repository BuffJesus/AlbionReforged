# NPC locomotion — data-backed cook + the render-channel dependency

Status 2026-08-16. This is the plan for folding NPC skeletal locomotion (walk/run/idle) into
Fable2Native, on the same data-backed footing as the hero. Two halves: the **cook/data** side is
DONE and general; the **visible render** side is a cross-track dependency on the ENVIRONMENT
session (which owns `cook_levels.py` + the world renderers).

## 1. How clips are resolved (data-backed, no guessing)

Cracked + byte-verified by an ultracode workflow (see memory `fable2native-stage2-control-camera-anim`):

- `locomotion.loco` carries **no** bank keys — it's a name-driven blend/state graph.
- The binding lives in **`globals.gdb`**: a locomotion slot NAME (`Idle`/`Walk`/`Run`/`Jog`/…) is
  `FNV-1` hashed (basis `0x811C9DC5`, prime `0x01000193`, mult-then-xor) to a GDB **field-name**
  hash on a creature's **animation-set record**; that field is type-4 and its raw u32 value **is**
  the bank clip `key0`. Fields inherit up the `kHashParent` (`0x5F6317D5`, type-6) chain.
- Resolver: `Fable2Native/tools/gdb_anim_slots.py` (faithful reimpl of the AssetBrowser
  `AnimBank.cpp` GdbMiniView + `scan_gdb_animation_fields`).
  - `gdb_anim_slots.py <globals.gdb> resolve 576283C7 Idle Walk Run` → the hero clips.
  - `gdb_anim_slots.py <globals.gdb> list` → all **52** character anim-set records + their clips.
- Hero-human anim set = GDB record **`0x576283C7`**: Idle `id_4B706EF5`, Walk `id_49220AA3`, Run
  `id_4AB9BC89`. Child rigs have no walk/run gait of their own → they reuse these clips retargeted.

## 2. Cook side — DONE + general

`Fable2Native/tools/cook_hero_anim.py` now:
- resolves clips through the GDB (`--anim-record`, `--slots`), no hardcoded hashes;
- accepts a multi-part model set (`--models head torso legs`) sharing one rig, and concatenates
  their skinned geoms into one package;
- bakes each clip **in-place** (the retail hero locomotion clips carry forward ROOT MOTION on
  anim-Y; it is dropped from the pose and consumed as controller world-motion) and records each
  clip's measured root speed (walk 1.555 / run 4.595 wu/s for the hero).

Cook any character:
```
# hero (default)
cook_hero_anim.py <game> -o cooked/hero.heroanim
# a villager child part-set (child rig reuses the hero anim set 0x576283C7)
cook_hero_anim.py <game> -o cooked/villagerchild_m.npcanim --models \
  "Art\Characters\Npc\Random Villagers\Children\dotXSI\CH_mchild_head_01\CH_mchild_head_01.mdl" \
  "...\CH_mchild_torso_01\CH_mchild_torso_01.mdl" "...\CH_mchild_legs_01\CH_mchild_legs_01.mdl"
```
Verified: the villager-child part-set cooks (66-bone child rig, hero clips retargeted, 5 geoms).
Per-NPC packages are user-local + gitignored (`cooked/`), like the hero package.

Open cook item: naming the 52 anim-set records to characters (they're keyed by opaque record hash,
not `fnv1(CreatureName)`). Grouping by rig (`key1` of the walk clip) → the `.loco` character export
(`rpt_HeroMale`/`rpt_Rose`/…) is the likely bridge; not needed for child NPCs (they share
`0x576283C7`).

## 3. Render side — the ENVIRONMENT-session dependency (blocks *visible* NPC anim)

The hero is animated because the renderer draws its mesh as a **character mesh** the app skins each
frame via `set_character_pose` (meshes named `hero*` → `character_meshes_`). **NPCs are cooked as
baked static instances** (`cook_levels.py` NPC part-set → ordinary `NativeInstance`s), so their
motion — and any skinned pose — is invisible (the flagged dynamic-instance foundation gap).

To make NPC locomotion visible, the environment session needs to:
1. **Cook NPC meshes as character meshes** — emit the placed NPC part-set into the F2SCENE as
   character meshes (a `character`/`npc` tag or a name prefix the renderer treats like `hero*`),
   one draw group per NPC instance, exposing `character_mesh_count()`/`_vertex_count(i)` for them.
2. Keep the baked instance as the fallback when no pose is supplied (so nothing regresses).

No renderer *code* change is required beyond recognising the extra character meshes —
`set_character_pose` already re-uploads per-mesh vertices on both backends.

## 4. Runtime side — my next step once (3) lands

Mirror the hero path per NPC (the `NpcAgent` substrate from Stage 2 already exists):
- Load each NPC's `.npcanim` (reuse `native_hero_anim.h`), give each `NpcAgent` an `AnimationPlayer`
  + bind verts + locomotion clips.
- Drive it each InWorld tick from the agent's `last_speed` (already tracked) via
  `select_locomotion_clip` (same as the hero).
- In both apps' `update_character_controller`, after the hero, loop the NPC agents:
  `compute_hero_pose(agent.anim, agent.bind, agent.facing - baked_yaw)` →
  `set_character_pose(npc_mesh_index, pose)` — the exact shared path, so it can't drift between
  backends.

This is inert until (3) provides NPC character meshes; the data (packages) is ready now.
