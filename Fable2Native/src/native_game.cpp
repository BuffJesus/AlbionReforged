#include "f2/native_game.h"

#include "f2/native_bindings.h"
#include "f2/native_hero_anim.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string_view>

namespace f2 {

namespace {
// Own-format save magic + version gate ("F2SV"). Bumped when the format changes.
constexpr std::uint32_t kSaveMagic = 0x46325356u;
}  // namespace

bool NativeGame::enable_scripting() {
    script_vm = std::make_unique<NativeScriptVM>();
    if (!script_vm->valid()) {
        script_vm.reset();
        return false;
    }
    script_vm->set_user_data(this);

    // The gameplay native API (Debug/Game/Player/World/Quest/Camera/Health...). Everything
    // the game's scripts call beyond this falls through to the auto-stub (boot path).
    register_native_api(*script_vm, *this);

    // Wire the three managers to their Lua Update entry points, in the retail-recovered
    // order (Quest -> General -> AI). The Lua side owns its coroutine scheduling; the
    // native tick just resumes each manager's Update each InWorld step. call_global is a
    // no-op (returns false) until a loaded manager script defines the function.
    // Drive each manager's Lua Update in the retail order. Retail resumes the manager
    // OBJECT's Update METHOD (Manager:Update(dt)); fall back to a free *Update global (used
    // by simple mods/tests). The Lua side owns coroutine scheduling.
    NativeScriptVM* vm = script_vm.get();
    script_systems.quest.enabled = true;
    script_systems.quest.update = [vm](double dt) {
        if (!vm->call_method("QuestManager", "Update", dt)) vm->call_global("QuestUpdate", dt);
    };
    script_systems.general.enabled = true;
    script_systems.general.update = [vm](double dt) {
        if (!vm->call_method("GeneralScriptManager", "Update", dt)) vm->call_global("GeneralUpdate", dt);
    };
    script_systems.ai.enabled = true;
    script_systems.ai.update = [vm](double dt) {
        if (!vm->call_method("AIManager", "Update", dt)) vm->call_global("AIUpdate", dt);
    };
    return true;
}

int NativeGame::boot_game_scripts(const std::filesystem::path& data_root) {
    if (!script_vm) return -1;
    script_bnk = std::make_unique<BnkReader>();
    // Prefer the cooked native script package (tools/cook_scripts.py output) when present — the
    // port-plan "consume the native package" path (docs/NATIVE_PORT_PLAN.md): decompressed once
    // at install, so boot skips per-entry zlib. Fall back to decompressing the raw user BNK.
    const auto cooked_dir = data_root / "cooked" / "scripts";
    const auto bnk_path = data_root / "data" / "gamescripts_r.bnk";
    if (!script_bnk->open_cooked(cooked_dir.string()) && !script_bnk->open(bnk_path.string())) {
        script_bnk.reset();
        return -1;
    }
    loaded_scripts.clear();

    // Substrate natives BEFORE the scripts run: the manager-registration + entity + message
    // + gameflow natives the game's own Lua calls (register_game_systems_api), and RunScript
    // (register_boot_api). The auto-stub, installed LAST, swallows the still-unimplemented
    // long tail so quest/gameflow Lua runs without every native.
    register_game_systems_api(*script_vm, *this);
    register_boot_api(*script_vm, *this);

    // Run the real boot: generalsetupscript loads GeneralScriptManager + every enum/helper/
    // interactable and self-registers via SetGeneralScriptManager. (RunScript skips the
    // project's own MyConsoleHook0 mod-menu hook — not stock game logic; see register_boot_api.)
    std::vector<std::uint8_t> boot =
        script_bnk->extract("miscellaneous/generalsetupscript.lua");
    if (!boot.empty()) {
        script_vm->run_bytecode(boot.data(), boot.size(), "=generalsetupscript");
    }

    // Open the game database the GDB native class reads. globals.gdb holds entity archetypes and
    // animation sets; interactivecutscenes.gdb holds the cutscene records PlayCutscene looks up by
    // name (QuestEntityThreadBase.PlayCutscene: GDB.RecordExists -> GDB.GetRecord ->
    // record:GetFloat("MaxRangeFromPlayer")). Each .list beside them is the game's own manifest.
    // Order = search order; a level's own gdb should be add()ed BEFORE these once one is cooked.
    // Missing files are not fatal — the natives then answer "no record", same as a bad name.
    for (const char* rel : {"data/Globals/globals.gdb",
                            "data/interactivecutscenes/interactivecutscenes.gdb"}) {
        gdb.add(data_root / rel);
    }

    // The localised text table. Cook it with tools/babel_text.py from the user's own
    // data/language/<locale>/text/book.babel; the runtime never ships strings. Absent is fine —
    // GetText then answers with the tag, exactly as the game does for an unknown tag.
    for (const char* rel : {"cooked/en-uk.f2text", "cooked/text.f2text"}) {
        if (text.load(data_root / rel)) break;
    }

    // The camera scripts are engine-loaded, not script-loaded: camera/camerasetupscript.lua is
    // just a list of AddCameraScriptFile(...) calls and nothing in the shipped scripts runs it
    // (see the AddCameraScriptFile binding for the evidence). Run it here, before the auto-stub,
    // so CameraFunctions/CameraValues/the camera classes are REAL tables — the scripted-cutscene
    // camera cages (CameraFunctions.CreateGenericClosure) are built from them.
    if (std::vector<std::uint8_t> cam = script_bnk->extract("camera/camerasetupscript.lua");
        !cam.empty()) {
        loaded_scripts.push_back(BnkReader::normalize("camera/camerasetupscript.lua"));
        script_vm->run_bytecode(cam.data(), cam.size(), "=camerasetupscript");
    }

    // Define the Platform enum consistently with GetPlatform() (Win32=2) before the auto-stub
    // claims `Platform` as a stub table. Values are arbitrary but self-consistent.
    script_vm->run_source("Platform = { Xbox360 = 1, Win32 = 2, PS3 = 3, PC = 2 }", "=platform");

    script_vm->install_autostub();

    // Make Lua's require() load quest modules from the BNK: QuestManager.LoadQuestModule uses
    // require() (which searches the filesystem), but our scripts live in the BNK. Insert a
    // package.loaders entry that compiles the module via __bnk_chunk. (module((...),
    // package.seeall) inside a quest module then sets up its namespace normally.)
    script_vm->run_source(
        "if package and package.loaders then table.insert(package.loaders, 1, function(m) "
        "local c = __bnk_chunk(m); if c then return c end; return '\\n\\tno BNK module '..m end) end",
        "=bnk_require");

    // Stage 2 control shim: a faithful CVector3 (the engine type is a native C++ vector; vector
    // math is unambiguous) + the Physics.* public natives that bridge CVector3 objects to the
    // scalar C++ primitives registered above. Installed AFTER the auto-stub so it overrides the
    // black-hole CVector3 stub; classmeta has no __newindex, so adding methods to the real
    // Physics table is a plain rawset. FLAGGED: CVector3 reimplements the native type's behaviour
    // (Normalise mutate-vs-return not RE'd — chosen in-place-returning-self).
    script_vm->run_source(
        "do local mt = {} mt.__index = mt "
        "function mt:GetX() return self.x end function mt:GetY() return self.y end "
        "function mt:GetZ() return self.z end "
        "function mt:GetSquaredLength() return self.x*self.x + self.y*self.y + self.z*self.z end "
        "function mt:GetLength() return math.sqrt(self:GetSquaredLength()) end "
        "function mt:Normalise() local l=self:GetLength() if l>0 then "
        "  self.x,self.y,self.z = self.x/l,self.y/l,self.z/l end return self end "
        "mt.__sub = function(a,b) return CVector3(a.x-b.x, a.y-b.y, a.z-b.z) end "
        "mt.__add = function(a,b) return CVector3(a.x+b.x, a.y+b.y, a.z+b.z) end "
        "mt.__mul = function(a,s) if type(a)=='number' then a,s=s,a end "
        "  return CVector3(a.x*s, a.y*s, a.z*s) end "
        "function CVector3(x,y,z) if type(x)=='table' then "
        "  return setmetatable({x=x.x or 0, y=x.y or 0, z=x.z or 0}, mt) end "
        "  return setmetatable({x=x or 0, y=y or 0, z=z or 0}, mt) end "
        "__CVector3_mt = mt end "
        "function Physics.TeleportToPosition(e,pos) Physics.__Teleport(e, pos.x, pos.y, pos.z) end "
        "function Physics.SetFacingVector(e,vec) Physics.__SetFacing(e, vec.x, vec.y, vec.z) end "
        "function Physics.GetFacingVector(e) return CVector3(Physics.__GetFacingRaw(e)) end "
        "function Physics.GetVelocity(e) return CVector3(Physics.__GetVelocityRaw(e)) end "
        // Navigation.MoveToPosition(entity, {position=CVector3, radius, speed=ENavigationSpeed}).
        // speed defaults to WALK (tier 2); the game usually sets NPC speed separately (FLAGGED).
        "function Navigation.MoveToPosition(e, opts) "
        "  local p = opts and opts.position "
        "  local r = (opts and opts.radius) or 0 "
        "  local s = (opts and opts.speed) or 2 "
        "  if p then Navigation.__MoveTo(e, p.x, p.y, p.z, r, s) end return true end "
        // Camera pose setters take CVector3 (unpacked to the scalar primitives).
        "function Camera.MoveTo(pos) Camera.__MoveTo(pos.x, pos.y, pos.z) end "
        "function Camera.SetDirection(dir) Camera.__SetDirection(dir.x, dir.y, dir.z) end",
        "=stage2_control");

    // ModMenu.Register(category, label, fn [, detail]) — the mod-facing half of the mod/debug-jump
    // menu (native_mod_menu.h). Defined in Lua because it captures a Lua FUNCTION; ModMenu::rebuild
    // discovers these alongside the game's OWN skip functions (Gameflow.ChildhoodVars.SkipTo*) and
    // every quest the gameflow registers as jumpable (Gameflow.DebugQuestStartTable). A mod script
    // loaded through NativeGame::load_mods can therefore extend the menu with no C++ change:
    //     ModMenu.Register("Mod", "Give 1000 gold", function() Money.Give(GetPlayerHero(), 1000) end)
    script_vm->run_source(
        "__f2_modmenu_entries = __f2_modmenu_entries or {} "
        "ModMenu = ModMenu or {} "
        "function ModMenu.Register(category, label, fn, detail) "
        "  if type(fn) ~= 'function' then return false end "
        "  __f2_modmenu_entries[#__f2_modmenu_entries + 1] = "
        "    {category = category or 'Mod', label = label or 'mod entry', fn = fn, detail = detail} "
        "  return true end",
        "=modmenu_api");

    // The childhood CHOICE entries, registered through the very same public API a mod uses — so
    // they double as a worked template (copy this shape in a mod script to add your own).
    // Both variables are the GAME'S OWN and their effect is verified, not guessed:
    //   Gameflow.ChildhoodResolutionEvil selects which BWSSlums scenario the gameflow activates
    //   after the childhood — Chapter2Slums (evil) vs Chapter2Posh (good), gameflow.txt around the
    //   ChildhoodResolutionEvil branch — i.e. this is literally "where the warrants went".
    //   Gameflow.ChildhoodVars.WantedCompleted marks the warrants sub-quest resolved
    //   (qc010_childhood.lua's SkipTo* functions set the same flag).
    // The recomp's mod menu made the same two writes behind an in-world sign; here they are menu
    // entries, so there is no HUD/toaster hack and the choice is reversible before it is used.
    script_vm->run_source(
        // NOTE: no `Gameflow` guard here — Gameflow does not exist yet at boot; the
        // closures resolve it when the entry is INVOKED, which is the point.
        "if ModMenu and ModMenu.Register then "
        "  ModMenu.Register('Choice', \"Old Town's fate: warrants to Derek (good)\", function() "
        "    Gameflow.ChildhoodResolutionEvil = false "
        "    if Gameflow.ChildhoodVars then Gameflow.ChildhoodVars.WantedCompleted = true end "
        "  end, 'post-childhood scenario becomes Chapter2Posh') "
        "  ModMenu.Register('Choice', \"Old Town's fate: warrants to Arfur (evil)\", function() "
        "    Gameflow.ChildhoodResolutionEvil = true "
        "    if Gameflow.ChildhoodVars then Gameflow.ChildhoodVars.WantedCompleted = true end "
        "  end, 'post-childhood scenario becomes Chapter2Slums') "
        "end",
        "=modmenu_choices");

    // Wire the registered manager Update callbacks into the tick, retail Quest->General->AI
    // order. The managers are pure Lua; the native tick just resumes each one's Update.
    NativeScriptVM* vm = script_vm.get();
    NativeGame* self = this;
    script_systems.quest.enabled = true;
    script_systems.quest.update = [vm, self](double) {
        if (self->quest_update_ref >= 0) vm->call_ref(self->quest_update_ref);
    };
    script_systems.general.enabled = true;
    script_systems.general.update = [vm, self](double) {
        if (self->general_update_ref >= 0) vm->call_ref(self->general_update_ref);
    };
    script_systems.ai.enabled = true;
    script_systems.ai.update = [vm, self](double) {
        if (self->ai_update_ref >= 0) vm->call_ref(self->ai_update_ref);
    };

    return static_cast<int>(loaded_scripts.size());
}

int NativeGame::load_quest_scripts() {
    if (!script_vm || !script_bnk) return -1;
    // The quest bootstrap (quests/questsetupscript.lua) RunScripts QuestManager.lua + the
    // quest modules + gameflow. QuestManager.lua self-registers via SetQuestUpdateFunction.
    std::vector<std::uint8_t> qb = script_bnk->extract("quests/questsetupscript.lua");
    if (qb.empty()) return -1;
    // The quest scripts store their managers in a `BaseObjects` table but reference them as
    // globals (BaseObjects.QuestManager = {...} then `QuestManager.NewQuestThread(...)`), so
    // BaseObjects must alias _G. The engine sets this up before the quests bank runs.
    script_vm->run_source("if not BaseObjects then BaseObjects = _G end", "=baseobjects");

    // The save/load system defines helpers the quest machinery calls at registration time
    // (AddFunctionsInTableToPermanentsTables, via QuestManager.AddQuestToPermanentsTables).
    // The engine loads it in C++ (no Lua RunScript references it), so we load it here too,
    // before the quests bank. Best-effort: a failure just leaves those helpers to the stub.
    // Common helper scripts the quest modules expect as globals but that generalsetupscript
    // doesn't load (the engine loads them on another path): saveload (permanents helpers) and
    // utils (CreateEnum, used by quest modules at load time).
    for (const char* dep : {"miscellaneous/saveload/saveloadsystem.lua",
                            "miscellaneous/utils.lua",
                            "miscellaneous/navigationspeedenum.lua"}) {  // ENavigationSpeed tiers
        std::vector<std::uint8_t> b = script_bnk->extract(dep);
        if (!b.empty()) script_vm->run_bytecode(b.data(), b.size(), (std::string("=") + dep).c_str());
    }

    const std::size_t before = loaded_scripts.size();

    // Pre-load QuestManager (questsetupscript's first RunScript) via RunScript so it registers
    // in loaded_scripts, THEN neutralize its save/permanents registration BEFORE the quest
    // modules load. FLAGGED stand-in: QuestManager.AddQuestToPermanentsTables reaches into
    // PlutoPermanentsSaveTable (initialized by the save subsystem we don't fully wire yet), so
    // every quest's NewQuestThread would otherwise abort at registration. Neutralizing it here
    // (before the quests, after QuestManager exists) lets ALL quests + gameflow load fully;
    // only save-persistence of quest state is deferred. Remove once save/permanents is wired.
    script_vm->run_source("RunScript('quests/questmanager.lua')", "=preload_qm");
    script_vm->run_source(
        "if QuestManager and QuestManager.AddQuestToPermanentsTables then "
        "QuestManager.AddQuestToPermanentsTables = function() end end",
        "=permanents_shim");

    // Now run the quest bootstrap; its RunScript('Quests/QuestManager.lua') is de-duped, and the
    // quest modules + gameflow load with the permanents registration neutralized.
    script_vm->run_bytecode(qb.data(), qb.size(), "=questsetupscript");

    // FLAGGED stand-ins for peripheral subsystems the gameflow coroutine drives but that aren't
    // wired yet, so Gameflow.Update advances instead of asserting. Orchestra/crescendo (audio
    // mood intensity) is called from CheckForGamePosition on every position change and asserts a
    // per-chapter gameflow name we don't author. Remove as each subsystem is implemented.
    script_vm->run_source(
        "if Orchestra then Orchestra.SetToDefaultForChapter = function() end "
        "  Orchestra.SetFromGameflow = function() end end "
        // The save/permanents tables the (un)load path indexes — empty tables so
        // AddFunctions/RemoveFunctionsInTableFromPermanentsTables no-op cleanly.
        "PlutoPermanentsSaveTable = PlutoPermanentsSaveTable or {} "
        "PlutoPermanentsLoadTable = PlutoPermanentsLoadTable or {} "
        // FLAGGED: keep quest-base metatables writable. The save system flips
        // ReadOnlyMetatablesActive on, which makes QuestThreadBase.__NewIndexFunc reject
        // non-function fields on quest TYPES (as opposed to instances) — but several scripts
        // (incl. the MyFirstQuest tutorial) set type-level fields. Off until save is wired.
        "ReadOnlyMetatablesActive = false",
        "=gameflow_compat");

    return static_cast<int>(loaded_scripts.size() - before);
}

void NativeGame::start_new_game(bool female) {
    if (!script_vm) return;
    // FLAGGED: gender -> child hero model. Stored as the hero entity's name so Gender.Get /
    // ChangePlayerEntityType have something to read until the appearance/model system is wired.
    const std::uint64_t hero = hero_uid;  // ensure_hero() runs inside GetPlayerHero below
    (void)hero;
    // The new-game handoff: prime QuestManager.HeroEntity, set GameflowMode, run Gameflow:Init
    // (populates the DebugQuestStartTable), and release the GAMEFLOW_START gate. After this, the
    // InWorld tick's QuestManager.Update advances the gameflow into QC010_Childhood.
    //
    // ORDER IS LOAD-BEARING: `GameflowMode` must be true BEFORE Init runs. The game ships its own
    // source for this file — `scripts/quests/gameflow.txt` in gamescripts_r.bnk — and it branches
    // on the flag mid-Init:
    //     2037  if QuestTracker.IsToStartGameflow(QuestManager.HeroEntity) then
    //     2038      Gameflow.GameflowMode = true
    //     2250  if not Gameflow.GameflowMode then
    //     2251      print("Gameflow INACTIVE")     -- debug/sandbox kit: dog, weapons, potions
    //     2289  else print("Gameflow ACTIVE")      -- the REAL new-game setup
    //     2383      Gameflow.WeaponNames = {}
    //     2384      Gameflow.WeaponNames.ChildMelee = "ChildSwordWooden"
    // Setting the flag after Init meant Init took the INACTIVE branch, so `Gameflow.WeaponNames`
    // was never created — and QC010_Childhood's Update dies on its FIRST frame at
    // `Gameflow.WeaponNames.ChildMelee` (qc010_childhood.lua Update instr 213-214). The gameflow's
    // WaitForQuestToFinish then sees a dead coroutine, prints "QC010 Ending" and walks on to
    // QC060/QC070 — which is exactly why the childhood "started" yet nothing ever played, and why
    // the census recorded `2x attempt to index field 'WeaponNames'` with QC070_Thag as the only
    // live quest thread.
    //
    // FLAGGED: pre-setting the flag stands in for `QuestTracker.IsToStartGameflow` (line 2037),
    // which the port does not implement — the auto-stub answers false for any `Is*` predicate. The
    // durable fix is to bind that native; this reproduces the state retail is in on a new game,
    // where the flag is only ever set, never cleared.
    //
    // Init's error is REPORTED, not swallowed: this bug hid behind a bare pcall for the whole
    // investigation. Debug.Error lands in script_log (native_bindings.cpp).
    script_vm->run_source(
        "QuestManager.HeroEntity = GetPlayerHero(); "
        "if Gameflow then "
        "  Gameflow.GameflowMode = true; Gameflow._Initialised = true; "
        "  Gameflow.LoadedFromSave = false; Gameflow.SkipToNextPositionInGameflow = true; "
        "  local ok, err = pcall(function() Gameflow:Init() end); "
        "  if not ok then Debug.Error('Gameflow:Init failed: ' .. tostring(err)) end; "
        "end",
        "=start_new_game");
    const std::string model = female ? "CreatureHeroFemaleChild" : "CreatureHeroChild";
    if (hero_uid != 0) entity_names[hero_uid] = model;
}

int NativeGame::load_mods(const std::filesystem::path& dir) {
    if (!script_vm) return 0;
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) return 0;
    std::vector<std::filesystem::path> mods;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (entry.path().extension() == ".lua") mods.push_back(entry.path());
    }
    std::sort(mods.begin(), mods.end());  // deterministic load order
    int loaded = 0;
    for (const auto& mod : mods) {
        if (script_vm->run_file(mod.string().c_str())) ++loaded;
        // else: broken mod -> skip (its error is in script_vm->last_error()), not fatal
    }
    return loaded;
}

