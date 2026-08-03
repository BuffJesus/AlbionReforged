# Modding Depth Reference — the Skyrim target, mapped to Fable II

The goal is a **very thorough** modding experience. Skyrim is the gold standard for depth, so this
doc catalogs what Skyrim's ecosystem actually enables, then maps each capability onto Fable II —
what the equivalent is, what we already have, and what the decomp/tooling must provide.

## The two pillars (and why our approach is *stronger* at pillar 2)

Skyrim's depth comes from two layers:

1. **Creation Kit (CK)** — the official *authoring tool*: edit records, worlds, quests, dialogue,
   magic, and attach **Papyrus** scripts. Bounded by what the engine exposes.
2. **SKSE (Script Extender)** — the community *native-code layer*: DLL plugins that **hook the
   engine's executable** to expose internals Papyrus can't reach (300+ new script functions,
   memory patches, custom UI/menus). This is what removes the CK's ceiling — SkyUI, MCM, and most
   deep gameplay mods depend on it.

**Key insight for us:** SKSE exists because Skyrim's engine is *closed* — the community
reverse-engineers hooks blindly (hence the "Address Library" for version-independent offsets).
**We are building a recomp/decomp — we *own* the engine source.** So the "SKSE tier" (native code
that can change anything) is not a hard-won hack for us; it's the *default*. We can expose a clean,
first-class modding API instead of scraping for offsets. In principle our ceiling is **higher** than
Skyrim's.

## Full capability catalog (Skyrim → Fable II)

| Category | Skyrim (CK + SKSE + tools) | Fable II equivalent | Have / Need |
|---|---|---|---|
| **World / cells** | Place statics, activators, lights, triggers; interior & exterior cells; new **worldspaces** (whole new lands, e.g. Enderal, Beyond Reach) | Fable levels/regions (TNG/LEV placement + EHF environment chunks) | AssetBrowser reads levels; **need** robust write/authoring + placement editor |
| **Terrain / landscape** | Heightmap Editor, land textures, LOD (DynDOLOD), navmesh | Fable heightfields + terrain-texture registry | AssetBrowser decodes heightfields/terrain; **need** heightmap *write* + LOD + AI-path equivalent |
| **Items / equipment** | Weapons, armor, clothing, ingredients, potions, books, misc; stats, enchantments, keywords; **leveled lists** (randomized loot) | Fable item/appearance defs + models | **Need** decomp of the item/def system + def editor; models via AssetBrowser |
| **NPCs / followers / creatures** | Appearance (FaceGen), stats, factions, **AI packages**, combat styles, voice; custom followers & creatures | Fable NPC/creature defs + skeletons/anims | AssetBrowser reads models/anims/skeletons; **need** NPC/AI def decomp + editor |
| **Magic / spells / perks** | **Magic Effects** (the logic/damage/type) → **Spells** (cost/casting) → **Enchantments**; **Perks** & perk trees; shouts; scripted effects for novel logic | Fable **Will** (magic) system — spells, spell weaving, upgrades | **Need** decomp of the Will/ability system; new spell = new effect + ability record + logic (Lua/native) + VFX |
| **Quests** | Quest stages, objectives, **aliases**, conditions; scripted branching; achievements | Fable quests (**Lua-scripted**) | Lua is the quest layer (AssetBrowser has a Lua decompiler); **need** quest/Lua editor + understanding of quest data |
| **Dialogue / voice** | Dialogue trees (topics, responses, conditions), emotions, lip-sync; voice files | Fable dialogue + the "expression" system | **Need** dialogue-data decomp + editor; TTS/voice pipeline for new lines |
| **Scripting** | **Papyrus** (CK) + **SKSE native** (unbounded) | Fable **Lua** (game logic) + **recomp/decomp native hooks** | Lua = the "Papyrus"; recomp `REX_HOOK` / decomp = the "SKSE" — but first-class since we own the engine |
| **Animation** | Behavior graphs via **FNIS/Nemesis/Pandora**; new anims, paired anims, combat movesets | Fable anim banks + **Havok** behaviors | AssetBrowser reads anim banks + Havok; **need** anim *inject* + behavior-graph editing |
| **Graphics** | Texture/mesh replacers (NifSkope), **BodySlide/Outfit Studio** (body/armor morphs), ENB, weathers, lighting, imagespaces | Fable textures/models/shaders/lighting | AssetBrowser decodes textures/models; **need** import/replace pipeline + shader/lighting hooks |
| **Audio** | Sound descriptors, music, custom SFX | Fable XMA2 audio + sound banks | AssetBrowser decodes XMA2; **need** audio inject |
| **UI / HUD** | **SkyUI**, MCM, custom menus (SKSE) | Fable HUD/menus | **Need** UI hooks (native) — natural via recomp/decomp |
| **Gameplay overhauls** | Combat, survival/needs, economy, leveling — mix of records + Papyrus + SKSE | Any system, via Lua + native hooks | The recomp/decomp makes deep systemic overhauls *first-class* |
| **Total conversions** | Whole new games on the engine (Enderal) | Long-horizon: a new game on the Fable II engine | Everything above, at scale |

