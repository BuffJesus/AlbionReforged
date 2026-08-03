# Fable II Modding — Master Plan

The single authoritative plan for turning the Fable II recomp into a thorough, Creation-Kit-class
**and** SKSE-class modding platform. Ties together the vision (MODDING.md), the depth target
(MODDING_REFERENCE.md), the per-system analysis (MODDING_CAPABILITIES.md), and the native API design
(NATIVE_MODDING_API.md). Read this first; the others are the detail.

---

## 1. Executive summary

Fable II modding has **two pillars**, and we can build both because we control a recompilation:
1. **An authoring tool** (Creation-Kit-class) — grown from Fable2AssetBrowser: read/edit models,
   textures, anims, levels, terrain, quests, and package mods.
2. **A native modding API** (SKSE-class) — hooks in our recomp runtime exposing engine internals to
   Lua. Because we *own the engine source* (not a closed binary), this is first-class, not
   offset-scraping — our ceiling is **higher than Skyrim's**.

A large amount is already possible with **Lua + the VFS overlay, no engine rebuild** (quests,
interactions, cutscenes, cheats, particles, spawning, asset *replacement*). The native API raises the
ceiling to **custom text, custom new assets, custom UI/HUD, freeform spellmaking, and limit removals**.

---

## 2. Ownership strategy — "make it our own" (recomp → decomp → our engine)

ReXGlue's recompilation is a **bootstrap**: a static PPC→C++ translation that runs the game now.
Making the platform genuinely ours is a three-stage migration, done incrementally, never blocking:

1. **Run it — recomp (ReXGlue), now.** The game runs; modding = hooks into translated guest functions,
   tied to guest addresses. Works, but it's "renting" the translation.
2. **Understand + own it — decompilation (Track B), the real work.** Reverse-engineer and rewrite
   subsystems as clean, understood C++, **verified against the recomp as a behavioral oracle** (it
   tells us the correct output for any input). Each decompiled subsystem — Lua binding layer, quest
   system, text/localization, asset loaders, the Will/magic system — becomes *our code*, not a blob.
3. **Expose it — the native API graduates.** `register_native`, `GetText`, asset hooks begin as recomp
   hooks (work today) and, as each underlying subsystem is decompiled, become **native features of our
   own engine code**. The API surface (what modders call) stays stable; the implementation moves from
   "hook the translation" to "our engine."

End state: the decompiled subsystems + our runtime shims (`rexglue-src`, which we already patch:
kernel, memory, GPU) stand as an engine we own, with ReXGlue reduced to the initial translation of
whatever we haven't decompiled yet. **The modding work is the forcing function** — every mod feature
we want (custom text, quests, assets) pulls the corresponding subsystem into "understood + owned."

---

## 3. Current state (works now — evidence-backed)

- **Lua modding pipeline (mature):** `tools/lua_mod` injects Lua into `gamescripts_r.bnk`; ~3350
  natives catalogued (`ghidra_out/lua_natives5_catalog.tsv`); a full in-game **mod menu** (cheats,
  spawning, warps, morph, quests) shipped.
- **Quests:** real custom quests via the engine load path (author a module + add to
  `questsetupscript.lua`), or runtime hijack (breadcrumb working).
- **Interactions/cutscenes/particles:** Lua-scriptable now (`OnActionUse`, hold-A toasters, camera API,
  `CreateParticleAtPosWithDirection`).
- **Overlay/inject:** VFS loose-file override (`ModSupport.cpp`) + `BnkWriter` per-entry inject.
- **AssetBrowser (read + early author):** decodes models/textures/anims/levels/terrain/Lua/audio;
  interactive move/rotate/delete of level instances; headless placement; **Level Editor backend**
  (named/removable placements) in progress.
- **Blockers → native tier:** custom text (babel hashed/compressed), custom *new* assets, clean input,
  rich UI.

See MODDING_CAPABILITIES.md for the full per-system table (UI, HUD, animations, meshes, textures,
terrain, levels, quests, combat, spellmaking, effects/particles, audio, dialogue, NPCs).

---

## 4. The native modding API — full hook catalog (sequenced)

Design + mechanism in NATIVE_MODDING_API.md. Ordered by leverage:

0. **`register_native(name, fn)` — the keystone.** Expose host C++ to Lua. Mechanism RE-confirmed:
   host `PPCFunc` + `FunctionDispatcher::SetFunction(addr, fn)` (reserved in-range address pool) +
   bind via `rex_lhRegisterAppBindings` hook + `lua_pushcfunction`/`setfield`. Once this exists, every
   hook below is exposed to modders as ordinary Lua. **Skeleton: `Fable2Recomp/src/ModApi.cpp`.**
1. **`GetText` override — custom text.** Fixes quest headers, box text, item/ability names. The current
   sign-text blocker. Weak-override the text-tag resolver.
2. **Asset-load hooks — custom new content + modern formats.** Texture (PNG/new images), model (glTF),
   audio; a real mod VFS with load order beyond loose files.