std::vector<std::uint8_t> NativeGame::save_state() {
    WorldArchive ar(ArchiveMode::Write);
    std::uint32_t magic = kSaveMagic;
    ar.visit(magic);
    game_state.hero_position = player.position();
    game_state.serialize(ar);   // header + quest bitfield + hero position
    world.serialize(ar);        // live entity delta
    return ar.take();
}

bool NativeGame::load_state(const std::vector<std::uint8_t>& data) {
    WorldArchive ar(data);
    std::uint32_t magic = 0;
    ar.visit(magic);
    if (magic != kSaveMagic) return false;
    game_state.serialize(ar);
    if (game_state.header.version != 1) return false;  // future-version gate
    player.set_position(game_state.hero_position);
    world.serialize(ar);  // overlay the delta onto the current (rebuilt) baseline
    world.resync_npcs_from_entities();  // agents resume at their restored transforms
    return ar.ok();
}

bool NativeGame::load_scene(const std::filesystem::path& path, std::string& error) {
    elapsed_seconds = 0.0;
    simulation_accumulator_ = 0.0;
    camera = NativeCamera{};
    frontend.reset();
    if (!load_native_scene(path, scene, error)) {
        return false;
    }
    prepare_world();
    return true;
}

void NativeGame::prepare_world() {
    // Seed the live entity graph from the cooked baseline (one entity per instance,
    // Transform + GraphicAppearanceStaticMesh). Sim only advances it when InWorld.
    world.spawn_from_scene(scene);

    // Build static collision (AABBs + terrain heightfield) and place the hero at the
    // RE'd PlayerStart (SimpleTransformComponent). Camera starts behind the hero's yaw.
    collision.build_from_scene(scene);
    std::array<float, 3> start = scene.has_hero_start ? scene.hero_start
                                                      : std::array<float, 3>{0.0f, 0.0f, 0.0f};
    if (scene.has_hero_start) {
        const float gy = collision.sample_ground(start[0], start[2], start[1] + 2.0f);
        if (std::isfinite(gy)) start[1] = gy;
    }
    player = NativePlayer{};
    player.set_position(start);
    camera_controller.yaw = scene.hero_yaw;
    camera_controller.pitch = -0.3f;
}