## The tool ecosystem (and our equivalents)

| Skyrim tool | Purpose | Fable II equivalent (plan) |
|---|---|---|
| **Creation Kit** | Master authoring GUI (records/world/quests/dialogue/Papyrus) | Our **CK-style tool** — grow from Fable2AssetBrowser |
| **xEdit (SSEEdit)** | Record editing, conflict resolution, patch generation | A Fable def/record editor + conflict/merge for the mod-overlay |
| **NifSkope** | Mesh editing | AssetBrowser MDL tools + glTF round-trip |
| **BodySlide / Outfit Studio** | Body/armor morphs & fitting | Armour/appearance fitting tool (later) |
| **FNIS / Nemesis / Pandora** | Animation behavior-graph patching | Havok behavior + anim-bank injection tool |
| **DynDOLOD** | LOD generation | Terrain/level LOD generator |
| **Mod Organizer 2 / Vortex** | Mod manager (load order, VFS) | Our **mod manager** over the recomp VFS overlay (`ModSupport.cpp`) + `BnkWriter` injection |

## Concrete example: a **custom spell** (what you specifically asked about)

In Skyrim a new spell is: a **Magic Effect** (defines behavior — damage type, area, duration, and an
optional Papyrus/SKSE script for novel logic) → wrapped in a **Spell** record (cost, cast type,
delivery) → given **art** (projectile, cast/hit FX, sound) → made available (spell tome, vendor, or
perk). Deep spells (e.g. Apocalypse's 150+ spells) lean on SKSE for effects the base engine can't do.

**Fable II mapping:** Fable's magic is the **Will** system. A custom Will spell needs:
1. **Understand the Will system** (decomp) — how abilities, effects, targeting, and upgrades are
   represented and dispatched.
2. **A new ability/effect record** in whatever def format Fable uses (authored in our tool).
3. **The effect logic** — reuse an existing effect, script it in **Lua**, or (for truly new behavior)
   a **native hook** in the recomp/decomp. This is the "Magic Effect + script" tier — and native is
   *easy* for us because we own the engine.
4. **VFX/SFX** — new particle/sound assets via AssetBrowser (or reuse existing).
5. **Availability** — grant it (a Will upgrade node, a pickup, or debug).

So a custom spell touches every layer: read (formats), author (the record + VFX), engine-bridge
(the Will dispatch), and scripting (Lua/native logic). It's the perfect stress-test of a thorough
modding pipeline.

## What this means for our roadmap
Thorough modding = **both pillars**: an authoring tool (CK-equivalent, grown from AssetBrowser)
*and* a native-code modding API (SKSE-equivalent, which the recomp/decomp gives us naturally). The
sequencing in [ROADMAP.md](ROADMAP.md) builds toward this: decode formats → overlay/inject → author
→ engine-bridge (extend loaders + expose a modding API) → deep systems (magic, quests, AI) → the
full CK-style GUI. Custom spells, new lands, followers, overhauls, and eventually total conversions
all fall out of that stack.

---
Sources: [UESP CreationKit wiki](https://ck.uesp.net/wiki/Main_Page),
[Creation Kit Usage (UESP)](https://en.uesp.net/wiki/Skyrim_Mod:Creation_Kit_Usage),
[SKSE / Papyrus extenders (Nexus)](https://www.nexusmods.com/skyrimspecialedition/mods/22854),
[SKSE plugin ecosystem (modding.wiki)](https://modding.wiki/en/skyrim/users/skse-plugins),
[Total conversions overview](https://gamerant.com/skyrim-best-total-conversion-mods/).
