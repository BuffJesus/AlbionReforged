# Fable II Modding Capability Map & Native-Hook Opportunities

What can be modded in Fable II on our recomp, split by **what it actually requires**. Grounded in the
reverse-engineering done so far (native catalog `ghidra_out/lua_natives5_catalog.tsv`, script
disassembly, and live testing) — not speculation. Companion to [MODDING.md](MODDING.md) (architecture)
and [MODDING_REFERENCE.md](MODDING_REFERENCE.md) (the Skyrim depth target).

## The two tiers (recap)

1. **Author layer — Lua / data / overlay. No engine rebuild.** The game's own Lua VM exposes ~3350
   natives (fully catalogued). Plus the recomp VFS loose-file override (`ModSupport.cpp`) shadows any
   asset, and `BnkWriter` injects per-entry into BNKs. A lot is possible here *today*.
2. **Native layer — hooks in the recomp runtime (`rexglue-src`). "SKSE tier."** For things Lua/data
   can't reach. **Because we own the engine, this is first-class for us** — not a blind offset-scrape
   like SKSE. One keystone hook (register custom natives) makes most of the rest Lua-accessible.

---

## Tier 1 — Works now (Lua / data / overlay)

| Capability | How | Status / evidence |
|---|---|---|
| **Gameplay logic / cheats** | Lua natives (god mode, XP, stats, spells, items, gold, renown, morph, spawn, time, weather) | ✅ Shipped: the R3/MMB mod menu, all calls verified from `DebugMenu.txt` |
| **Custom quests (real)** | Author a quest **module** `.lua` (`NewQuestThread`+`Init/Register/SetAsActive/Update`) and add it to `questsetupscript.lua`'s `RunScript` list (BNK edit); engine's update loop then ticks it | ⚙️ RE'd — no native hook needed; it's a data/BNK pipeline |
| **Custom quests (runtime hijack)** | Redirect the active quest's objective to your entity each frame: `QuestTracker.SetObjectiveEntity(h, GetPrimaryQuestName(h), ent, true)` | ✅ Working (breadcrumb confirmed) |
| **Interactions** | `OnActionUse` scripts, proximity + hold-A toaster (`GUI.DisplayInfoBoxParams{DBS_QUEST_ACCEPTANCE, IsHoldAButton}`), `Targeted.SetAsTargetable` highlight | ✅ Working sign interaction |
| **Cutscenes / cameras (in-engine)** | Fully Lua-scriptable: `PlayFullCutscene`, `SetInteractiveCutsceneRules`, `SetFixedCamera`/`SetLookAtCamera`/`SetCameraCircle`/`SetCameraShake`, `PlayAnimation` | ⚙️ API confirmed; authorable now |
| **Level placement (props/entities)** | AssetBrowser `LevelEdit` → `levels.bnk` (place model, mark as interactable prop entity, custom name, scale) | ✅ Editor working; edits journaled to durable **edit sets** + exportable as overlay mods (2026-07-18) |
| **Asset *replacement*** (textures, models, audio, Bink movies) | VFS loose-file override OR `BnkWriter` per-entry inject — drop a same-format file, it shadows the original | ⚙️ Scaffolded; needs author-side write paths (PNG→Lh etc.) |
| **Entity/enemy spawning** | `Debug.CreateEntityByHero`/`CreateBanditGroupAroundHero`/`CreateEntityAt` | ✅ In the mod menu |

**Tier-1 blockers that push things to Tier 2:** custom **text** (babel is hashed+compressed → unknown
tags render blank), custom **new** assets not replacing an existing file, clean **input** hotkeys, and
anything touching engine internals not exposed to Lua.

---

## Tier 2 — Native hooks ("what else could we hook?")

Ordered by leverage. The first one is the keystone.

### 0. ★ Lua↔native bridge — `register_native(name, fn)`  *(the keystone)*
Expose new C++ functions to the Lua VM. This is the SKSE "300+ new script functions" equivalent: with
one hook, **every** capability below becomes callable from ordinary Lua mods, and modders never touch
C++. Highest leverage by far — build this first and most other hooks become thin Lua-exposed shims.

### 1. `GetText` / localization override  *(custom text)*
Weak-override the text-tag resolver so our tags return custom strings. Fixes: quest objective headers,
hold-A box text, item/ability names, custom dialogue. This is the current **sign-text blocker**. High
value, contained.

### 2. Asset-load hooks  *(custom NEW content, modern formats)*
- **Texture load** → accept PNG / load new textures (custom images, UI art). AssetBrowser already
  decodes textures; pair with a PNG→Lh author path.
- **Model load** → glTF / new models & armours.
- **Audio load** → inject custom music/SFX (XMA2 or a modern codec).
- General **resource-load redirect** → a real modding VFS beyond loose files (load order, mod folders).