// Walk a cutscene record's authored beat list and perform what we can.
//
// Structure (readable with tools/gdb_record_dump.py, decoded from the game's own data):
//     QC010_JeevesGreet
//       UseCutsceneCamera bool
//       SceneElements     record
//           SayLine       record  { Character, CharacterToTalkTo, TextTag }
//           SayLine       record  { ... }            <- the SAME field name repeats, one per beat,
//           SetEntityMode record  { Character, AnimationGroup }   which is why this ENUMERATES
//                                                                 fields instead of looking up
// A SayLine's TextTag resolves through the game's own localised text, so the port speaks the
// game's words. FLAGGED: beats are emitted instantly, with no timing, camera or animation.
std::uint32_t NativeGame::gdb_token_for(std::uint32_t guid) {
    for (std::size_t i = 0; i < gdb_id_tokens_.size(); ++i)
        if (gdb_id_tokens_[i] == guid) return static_cast<std::uint32_t>(i + 1);
    gdb_id_tokens_.push_back(guid);
    return static_cast<std::uint32_t>(gdb_id_tokens_.size());
}

std::uint32_t NativeGame::gdb_guid_for_token(std::uint32_t token) const {
    if (token == 0 || token > gdb_id_tokens_.size()) return 0;
    return gdb_id_tokens_[token - 1];
}