3. **Input hooks — hotkeys + a real console.** Keyboard/pad → Lua callbacks (the retail dev menu wants
   an event feed we don't provide; MMB was a workaround).
4. **UI/HUD overlay — SkyUI/MCM class.** Native ImGui-style overlay for a mod-config menu + HUD widgets
   (cleaner than the gameface GUI VM we hit ceilings in).
5. **Engine-limit removals.** RAM (512 MB), draw distance, entity caps, resolution/FPS, LOD.
6. **Event hooks** (on-load/death/interact/save/frame), **save hooks** (mod state), **rendering hooks**
   (shaders, the black-skin RTT fix, weather/lighting), **physics/anim hooks** (Havok behavior graphs),
   **networking** (co-op — recomp = same x86 on all clients, no cross-arch desync).

---

## 5. The authoring tool (AssetBrowser → Creation Kit)

Read layer is strong; the work is the **write/author** paths. Priorities:
- **Level Editor** (in progress): pick model, place/move/scale/rotate, name, mark interactable /
  quest-target, list + delete placements, save to `levels.bnk` — as a reproducible edit set.
- **Asset write paths:** glTF→MDL, PNG→Lh/DDS, audio pack — to author new/replacement assets.
- **Terrain write:** heightmap edit + LOD (reads exist).
- **Quest/interaction authoring:** a structured/visual editor emitting Lua quest modules + `OnActionUse`
  + placements + breadcrumbs (the "node tool" idea) — grounded in the quest RE.
- **Def editors:** item/appearance, creature/NPC, spell/ability (needs subsystem decomp).

## 6. Mod format + manager
Over the VFS overlay: a mod folder format (assets + Lua + level-edit sets + a manifest), **load order**,
and a manager (Mod-Organizer/Vortex class). Makes edits packageable, conflict-resolvable, and
non-destructive (also fixes the "in-place bnk mutation" / stray-bake problem by design).

---

## 7. Phased roadmap (with dependencies)

- **Phase 0 — `register_native` bridge.** Pin Lua C-API addresses (disasm `rex_lhRegisterAppBindings`),
  implement `ModApi.cpp`, verify the reserved-address pool, build + test. *Unlocks everything native.*
- **Phase 1 — `GetText` override.** Custom text (fixes the sign header). Exposed via `ModText_Set`.
- **Phase 2 — Level Editor + asset write paths.** Visual placement + PNG/glTF write → custom
  images/models + non-destructive level edits.
- **Phase 3 — Input + UI/HUD overlay.** Real console, hotkeys, mod-config menu.
- **Phase 4 — Limit removals + mod format/manager.** Lift Xbox-360 ceilings; packageable mods + load order.
- **Phase 5 — Deep systems.** Spell/ability + item/appearance + creature defs (needs targeted decomp);
  freeform **spellmaking** as the showcase; terrain/new-lands; dialogue/TTS.
- **Phase 6 — Multiplayer** (Track D) once netcode is decompiled.

Phases 0–1 are the highest-leverage and mostly RE-complete; 2 ships in parallel via the AssetBrowser;
3–6 build depth. Track B (decomp) runs continuously underneath, converting hooks into owned code.

---

## 8. Risks / open unknowns
- **Reserved-address pool** for `register_native` must be verified safe/in-range (a bad pick silently
  shadows a real function). Mitigate: pick from confirmed padding; assert on collision.
- **Lua C-API addresses** not yet pinned — disasm `rex_lhRegisterAppBindings` (highest-confidence source).
- **GetText resolver** location — follow the GUI text-queue consumer.
- **Def formats** (item/creature/spell) not yet decompiled — required for record-level authoring.
- **Recomp still parked** at the deeper boot frontier for full playthroughs (see HANDOFF); modding
  tooling advances regardless (the AssetBrowser + Lua pipeline don't need a fully-booting game).

## 9. Immediate next actions
1. Disassemble `rex_lhRegisterAppBindings` → pin the Lua C-API guest addresses (Phase 0 unblock).
2. Verify a safe reserved-address pool in the exe range; finish `ModApi.cpp` marshalling.
3. Ship the Level Editor UI (Phase 2, parallel, testable now).
4. Locate the `GetText` resolver (Phase 1).

---
*Docs:* [MODDING.md](MODDING.md) · [MODDING_REFERENCE.md](MODDING_REFERENCE.md) ·
[MODDING_CAPABILITIES.md](MODDING_CAPABILITIES.md) · [NATIVE_MODDING_API.md](NATIVE_MODDING_API.md) ·
[ROADMAP.md](ROADMAP.md)
## Native-port ownership update

The final modding platform lives in `Fable2Native`, a clean C++23 runtime. ReXGlue hooks and
translated code are compatibility/oracle tooling while their corresponding native subsystems
are reconstructed. Stable IDs, package manifests, script APIs, menu commands, events, and save
hooks are the public modding contract.