### 3. Input hooks  *(clean hotkeys / console)*
A native keyboard/gamepad hook to bind keys → Lua callbacks (the retail dev menu wants an event-queue
feed we don't provide; MMB was a workaround). Enables a real in-game console, hotkeyed mod actions,
and debug tools.

### 4. UI / HUD hooks  *(SkyUI / MCM equivalent)*
Native overlay rendering (ImGui-style) for custom menus, a mod-config menu, HUD widgets, debug draw.
Cleaner than fighting the game's `gameface` GUI VM (the pause-menu ceiling we hit).

### 5. Engine-limit removals  *(the "console limits" goal)*
RAM (512 MB), draw distance / entity caps, resolution/FPS (already touched `draw_resolution_scale`),
LOD. We own the runtime — lift the Xbox-360 ceilings.

### 6. Event hooks  *(mod logic triggers)*
Expose engine events to Lua: on-level-load, on-entity-death, on-interact, on-save/load, per-frame.
Lets mods react without polling. Pairs with #0.

### 7. Save/load hooks  *(mod-state persistence)*
Clean custom save data for mods (quest state, config) beyond the Pluto permanents-table dance.

### 8. Script-load interception  *(FSE-style, but native to us)*
Intercept the engine's script load to swap/extend native Lua scripts wholesale (FSE does this on
Fable:TLC via a `0xCDB355` DLL hook — not portable; we'd do the equivalent in `rexglue-src`). Enables
overriding native quest/AI scripts without editing BNKs. Mostly obviated by #0 + the BNK pipeline, but
useful for total-conversion scale.

### 9. Rendering / graphics hooks
Post-processing, custom shaders, render-target fixes (the adult-hero black-skin bug is an RTT→texture
handoff issue), lighting/weather overrides, ENB-equivalent.

### 10. Physics / animation hooks  *(Havok)*
Behavior-graph edits (FNIS/Nemesis equivalent), new anims, paired anims, custom movesets.

### 11. Networking hooks  *(multiplayer — Track D)*
Session/netcode exposure for co-op; recomp = same x86 on all clients → no cross-arch desync.

---

## Recommended sequence
1. **`register_native` bridge (#0)** — unlocks everything else as Lua-callable.
2. **`GetText` override (#1)** — immediate: fixes custom text (the sign header), item/ability names.
3. **Asset-load hooks (#2)** + author-side write paths — custom images/models/audio.
4. **Input + UI hooks (#3, #4)** — real console, mod-config menu, hotkeys.
5. **Limit removals (#5)** — the "no more console limits" goal.
6. Then event/save hooks, rendering, physics, networking as depth demands.

Everything Tier-1 keeps shipping in parallel (Lua mods, level editor, asset replacement) — the native
tier raises the ceiling, it isn't a prerequisite for most mods.

---

## System-by-system depth (the specifics)

| System | Fable II reality | Now (Lua/overlay) | Needs a hook |
|---|---|---|---|
| **UI** (menus/screens) | `gameface` ExpandableMenu, a SEPARATE Lua VM (`guiscripts.bnk`) | Edit menu population/logic in Lua; pause "Mod Menu" launcher now bridges GUI VM → host → gameplay VM (2026-07-18, `docs/MENU_SYSTEM_RE.md`). Native list integration still brittle | Native overlay (ImGui-style) = a clean SkyUI/MCM-class UI, no VM fighting |
| **HUD** | `HUD.lua`, quarterback buttons, minimap, world icons (`AddWorldIcon`) | Add/edit HUD elements + world icons in Lua now | Native overlay for rich custom HUD widgets |
| **Animations** | Anim banks + **Havok** behavior graphs; `PlayAnimation`, cutscene anims | Play existing anims from Lua; AssetBrowser decodes banks + Havok | Anim **inject** + behavior-graph edit (FNIS/Nemesis-class) = new/paired anims, movesets |
| **Meshes / models** | `.mdl` (dotXSI-derived); AssetBrowser → FBX/glTF | **Replace** existing models via overlay/inject (needs glTF→MDL write path) | Model-**load** hook = brand-new models/armours in glTF |
| **Textures / images** | `Lh`/DDS; AssetBrowser decodes → PNG/DDS | **Replace** existing textures via overlay (needs PNG→Lh write path) | Texture-**load** hook = new images / modern formats (custom UI art) |
| **Terrain / landscape** | Heightfields + terrain-texture registry + EHF env chunks; AssetBrowser reads heightfields/terrain/splat | Read + export to Blender now; small edits via level write | Robust heightmap **write** + LOD gen + AI-path (navmesh) = new lands |
| **Levels / world** | `levels.bnk` (TNG/LEV placement + GDB entities); AssetBrowser `LevelEdit` | Place/scale/name/mark-interactable props + save; ✅ edit-set journal + **Export Edits as Mod** → `mods/enabled/<name>/` overlay (2026-07-18, docs/MODDING_ENVIRONMENT.md) | (done — was: load-order overlay for packageable level edits) |
| **Quests** | Pure-Lua modules (`NewQuestThread` + `QuestTracker`), engine-loaded via `questsetupscript.lua` | **Real custom quests today** (author a module + add to the RunScript load list); or runtime hijack | Only for total-conversion scale (native script-host); text needs GetText override |
| **Combat / abilities** | `Stats.SetHeroAbilityLevel(hero, EHeroAbilityType.HERO_ABILITY_*, lvl)`; god-mode, damage, factions | Set/max abilities, god mode, spawn combatants, tune (all in the mod menu now) | Deep combat overhaul = native hooks on damage/AI dispatch |
| **Effects / particles** | Particle banks + decals + trails; `CreateParticleAtPosWithDirection`, `AddDecal`/`AddDecalToEntity`, `ReloadParticleBank`, `SSTrail*` (spell/weapon trails) | **Spawn/attach particles + decals + trails from Lua NOW**; retune trail params; `ReloadParticleBank` to swap a bank | New particle *definitions* = author a particle bank + asset-load hook; custom shaders |
| **Audio** | XMA2 + sound banks (`SoundTools.PlayMusic/PlayEvent`); AssetBrowser decodes XMA2 | Play existing music/SFX events from Lua; **replace** banks via overlay | Audio-**load** hook = new SFX/music in a modern codec |
| **Dialogue / expressions** | Dialogue + the "expression" system; `Debug.SetAllExpressionsAvailable`, expression FX | Trigger existing expressions/lines from Lua; grant all expressions | Dialogue-data decomp + editor; TTS pipeline for new voiced lines; needs GetText for new text |
| **NPCs / creatures** | Creature/NPC defs + skeletons/anims; `Debug.CreateEntityByHero('CreatureX', ...)` spawns any of ~100 types | **Spawn any existing creature/NPC now**; script AI/behaviour in Lua (behaviour scripts) | New creature *defs* (appearance/stats/AI) = def decomp + editor; new skeletons via model/anim hooks |

## ★ Spellmaking (the Morrowind/Oblivion dream)

Fable II's magic is the **Will** system, and it's a *fixed* set — the ability enum is concrete:
`HERO_ABILITY_WILL_FIREBALL / LIGHTNING / SLOW_TIME / SWORDS / VORTEX / CHAOS / FORCE_PUSH / DEAD_RISING`
(each 0–5 levels, set via `Stats.SetHeroAbilityLevel`). So unlike Morrowind, there's no built-in
"combine an effect + magnitude + area + duration into a new spell" screen. But that's exactly the kind
of thing our **native modding API makes moddable**:

- **Reskin/retune existing Will spells now (Lua):** grant/level them, tweak damage/cost/VFX bindings,
  script new *triggers* (e.g. a spell that also heals). Achievable today via the natives.
- **A custom freeform spell system (native API + Lua):** the Morrowind-style "spellmaker" becomes a
  **mod-authored system** on top of `register_native`: expose primitives (apply-damage-in-radius,
  apply-status, spawn-VFX, consume-mana) as natives, then a Lua layer + a UI lets you compose
  effect + magnitude + area + duration into a named custom spell, bound to a cast input. VFX/SFX reuse
  existing Will particles or new assets via the asset-load hook. This is the perfect showcase of the
  full stack (read → author → native-bridge → Lua logic) and is very much on the table once the
  native API lands.
- **New effect logic** the base engine can't do (chain lightning, summons, transmutation) = a native
  hook + Lua — the "Magic Effect + script" tier, which is *easy for us because we own the engine*.

So: retuned/scripted spells today; true Morrowind-style spellmaking as a native-API showcase. It
touches every layer, which is why MODDING_REFERENCE.md calls a custom spell the ultimate stress-test.

---
*Provenance:* quest/interaction/breadcrumb/text findings from `memory/fable2-quest-interactable-system.md`
and `fable2-babel-text-system.md`; native inventory from `ghidra_out/lua_natives5_catalog.tsv`
(~3350 natives) and `fable2-decomp-labeling-progress.md`; overlay/inject from `MODDING.md`.
# Native-port update

The capability map now applies to both the frozen oracle and the standalone PC runtime. The
public modding contract is implemented in `Fable2Native` with C++23 services, stable IDs, native
packages, scripts, events, menus, and save data; oracle hooks are transitional adapters only.

## ★ GDB is now READABLE — cutscenes, dialogue and archetypes (2026-08-19)

The game's authored data lives in `.gdb` files, which were previously a wall of 32-bit hashes.
Both of the file's own tables are now decoded, so records read as text — and *that* is the
prerequisite for authoring them.

| table | what it maps | why a modder cares |
|---|---|---|
| **name table** | `fnv1(name)` → record GUID | look a record up **by name**, exactly as the game's `GDB.GetRecord(name)` does |
| **string table** | `fnv1(s)` → `s` | resolves type-4 string VALUES *and* the **field names**, so records self-describe |

A record key is an editor-assigned opaque GUID, not a hash of the name — the name table is the
indirection. Layout + verification: `Fable2Native/include/f2/native_gdb.h`.

**Read any record:**

```
python Fable2Native/tools/gdb_record_dump.py <file.gdb> <RecordName> [--depth N] [--inherit]
python Fable2Native/tools/gdb_record_dump.py <file.gdb> --grep QC010_      # search record names
python Fable2Native/tools/gdb_entity_dump.py <level.save> <level.gdb> --grep QC010_
```

Which turns an interactive cutscene into its actual script:

```
QC010_JeevesGreet  (guid 0x8EB51907)
    UseCutsceneCamera          bool    = 0
    SceneElements              record
        SayLine                record
            Character              string  = 'QC010_EscortGuardFairfax'
            CharacterToTalkTo      string  = 'QC010_Jeeves'
            TextTag                string  = 'TEXT_QUEST_QC010_JEEVES_GREET_02'
```

So **cutscenes and dialogue are authored data**, not code: beats (`SayLine`, `SetEntityMode`),
their speakers, their animation groups, and their text tags. Writing is already possible —
`Fable2AssetBrowser/source/src/Level/GdbEdit.cpp` writes this same layout and round-trips.

**In-game, from Lua** (mods loaded via `NativeGame::load_mods` get these too):
`GDB.RecordExists(name)`, `GDB.GetRecord(name)` → a record handle with
`:GetID() / :GetFloat(field) / :GetInt(field) / :GetBool(field)`.
⚠ No string getter yet — that needs the string table on the C++ side.

## ★ Named entities and markers are a text sidecar a mod can override

`Fable2Native/tools/cook_quest_markers.py` cooks a level's named entities into a `.f2names`
sidecar — a plain tab-separated table (`name  x  y  z  yaw  kind`), deliberately human-readable
and diffable rather than a binary blob. It carries both kinds of `.save` registry entry: **markers**
(placed, position read from their transform component) and **declared entities** (a GDB record but
no position — script-placed, e.g. `QC010_Rose`).

`NativeGame::load_named_entities` is **override-by-name**: loading a second `.f2names` on top of
the cooked one MOVES an existing name rather than duplicating it, and adds names it has not seen.
So a mod ships a small file listing only what it changes. (Duplicating would be silently harmful:
`StartNewEntityThread` spawns one thread *per matching entity*, so a duplicated name would run a
quest branch twice.) Covered by `test_named_entity_sidecar`.

## ★ Mod / debug-jump menu — `ModMenu.Register` (2026-08-19)

`Fable2Native/include/f2/native_mod_menu.h` is the native replacement for the recomp's pause-menu
"Mod Menu". It is a **model** (a renderer draws whatever `entries()` reports), and it is built by
reflection over the game's own tables rather than a hardcoded list:

| source | what it yields |
|---|---|
| `Gameflow.ChildhoodVars.SkipTo*` | the game's **own** skip functions — `SkipToWino`, `SkipToLL`, `SkipToLL2`, `SkipToBuyMusicBox`, `SkipToLuciensStudy` |
| `Gameflow.DebugQuestStartTable` | every quest the gameflow registers as jumpable, with its level + start marker |
| `ModMenu.Register` | whatever mods add |

Measured on real game data: **72 entries** (5 skips + 65 jumpable quests + the shipped choices).
Because it is discovery, the list cannot drift from the data, and a skip runs the *game's* code —
setting real sub-quest flags, the gold counter, Rose's follow state, the level handoff — instead of
a state we guessed at.

**Add your own entry from a mod script** (loaded via `NativeGame::load_mods`):

```lua
ModMenu.Register("Mod", "Give 1000 gold", function()
  Money.Give(GetPlayerHero(), 1000)
end, "optional detail text")
```

Every action runs under `pcall` and a failure is **reported** (`ModMenu::last_error()`), never
swallowed — silent failure is what hid the childhood's frame-0 death for this entire project.

The childhood's "where do the warrants go" choice ships as two *registered* entries, so they double
as a worked template: they write `Gameflow.ChildhoodResolutionEvil` (which the gameflow uses to pick
the post-childhood scenario — `Chapter2Slums` evil vs `Chapter2Posh` good) and
`Gameflow.ChildhoodVars.WantedCompleted`. Those are the same two writes the recomp's menu made
behind an in-world sign, minus the HUD/toaster hack.