// Read a cutscene's authored beat list. Field order in the schema IS play order (verified: the
// SayLine beats of QC010_JeevesGreet come out _02, _04, _10, _20, _30). Beat schemas, all decoded
// from the game's own data with tools/gdb_record_dump.py:
//     SayLine                 { Character, CharacterToTalkTo, TextTag, WaitUntilComplete }
//     Wait                    { TimeToWait }                      <- authored seconds
//     SetLookAtCamera         { PositionEntity, FocusEntity }     <- each a marker record whose
//                                PhysicsSimpleComponent.Position is the camera pose
//     PlayAnimation           { Character, AnimationName, CharacterToFace }
//     StartLookingAtCharacter { Character, CharacterToLookAt }
// Kinds we do not stage yet are still recorded (duration 0) so the ORDER and count stay honest.
std::vector<NativeGame::CutsceneBeat> NativeGame::build_cutscene_beats(std::uint32_t record_id) const {
    std::vector<CutsceneBeat> out;
    const auto elements = gdb.field_raw(record_id, "SceneElements", gdb::kTypeRecord);
    if (!elements) return out;

    // A type-7 field is also a record reference; a camera anchor's position lives on its
    // PhysicsSimpleComponent (the same component the level markers use).
    auto anchor_pos = [this](std::uint32_t anchor, std::array<float, 3>& out_pos) {
        const auto phys = gdb.field_raw(anchor, "PhysicsSimpleComponent", gdb::kTypeRecord);
        if (!phys) return false;
        const auto pos = gdb.field_raw(*phys, "Position", gdb::kTypeRecord);
        if (!pos) return false;
        const auto x = gdb.field_float(*pos, "X");
        const auto y = gdb.field_float(*pos, "Y");
        const auto z = gdb.field_float(*pos, "Z");
        if (!x || !y || !z) return false;
        out_pos = {*x, *z, *y};   // game(x,y,z) -> world {x,z,y}
        return true;
    };

    for (const auto& field : gdb.fields(*elements)) {
        const char* kind = gdb.intern(field.name_hash);
        if (!kind || std::string_view(kind) == "parent") continue;
        if (field.type != gdb::kTypeRecord && field.type != 7) continue;

        CutsceneBeat beat;
        beat.kind = kind;
        if (const char* c = gdb.field_string(field.value, "Character")) beat.character = c;
        if (const char* c = gdb.field_string(field.value, "CharacterToTalkTo")) beat.listener = c;

        if (beat.kind == "SayLine" || beat.kind.find("Talks") != std::string::npos ||
            beat.kind.find("Speaks") != std::string::npos) {
            if (const char* tag = gdb.field_string(field.value, "TextTag")) {
                beat.tag = tag;
                beat.text = text.get(tag);
                beat.duration = say_line_base_seconds +
                                say_line_per_char_seconds * static_cast<double>(beat.text.size());
            }
        } else if (beat.kind == "Wait") {
            if (const auto t = gdb.field_float(field.value, "TimeToWait")) beat.duration = *t;
        } else if (beat.kind == "MoveToMarker") {
            // MarkerToMoveTo is a marker RECORD (type 7); its PhysicsSimpleComponent.Position is
            // the destination. Range is the authored arrival radius, WaitUntilComplete says
            // whether the scene holds for the walk.
            const auto marker = gdb.field_raw(field.value, "MarkerToMoveTo", 7);
            if (marker && anchor_pos(*marker, beat.move_to)) beat.has_move = true;
            beat.arrive_range = gdb.field_float(field.value, "Range").value_or(1.0f);
            beat.wait_until_complete =
                gdb.field_raw(field.value, "WaitUntilComplete", gdb::kTypeBool).value_or(0u) != 0;
        } else if (beat.kind == "PlayAnimation") {
            if (const char* anim = gdb.field_string(field.value, "AnimationName")) {
                beat.animation = anim;
                beat.wait_until_complete =
                    gdb.field_raw(field.value, "PlayIntoAndOutof", gdb::kTypeBool).value_or(0u) != 0;
            }
        } else if (beat.kind == "SetLookAtCamera") {
            const auto p = gdb.field_raw(field.value, "PositionEntity", 7);
            const auto fo = gdb.field_raw(field.value, "FocusEntity", 7);
            if (p && fo && anchor_pos(*p, beat.cam_pos) && anchor_pos(*fo, beat.cam_focus))
                beat.has_camera = true;
        }
        out.push_back(std::move(beat));
    }
    return out;
}

