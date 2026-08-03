# Fable II — Systems Analysis for Modding

Evidence-cited reverse-engineering of the game's actual subsystems, to ground the modding plan in real
understanding. Each section: how the system works, the exact formats/APIs, and a moddability verdict
(Lua-now vs tool-write-path vs native-hook). Sourced from disassembly (`luadis51.py`), the native
catalog (`ghidra_out/lua_natives5_catalog.tsv`), the AssetBrowser decoders (`Fable2AssetBrowser/source/src/`),
and on-disk game data. Companion to [MODDING_CAPABILITIES.md](MODDING_CAPABILITIES.md) (summary table)
and [MODDING_MASTER_PLAN.md](MODDING_MASTER_PLAN.md) (the plan).

> Status: assembled from parallel deep-RE (5 subsystem analyses, all evidence-cited). COMPLETE.

## Consolidated verdict — the pattern across every system

Reverse-engineering all five subsystems reveals one consistent, actionable shape:

**Tier 1 — reference / trigger / tune / EDIT existing content → Lua or data, TODAY (no rebuild):**
- **Spells:** cast real Will effects at pos/target (`SpellManager.CreateScriptedSpellShot`), grant/retune,
  re-skin trails. (~80% of a spellmaker.)
- **Effects/particles:** spawn/attach/fade/kill any `FX_*` effect, place the 8 decal types, retune all
  trails (re-skin via `SSTrailSetStagePFXName`).
- **Items/equipment/appearance:** **edit existing stats/models/augments as a DATA-ONLY GDB round-trip**
  (`GdbEdit.cpp` already parses+serializes the loose `globals.gdb`); grant items; new items by GUID via
  `*OfRecordID`.
- **Quests/interactions/cutscenes/cheats:** already shipping (Lua).

**Tier 2 — author brand-NEW assets → tool WRITE-PATHS (formats fully decoded; encoders are the gap, NOT engine hooks):**
- **New meshes:** MDL serializer + glTF/FBX→MDL importer (then BNK repack).
- **New animations:** AnimBank encoder (re-quantize the 4-mode/Bézier codec) + inject.
- **New particle defs:** `ParticleBankFile` writer (extend `ParticleBank.cpp`) + new `fxt_*.tex`.
- These are AssetBrowser tooling tasks — the read side is done for all three.

**Tier 3 — a few things need a NATIVE hook (`register_native` + targeted overrides):**
- **Custom text** (sign header, InfoBox, tooltips): weak-override the babel resolver **`sub_82374980`
  @0x82374980** (PINNED). *(Menu/message text already renders raw — no hook.)*
- **Freeform spellmaker verbs (3):** AoE-magnitude-in-radius; `Health.Modify(target,delta)` for arbitrary
  targets; free PFX-at-world-pos.
- **New-item string grants:** one `REX_HOOK` on `AddItemOfType` (0x824F0180) — *or avoid entirely* by
  instantiating via GUID.
- **Havok behavior graphs** (movement/combat AI): the one genuinely undecoded system — needs new RE **and**
  a packfile serializer; best driven by a runtime hook, not static authoring. **This is the biggest gap.**
- Plus the cross-cutting native tier: input, UI/HUD overlay, engine-limit removals.

**Sharpened roadmap consequence:** the native tier is SMALLER than it looked — most "modding" is Tier 1
(Lua/data now) or Tier 2 (tool write-paths). The native hooks reduce to: **GetText (pinned)**, **3 spell
verbs**, **1 optional item hook**, and the **Havok** deep-dive — all riding on `register_native`. Meshes,
anims, particles, and items need *encoders/tools*, not engine changes. This front-loads value: ship the
Tier-1 Lua/data wins + the GDB item editor now; add GetText + the spell verbs via the native bridge;
tackle Havok behaviors last (hardest).

---

## Animation, Havok behaviors & Models/Meshes

### Model / mesh format (`MDL/ModelParser.cpp`)
- Container magic `"MeshFile"`, **big-endian** (PPC), header ~0x68. **Skeleton is embedded in the MDL**
  (bind pose): `BoneCount` → per bone {name, `ParentID` i32be, `0xFFFFFFFF`=root}, plus optional
  `BoneTransforms` = 11×f32be **TRS** (quat[4] + trans[3] + scale[3]).