// Drive the running cutscenes. Each holds the scene for its beat's authored duration, then plays
// the next; when the beats run out the cutscene posts its finish message (see the ICFS note in
// native_game.h) and retires.
void NativeGame::update_cutscenes(double dt) {
    if (cutscenes.empty()) return;
    constexpr int kIcfs = ('I' << 24) | ('C' << 16) | ('F' << 8) | 'S';

    for (auto& c : cutscenes) {
        c.timer -= dt;
        while (c.timer <= 0.0 && c.next < c.beats.size()) {
            const CutsceneBeat& beat = c.beats[c.next++];
            if (!beat.tag.empty()) {
                script_log.push_back("[say] " + beat.character + ": " + beat.text);
                spoken_lines.push_back({beat.character, beat.listener, beat.tag, beat.text});
            }
            if (beat.has_move || !beat.animation.empty()) {
                // Stage the action on the named character. Both are authored:
                //   MoveToMarker  -> a destination + arrival Range
                //   PlayAnimation -> a clip NAME, resolved to the bank clip id through the
                //                    character's OWN AnimationManagerComponent.Animations
                //                    (e.g. QC010_Rose has RoseWarmingUp, RoseTantrum, Idle, ...).
                StagedAction act;
                act.character = beat.character;
                act.animation = beat.animation;
                act.moving = beat.has_move;
                act.target = beat.move_to;

                std::uint64_t uid = 0;
                for (const auto& [id, nm] : entity_names)
                    if (nm == beat.character) { uid = id; break; }

                if (!beat.animation.empty() && uid != 0) {
                    const auto rec = entity_gdb_guid.find(uid);
                    if (rec != entity_gdb_guid.end()) {
                        if (const auto anims =
                                gdb.field_raw(rec->second, "AnimationManagerComponent",
                                              gdb::kTypeRecord)) {
                            if (const auto list =
                                    gdb.field_raw(*anims, "Animations", gdb::kTypeRecord)) {
                                // An animation entry comes in TWO authored shapes, both seen on
                                // QC010_Rose: either the clip key DIRECTLY (a type-4 field whose
                                // RAW u32 value is the bank key — see tools/gdb_anim_slots.py,
                                // which resolves the hero's Idle/Walk/Run the same way), or a
                                // type-6 sub-record of named slots, e.g. RoseWarmingUp ->
                                // { Pose, Idle }. Note the raw value is NOT a string-pool
                                // reference; formatting it as "0x...." is just how the dump tool
                                // prints an unpooled value.
                                if (const auto direct =
                                        gdb.field_raw(*list, beat.animation.c_str(),
                                                      gdb::kTypeString)) {
                                    act.clip = *direct;
                                } else if (const auto sub =
                                               gdb.field_raw(*list, beat.animation.c_str(),
                                                             gdb::kTypeRecord)) {
                                    for (const auto& slot : gdb.fields(*sub)) {
                                        if (slot.type != gdb::kTypeString) continue;
                                        act.clip = slot.value;
                                        break;
                                    }
                                }
                            }
                        }
                    }
                }
                if (beat.has_move && uid != 0) {
                    // Walk the character to the authored marker. FLAGGED: this sets the transform
                    // directly rather than pathing — the nav mesh is not consumed here — so the
                    // move is a straight line at the locomotion speed the cooked clips measured.
                    if (NativeEntity* e = world.entities.find(uid)) {
                        if (auto* t = e->get<TransformComponent>(kTypeIdTransform))
                            t->position = beat.move_to;
                    }
                }
                script_log.push_back("[stage] " + act.character +
                                     (act.moving ? " move" : "") +
                                     (act.animation.empty() ? "" : " anim=" + act.animation));
                staged_actions.push_back(std::move(act));
            }
            if (beat.has_camera) {
                // The authored cutscene camera: sit at PositionEntity, look at FocusEntity.
                camera.position = beat.cam_pos;
                const float dx = beat.cam_focus[0] - beat.cam_pos[0];
                const float dy = beat.cam_focus[1] - beat.cam_pos[1];
                const float dz = beat.cam_focus[2] - beat.cam_pos[2];
                camera.yaw = std::atan2(dx, dz);
                const float flat = std::sqrt(dx * dx + dz * dz);
                camera.pitch = (flat > 1e-4f) ? std::atan2(dy, flat) : 0.0f;
                camera_scripted = true;   // the follow-cam yields while a cutscene frames the shot
            } else if (beat.kind == "ClearCamera") {
                camera_scripted = false;
            }
            c.timer += beat.duration > 0.0 ? beat.duration : c.element_delay;
        }
    }

    for (const auto& c : cutscenes) {
        if (c.next >= c.beats.size() && c.timer <= 0.0)
            messages.post(kIcfs, c.entity, c.entity, static_cast<double>(gdb_token_for(c.record_id)));
    }
    cutscenes.erase(std::remove_if(cutscenes.begin(), cutscenes.end(),
                                   [](const PendingCutscene& c) {
                                       return c.next >= c.beats.size() && c.timer <= 0.0;
                                   }),
                    cutscenes.end());
}

int NativeGame::perform_cutscene(std::uint32_t record_id) {
    if (record_id == 0) return 0;
    const auto elements = gdb.field_raw(record_id, "SceneElements", gdb::kTypeRecord);
    {   // Trace every performance attempt: which record, and whether it had a beat list at all.
        char buf[96];
        std::snprintf(buf, sizeof(buf), "[cutscene-perform] record 0x%08X elements=%s", record_id,
                      elements ? "yes" : "NONE");
        script_log.push_back(buf);
    }
    if (!elements) return 0;

    int performed = 0;
    for (const auto& beat : gdb.fields(*elements)) {
        if (beat.type != gdb::kTypeRecord) continue;
        const char* kind = gdb.intern(beat.name_hash);
        if (!kind || std::string_view(kind) == "parent") continue;
        if (std::string_view(kind) != "SayLine") continue;   // other beat kinds: not staged yet

        const char* tag = gdb.field_string(beat.value, "TextTag");
        if (!tag || !*tag) continue;
        const char* who = gdb.field_string(beat.value, "Character");
        const char* to = gdb.field_string(beat.value, "CharacterToTalkTo");
        SpokenLine line;
        line.speaker = who ? who : "";
        line.listener = to ? to : "";
        line.tag = tag;
        line.text = text.get(tag);   // the tag itself when the table has no such string
        script_log.push_back("[say] " + line.speaker + ": " + line.text);
        spoken_lines.push_back(std::move(line));
        ++performed;
    }
    return performed;
}