- Geometry has **multiple stride variants** (skinned 28B: pos half3 + 4×u8 boneIDs + 4×u8 weights + UV
  half2; unskinned 20B; foliage 36B/48B). **Normals are NOT stored** — recomputed (smooth-normal
  averaging). Indices = u16be triangle **strips** with `0xFFFF` restart. Skinning capped at **4
  bones/vertex**. Materials = 5 texture-name slots (diffuse/normal/spec/metallic/extra). Cloth = full
  soft-body block (constraints, rest positions).
- ⚠ Format not 100% formalized: four heuristic fallback reparsers scan for `polymsh\0\x01` markers
  (it's dotXSI/POLYMSH-derived) — a meaningful fraction of files rely on pattern-matching.
- **Export (write-out):** `MdlFbxExport.cpp` (binary FBX 7400 — geometry, skin clusters + inverse-bind,
  bindpose, embedded textures, baked anim) and `mdl_converter.cpp` (glTF 2.0/GLB — POSITION/NORMAL/
  TEXCOORD/JOINTS/WEIGHTS, skin, PBR, anim channels, own BC1/3/5 decoders + PNG).
- **Write-back GAP:** **no MDL serializer, no glTF/FBX→MDL importer** (grep-confirmed zero hits).
  Pipeline is **decode + export only (one-way)**.

### Animation banks (`animations/AnimBank.cpp`, `AnimDecoder.cpp`)
- Two files: `fable2_anims.animation_toc` (magic `"AnimBank"`) + `...animation_data` (magic
  `0xCEA5EBED`, v7). TOC per clip: `key0`(anim hash), `key1`(skeleton/track-map id), data_offset,
  frame_count, fps, **event list** (time+name, e.g. `ACTION PERFORM`, `IK_TARGET_SET`). "Special"
  records = shared bone **track maps** (name+parent).
- **Codec:** per bone 7 channels `[qx qy qz qw tx ty tz]`, frames in blocks of 8; each channel is one
  of 4 modes (const-0 / const-1 / single quantized const / **Bézier-curve keyframes**, bit-packed).
  Rotations 16-bit quantized [-1,1], translations 24-bit [-256,256]. A real custom compressed codec,
  **fully decoded**.
- **Referenced by HASH, never by mesh.** Anims retarget to a rig by **bone-name matching** (~66%
  threshold, shadow_ aliasing) → one anim plays on many rigs. Lua: `Animation.DebugPlaySpecificAnimation`
  (0x82564170), `CreateCompositeAnimation`/`CreateCombinationAnimation`, `PickAnAnimationVariation`,
  `SetRunAnimationOverrideName`, `GetCurrentAnimName` — all **select/trigger existing** clips, none create.
- **Write GAP:** decode-only — **no encoder, no TOC/data writer, no injection**.

### Havok behaviors (`Havok/HavokPackfileReader.cpp`)
- Standard Havok binary **tagfile** (magic `0x57E0E057 0x10C0C010`, BE, 3 sections + fixup tables),
  fully parsed incl. `ApplyLocalFixups`.
- **Only collision geometry + placement decoded:** `hkpStorageExtendedMeshShapeMeshSubpartStorage`
  (verts+indices) and `hkpRigidBody` (raw-dumped; positions via heuristic vector scan).
- ⚠ **Behavior graphs entirely undecoded:** zero support for `hkbBehaviorGraph` (state machines/blend
  trees), `hkaSkeleton`, `hkaAnimationBinding`, `hknpRagdoll`, `hkbCharacterData` (grep-confirmed).
  Character movement/combat runs on these — **the AssetBrowser doesn't understand them at all**.
- Game-side Lua can *tune/query*: `PhysicsCharacter.SetRagdollControllerParams`, `PoseRagdoll.SetPose`,
  `SetRagdollVelocitiesFromAnimation`, and **dummy/attach-point** natives (`GetDummyObjectPosition`,
  `GetEntityAttachedToDummy`, `CreateNamedEntityDummyEndPointsArc`) — the FX/weapon-attach mechanism,
  moddable from script today. But no behavior-graph authoring.

### Skeletons / attach points
- Mesh bind-pose skeleton = **inside the MDL**. Animation-time skeleton = separate **track map** in the
  AnimBank TOC, matched by bone name (retargeting). Attach points = named **dummy objects/bones**
  exposed to Lua (weapon/FX attach) — moddable now.

### Verdict
| Domain | Read | Write/author | Do TODAY | Gap to author NEW |
|---|---|---|---|---|
| **Meshes** | Strong (decode+export FBX/GLB; skin+cloth) | **None** (no serializer/importer) | View + export; mechanically swap existing MDL bytes in a BNK via `BnkWriter` | **MDL writer + glTF/FBX→MDL importer** (BE strip/half-float/skin layout, normals recompute-vs-store), then BNK repack. Tool write-path — NOT a native hook. |
| **Animations** | Strong (codec decoded; retarget understood) | **None** (decode-only) | Play/select existing clips from Lua; export to FBX/GLB | **AnimBank encoder** (re-quantize 4-mode/Bézier, build TOC+events+track maps) + inject into `.animation_data/_toc`. FNIS/Nemesis-class = this encoder. |
| **Behaviors (Havok)** | **Weak** (only collision + placement) | **None** | Extract collision; tune ragdoll + dummy-attach from Lua | **Biggest gap:** decode `hkbBehaviorGraph`/ragdoll/`hkaSkeleton` (none exists) **+** packfile re-serializer. Likeliest a **runtime/native hook** (drive the engine's own Havok behavior objects) rather than static file authoring. |

**Bottom line:** meshes & anims are **decode+export only** — authoring new content needs *tool write-paths*
(MDL serializer, glTF importer, AnimBank encoder), not engine hooks. Behaviors need *both* new RE and a
serializer, and are the natural candidate for a runtime hook. Texture replacement + existing-byte swap
(via `BnkWriter`) work today.

---

## Will / Magic / Spells  ★ (mostly moddable from Lua TODAY)

### The ability/spell set
- **Two enums.** `EHeroAbilityType` (skill tree, `bonustypeenum.lua`): WILL_LIGHTNING=7, FIREBALL=8,
  SLOW_TIME=9, SWORDS=10, VORTEX=11, CHAOS=12, FORCE_PUSH=13, DEAD_RISING=14. `ESpellType` (runtime
  cast, `spelltypes.lua`): SPELL_LIGHTNING=0 … SPELL_DEAD_RISING=7. Display names: Shock/Inferno/Time
  Control/Blades/Vortex/Chaos/Force Push/Raise Dead.
- **Levels 0–5** set via `Stats.SetHeroAbilityLevel(entity, EHeroAbilityType, level)` (0x824E39D0);
  read `GetHeroAbilityLevel` (0x824E2DC8). Convenience: `Debug.SetHeroAbilityLevelsToMax`,
  `Debug.SpellsCheat*`. Award/purchase bookkeeping = `*EntitySpellType*Awarded/Purchased*` family.
- **No data-driven spell def file.** Ability *stats* (damage/charge) are hardcoded in native, tuned by
  a large `Set*` native family (§upgrades). Spell assets are cosmetic only (`audio/spells.bnk`,
  `art/videos/*_spell.bik`).

### Casting / dispatch — the effect CAN be fired from Lua
- ★ **`SpellManager.CreateScriptedSpellShot(caster, ESpellType, powerLevel, originPos, bool, target,
  dirVec)`** (0x82540E38-fam) — fires the REAL effect (damage+knockback+VFX+SFX) at a position/target,
  power 1–4. Proven call sites: `generictriggers.lua` (SPELL_FORCE_PUSH), `qc090_garth.lua`
  (SPELL_SWORDS). So a mod can cast any of the 8 spells on demand today.
- **`ScriptControlledTargeting.SetSpellAttack(caster, target, ESpellType, ESpellCastDirMode, power)`**
  (0x8291A3B8) — the AI cast path (proven in behaviourscriptnpctargeting.lua). `ESpellCastDirMode`:
  eSCDM_SURROUND=0 / eSCDM_TARGETED=1.
- **Declarative AI schema**: combat sequences are Lua tables `{Type=EScriptableAction.NPC_MAGIC_QUICK_CAST,
  SpellType=…, SpellDirectionMode=…, PowerLevel=…}` (combatsequences.lua). Player cast state via
  `PlayerSpellManager.IsUsingMagic/IsFullCharging/…`.

### VFX/SFX + tuning (data-driven from Lua)
- Spell trails fully data-driven: `spellshottrailparams.lua` builds each via `Debug.SSTrail*` setters,
  binding PFX **by name** (`FX_Spell_Fireball`, `FX_Forcepush_Projectile`, …). Rebind → re-skin a
  spell. Master: `SetSpellShotTrailsEnabled` (0x823BA6B0), `ReloadSpellShotTrailParams`.
- Tuning knobs (native, callable from a setup script): `SetForceSpellPowerFactor`,
  `SetStopTimeSpellDurationSeconds`, `SetSinglePlayerMaxNumVortexObjects`, charge-time setters, etc.
- Mana/Will: `Mana.Get/GetMax/SetMax/Modify`, `GetWill/ModifyWill`, `Debug.SetHeroWill/Mana/ManaInfinite`.

### Verdict — spellmaking is ~80% Lua TODAY
- **Now, pure Lua:** retune, grant/max, **cast real effects at pos/target**, re-skin VFX, control mana.
- **Freeform Morrowind-style composer** (effect + magnitude + area + duration + cost): chain existing
  primitives — spawn/summon (`Debug.CreateEntityAt`, `SpawnPoint.SpawnCreature`), burning
  (`SetEntityBurning`+`SetEntityBurnDuration`), force (`CreateScriptedSpellShot(FORCE_PUSH)`), kill
  (`Creature.Kill`), mana cost (`Mana.Modify`), radius (`SetAreaRadius`). **Three native-hook gaps** for
  a *true* composer: (1) a generic "apply magnitude to all entities within radius R of P" AoE verb (none
  Lua-exposed); (2) `Health.Modify(target, delta)` exposed for arbitrary targets (native 0x824CBC68 is
  object-bound); (3) free-standing "spawn PFX at world position" decoupled from a spell type. All three
  are perfect first customers for `register_native`.

**Bottom line:** a "spell mod" (new castable spells composed from existing effects, re-skinned, granted,
tuned) is achievable in Lua **now**; three small native verbs turn it into a full freeform spellmaker.

---

## Text / Localization (babel)  ★ (resolver PINNED — custom text now actionable)

### Resolution flow (verified end-to-end in the translated recomp)
A tag becomes text via a `std::map<uint32 FNV1hash, wideText>` ("babel DB"). Chain:
`GetText wrappers sub_822F3D08 / sub_822F3B98` → DB singleton `sub_823745B8` → **THE QUERY
`sub_82374980` @0x82374980** (hash the tag via `sub_822F1FA8`→FNV-1 `sub_822CA588`; RB-tree
`lower_bound` `sub_823751F8`, key@node+0x0C) → on HIT copy wide-string to out; **on MISS the copy is
skipped → blank**. Natives **store** the tag and defer resolution to render time: `SetObjectiveTag`
@0x8251D3A0 interns a tag-id; `DisplayInfoBoxParams` @0x823033D0 carries the tag in its params struct;
both resolved later by the HUD via the wrappers. (`FormatTextTagHash` @0x8245B2D0 is a red herring — a
Lua printf helper, not the resolver.)

### Why menus render raw but objectives/infoboxes blank
Two structurally different paths, NOT one resolver with a fallback:
- **`DisplayMenuBox` @0x82303EE0 / `DisplayMessageBox` @0x823027B0 never call babel** — they copy the
  raw Lua string straight into the widget buffer. So raw strings are the ONLY thing they render (this
  is why the R3 mod menu + message boxes show custom text for free).
- **Objective / InfoBox DO call `sub_82374980`** and have **no raw fallback** — unknown tag → blank.
  There is no shared choke point to "flip a flag."

### babel file (`data/language/en-uk/text/book.babel`, ~2.63 MB)
12-byte big-endian index records; high-entropy (compressed) string blob; UTF-16LE speaker dict at tail.
**Hash = FNV-1 32-bit** (basis 0x811C9DC5, prime 0x01000193, multiply-then-xor over UTF-16 chars —
confirmed in `sub_822CA588`). Tag ending in `\`+40 hex = literal-hash escape. Loaded (+ a
`patch_text/book.babel` overlay) by `sub_82382148`/`sub_823824B8`. **No file parsing needed for the
override** — intercept at resolve time.

### THE override target (for custom text)
- ★ **Weak-override `sub_82374980` @0x82374980** (broadest — every consumer): `r3`=out, `r4`=DB,
  `r5`=key, `r6`=**tagObj** (chars@+0x04, len@+0x14, FNV1@+0x1C). Read the tag from r6; if its hash ∈ our
  mod set, synthesize the out result with our UTF-16 text; else call the original. Unlocks custom text
  for objective headers, InfoBox/hold-A prompts, item tooltips — everything — at once.
- Alt (GUI-text-only, cleaner sig): override `sub_822F3D08`/`sub_822F3B98` @0x822F3D08/0x822F3B98
  `(r3=tagStrObj, r4=outObj)`, write UTF-16 into `outObj+0x24`. Greenfield in `src/` (pattern:
  `src/RtlGetLastErrorFix.cpp`).

### Verdict
- **Menu/message/item-option text: Lua TODAY** (raw render — no work).
- **Objective header, hold-A/InfoBox text, tag tooltips: need the native override** (rebuild). One
  weak-override of `sub_82374980` (mod-hash→mod-string, else passthrough) fixes the **sign header** and
  all custom tag text globally. This is Phase 1 (`ModText_Set` exposed via `register_native`), now with
  an exact target.

---

## Effects / Particles / Decals / Trails  ★ (existing FX fully Lua-moddable; new defs need a bank writer)

### Two FX layers
- **Layer A — FX-as-entity (what scripts use).** A particle effect is spawned as a normal entity from a
  template named `FX_*` via `Debug.CreateEntityAt(fxName, tag, pos)` (0x8245E038) / `CreateEntityAtPosition`
  (0x8245E288), positioned at a model socket from `ScriptFunction.TrackDummy(entity, dummyName)→CVector3`,
  and bound to a host via `ObjectAttachment.AddEntity(host, fx, socket, flags)` (0x825EC7A8) /
  `KillAttachedParticlesWithFadeOut` (0x825EF638). Proven across qc010/miscfunctions/combatstatesdlc2.
- **Layer B — raw particle natives (registered, script-unused).** `CreateParticleAtPosWithDirection`
  (0x823BBE68), `AddDecal` (0x8260E388), `ReloadParticleBank` (0x823BEBD4) — callable but no Lua
  precedent; the FX-entity path is the proven route.

### Particle bank
`art/particles/particle_bank.bnk` (~2.8 MB, magic `ParticleBankFile`, BE) — **global** registry, fully
decoded by `ParticleBank.cpp` (visual/material nodes, SystemDefs, 9 emitter kinds with keyframed
Timelines, Effects). **Effects keyed by FNV-1-lowercase hash — ZERO name strings in the bank** (why
`FX_*` names live only in scripts). 248 `fxt_*.tex` textures. `ParticleFX.cpp` is a preview simulator
(name-keyword heuristics, not byte-exact). **No writer** — read-only.

### Attach / decals / trails
- **Attach:** model dummy sockets (`Prop.FX.Particle.*.par` — parsed by LevelLoader), `TrackDummy` for
  position, `ObjectAttachment`. Turn on a model's built-in particles: `ParticleAttacher.AttachParticles(entity)`
  (0x82765FF0) / `DetachParticles` / `FadeParticles`.
- **Decals:** `DecalManager.AddDecal(EDecalType.<8 fixed types>, entity, CVector3)` — enum-chosen (blood/
  footstep/beetle/burn/generic), not bank-named. Low-level `AddDecal`/`AddDecalToEntity`/`AddDecalToLandscape`.
- **Trails (3 config subsystems):** spell-shot `Debug.SSTrail*` (per-stage; `SSTrailSetStagePFXName(idx,
  stage,'FX_...')` rebinds a stage's effect by name — the re-skin lever); `Breadcrumber.*` (the golden
  trail — length/density/brightness); weapon/entity `SetAutoUpdateTrail`/`SetDrawTrail`.

### Verdict
- **Now, pure Lua:** spawn/attach/fade/kill any existing `FX_*` effect, place any of the 8 decal types,
  and retune **every** trail parameter (incl. re-skin a spell/weapon trail via `SSTrailSetStagePFXName`).
- **Needs work:** a **new particle definition / custom PFX** requires a `ParticleBankFile` **writer**
  (format fully RE'd in `ParticleBank.cpp` — building the encoder is the single unlock) + new `fxt_*.tex`
  + the FNV-1-lowercase name hash into the Effects section. New decal *types* = native (enum is native-side).
  Hot-reload an edited bank = wire `ReloadParticleBank` via a loader hook.

**Bottom line:** referencing/spawning/attaching existing effects + decals + all trail tuning is fully Lua
**today**; new PFX authoring is blocked only by a missing bank encoder (feasible — format understood).

---

## Items / Equipment / Appearance defs  ★ (edit-existing is DATA-ONLY, tool already exists)

### The def store
- **`data/Globals/globals.gdb`** (~3 MB, **71,847 records**) — THE global def store, a **loose file on
  disk** (mounted via `startup.vfsconfig`; `globals.list` = one line `Globals\Globals.gdb`). Format: BE,
  hash-keyed, schema-based ("GDB\0" magic; header {record_count, body_size, schema_blob_size, name_pairs}).
  Each record = `schema_rel` + N u32 field slots; schema gives per-field `{hash, type}` (t1=int, t3=float,
  t4=resource/name-hash, t6/t7=record ref/GUID); `parent` (0x5F6317D5) = **archetype inheritance**. Hash =
  **FNV-1** (0x811C9DC5 / 0x01000193). Fully decoded by `GdbParser.cpp` + `GdbReaderInternal.h`.
- **1728 records carry `InventoryItemComponent`** = the item catalog.

### An item def (concrete: Steel Turret Pistol `0x002C0852`)
Component-composed: `WeaponComponent` {`DamageMultiplier`=42.0, `CombatRating`=72, sound tag},
`InventoryItemComponent` {`NameTag`=INV_ITEM_..._NAME (a GUI text tag), `Rating`=3, `_DESC`},
`GraphicAppearanceStaticMeshComponent` (model), value = `BaseValue` (t3), `parent` (base archetype).
Appearance/clothing/morph = more components (`AppearanceModifierComponent`, `GraphicAppearanceMorph/Eyes/DLC`,
morph composites); augments = `EAugmentationType` (28 types, Lua enum) + weapon `Augmentation1..17` slot fields.

### ID resolution
`Inventory.AddItemOfType(hero, 'ObjectX')` (0x824F0180): the string is FNV-1 hashed and resolved to a
record. ⚠ The record KEY is a GUID, **not** the name-hash; the name→record map is a `t4` field +
name_map. `*OfRecordID` variants (`InstantiateItemOfRecordID` 0x824F2730, `EquipWeaponOfRecordID`) take
the **GUID directly, bypassing string resolution**.

### Verdict — the write toolchain ALREADY EXISTS in-repo
- `GdbEdit.cpp` — `GdbFile::Parse` **+ `Serialize`** round-trips a GDB (rebuilds schema/body/key-array/
  name_map/string-dict). `BnkWriter.cpp` — repack. `ModSupport.cpp` — VFS overlay.
- **Edit existing item stats/model/augments: YES, TODAY, data-only.** Parse `globals.gdb` → find the
  record → edit the typed slot (`DamageMultiplier`, `Rating`, `BaseValue`, swap model hash, augment refs)
  → `Serialize` → overwrite the loose file (or serve via ModSupport). No native hook. (Needs a small
  "set field by record+field-hash" helper wired to the AssetBrowser UI — the primitives exist.)
- **New item TYPES:** append a record with a new GUID + components (data-only via `GdbFile`), then either
  instantiate by **GUID via `*OfRecordID`** (fully data-only, no hook) or, for string-named grants, add
  the name-hash field/name_map entry + one `REX_HOOK` on `AddItemOfType` (0x824F0180).

**Bottom line:** item/equipment/appearance defs are round-trippable GDB records in a loose file, and the
**parse+serialize tool already exists** — editing existing items is a data-only win available now; new
items are data-only if instantiated by GUID.