int NativeGame::load_named_entities(const std::filesystem::path& path) {
    std::ifstream f(path);
    if (!f) return 0;
    std::string line;
    if (!std::getline(f, line) || line.rfind("F2NAMES", 0) != 0) return 0;  // magic gate
    int seeded = 0;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        // name \t x \t y \t z \t yaw [\t kind] — names cannot contain a tab, so the split is
        // exact. The trailing `kind` column (marker | entity) is optional: a "marker" is placed
        // and carries a real position, an "entity" is declared in the level's registry with no
        // position and is placed by script. Both become named entities here; the distinction is
        // recorded in the cook for the reader, not acted on yet.
        std::array<std::size_t, 4> tab{};
        std::size_t at = 0;
        bool ok = true;
        for (std::size_t i = 0; i < tab.size(); ++i) {
            at = line.find('\t', at);
            if (at == std::string::npos) { ok = false; break; }
            tab[i] = at++;
        }
        if (!ok) continue;
        const std::size_t yaw_end = std::min(line.find('\t', tab[3] + 1), line.size());
        // Optional trailing columns: `kind`, then the entity's authored GDB record id. The id is
        // what lets a runtime beat reach the entity's own data (e.g. a cutscene PlayAnimation
        // resolving its clip through AnimationManagerComponent.Animations).
        std::uint32_t row_guid = 0;
        if (yaw_end < line.size()) {
            const std::size_t kind_end = std::min(line.find('\t', yaw_end + 1), line.size());
            if (kind_end < line.size()) {
                try {
                    row_guid = static_cast<std::uint32_t>(
                        std::stoul(line.substr(kind_end + 1), nullptr, 0));
                } catch (const std::exception&) {
                    row_guid = 0;
                }
            }
        }
        const std::string name = line.substr(0, tab[0]);
        if (name.empty()) continue;
        float v[4];
        try {
            for (std::size_t i = 0; i < 4; ++i) {
                const std::size_t beg = tab[i] + 1;
                const std::size_t end = (i + 1 < tab.size()) ? tab[i + 1] : yaw_end;
                v[i] = std::stof(line.substr(beg, end - beg));
            }
        } catch (const std::exception&) {
            continue;   // a malformed row is skipped, not fatal
        }
        // OVERRIDE, don't duplicate. A name already present is MOVED rather than added again, so
        // loading a second .f2names on top of the cooked one is a clean override — which is how a
        // mod adjusts or adds named entities (drop a small .f2names listing only what it changes).
        // Duplicating instead would silently break the game's own lookups: StartNewEntityThread
        // spawns one thread PER MATCHING ENTITY (questmanager.lua:942), so a duplicated name would
        // run a quest's branch twice.
        std::uint64_t uid = 0;
        for (const auto& [existing_uid, existing_name] : entity_names) {
            if (existing_name == name) { uid = existing_uid; break; }
        }
        if (uid != 0) {
            NativeEntity* existing = world.entities.find(uid);
            if (TransformComponent* tf =
                    existing ? existing->get<TransformComponent>(kTypeIdTransform) : nullptr) {
                tf->position = {v[0], v[1], v[2]};
                tf->rotation = {0.0f, v[3], 0.0f};
            }
            if (row_guid != 0) entity_gdb_guid[uid] = row_guid;
        } else {
            NativeEntity& e = world.entities.create_entity();
            auto tf = std::make_unique<TransformComponent>();
            tf->position = {v[0], v[1], v[2]};
            tf->rotation = {0.0f, v[3], 0.0f};  // yaw about world up
            e.add_component(std::move(tf));
            entity_names[e.uid] = name;
            // Remember the entity's AUTHORED record so a beat can reach its own data later
            // (PlayAnimation resolves its clip through AnimationManagerComponent.Animations).
            if (row_guid != 0) entity_gdb_guid[e.uid] = row_guid;
        }
        ++seeded;
    }
    return seeded;
}

bool NativeGame::load_hero_anim_package(const std::filesystem::path& path) {
    HeroAnimData d = load_hero_anim(path.string());
    if (!d.ok) return false;
    hero_clips = std::move(d.clips);
    hero_bind = std::move(d.geom_bind);
    hero_locomotion.clear();
    for (std::size_t i = 0; i < hero_clips.size(); ++i)
        hero_locomotion.push_back({&hero_clips[i], d.root_speeds[i]});
    // Data-backed move speed: drive the hero at the cooked locomotion clips' OWN measured root
    // speed (min positive = walk, max = run) so the feet track the ground at playback rate 1.0
    // (retail hero speed = anim root motion). No hardcoded literal — it follows the actual clips.
    float wmin = 1e9f, wmax = 0.0f;
    for (float rs : d.root_speeds)
        if (rs > 0.05f) { wmin = std::min(wmin, rs); wmax = std::max(wmax, rs); }
    if (wmax > 0.0f) {
        player.config.walk_speed = wmin;
        player.config.run_speed = wmax;
    }
    return true;
}

void NativeGame::tick(double delta_seconds) {
    delta_seconds = std::clamp(delta_seconds, 0.0, 0.25);

    // Input is sampled ONCE per frame, before the fixed-step accumulator: retail
    // input_pad_poll (@0x822B3A10) polls per frame, not per simulation sub-step.
    // external_input lets a headless driver (tests/tools) set `input` directly.
    if (!external_input) input_sampler.tick(input);

    constexpr double simulation_step = 1.0 / 60.0;
    simulation_accumulator_ += delta_seconds;
    while (simulation_accumulator_ >= simulation_step) {
        simulation_accumulator_ -= simulation_step;
        elapsed_seconds += simulation_step;

        // Master gameplay tick order. Only the script sub-order (Quest->General
        // ->AI, Function_82281FE0) is decomp-confirmed today; movement/camera
        // steps join in P2. The front-end always ticks (menus/loading overlays).
        frontend.tick(simulation_step);
        if (mode == GameMode::InWorld) {
            // Drive cutscenes BEFORE the scripts run, so a thread polling
            // CheckForInteractiveCutsceneFinished sees the finish message on its next resume.
            // Beats now play over TIME (authored Wait.TimeToWait / ElementDelayInSeconds, and a
            // flagged stand-in for a spoken line's voice-over length) and the camera follows the
            // authored SetLookAtCamera anchors — see build_cutscene_beats.
            update_cutscenes(simulation_step);

            script_systems.tick(simulation_step);

            // Entity/brain: tick the live NPCs (ACT layer) against the hero as target/
            // LOD centre — before movement, so the follow camera later frames the moved
            // hero. Uses last frame's hero position (1-frame lag, consistent).
            world.update_npcs(collision, player.position(),
                              static_cast<float>(simulation_step));

            // Melee: X swings toward the aim (camera yaw). One swing per press (edge).
            if (input.pressed(PadButton::X)) {
                world.melee_attack(collision, player.position(), camera_controller.yaw);
            }

            // Movement: camera-relative input -> desired velocity -> collide-and-slide.
            player.update(collision, input, camera_controller.yaw,
                          static_cast<float>(simulation_step));

            // Skeletal animation: pick the locomotion clip whose root speed matches the hero's
            // planar speed and advance the player. Only (re)set the clip when it CHANGES —
            // set_clip resets the playback time, so calling it every step would pin the clip to
            // frame 0 and freeze the animation. FLAGGED: no-op bind pose until the cook emits
            // hero_locomotion clips (select returns nullptr on empty; update guards a null clip).
            float anim_rate = 1.0f;
            if (!hero_locomotion.empty()) {
                const float sp = player.planar_speed();
                const AnimClip* want = select_locomotion_clip(sp, hero_locomotion);
                if (want != hero_anim.clip()) hero_anim.set_clip(want);
                // Scale playback so the clip's foot speed ~= ground speed (anim_runtime_sampler
                // §C: avoid foot-slide). rate = ground_speed / clip_root_speed; idle (root ~0)
                // plays at 1x. Clamped so a large speed/clip mismatch can't look frantic/frozen.
                for (const LocomotionClip& lc : hero_locomotion)
                    if (lc.clip == want && lc.root_speed > 0.01f)
                        anim_rate = std::clamp(sp / lc.root_speed, 0.25f, 4.0f);
            }
            hero_anim.update(static_cast<float>(simulation_step) * anim_rate);
            // Publish the hero heading into its entity transform (rotation[1]=yaw). The visible
            // facing forward to the renderer is Stage 3 (avoids double-rotation vs the baked yaw).
            if (NativeEntity* he = world.entities.find(hero_uid))
                if (auto* t = he->get<TransformComponent>(kTypeIdTransform))
                    t->rotation[1] = player.facing_yaw();

            // Camera follows the moved hero (look from right-stick or mouse) — unless a script
            // has taken direct control of the camera pose (camera_scripted), in which case the
            // follow-cam yields so it does not stomp the scripted pose.
            if (camera_controller.mode == CameraMode::Follow && !camera_scripted) {
                const bool look_is_mouse = input.last_active_device == InputDevice::KeyboardMouse;
                camera_controller.update(player.position(), input.look, look_is_mouse,
                                         static_cast<float>(simulation_step), &collision);
                // Feed the follow pose into NativeCamera (the renderer reads pos/yaw/pitch).
                camera.position = camera_controller.position;
                camera.yaw = camera_controller.yaw;
                camera.pitch = camera_controller.pitch;
            }

            // Final step: push live entity transforms into the render scene.
            world.sync_to_scene(scene);
        }
    }
}

}  // namespace f2
