#include "f2/native_game.h"

#include "f2/native_bindings.h"

#include <algorithm>
#include <cmath>

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
        "  if p then Navigation.__MoveTo(e, p.x, p.y, p.z, r, s) end return true end",
        "=stage2_control");

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
    // The new-game handoff: prime QuestManager.HeroEntity, run Gameflow:Init (populates the
    // DebugQuestStartTable), flip GameflowMode on, and release the GAMEFLOW_START gate. After
    // this, the InWorld tick's QuestManager.Update advances the gameflow into QC010_Childhood.
    script_vm->run_source(
        "QuestManager.HeroEntity = GetPlayerHero(); "
        "if Gameflow then "
        "  pcall(function() Gameflow:Init() end); "
        "  Gameflow.GameflowMode = true; Gameflow._Initialised = true; "
        "  Gameflow.LoadedFromSave = false; Gameflow.SkipToNextPositionInGameflow = true; "
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
            // planar speed and advance the player. FLAGGED: no-op bind pose until the cook emits
            // hero_locomotion clips (select returns nullptr on empty; update guards a null clip).
            if (!hero_locomotion.empty())
                hero_anim.set_clip(select_locomotion_clip(player.planar_speed(), hero_locomotion));
            hero_anim.update(static_cast<float>(simulation_step));
            // Publish the hero heading into its entity transform (rotation[1]=yaw). The visible
            // facing forward to the renderer is Stage 3 (avoids double-rotation vs the baked yaw).
            if (NativeEntity* he = world.entities.find(hero_uid))
                if (auto* t = he->get<TransformComponent>(kTypeIdTransform))
                    t->rotation[1] = player.facing_yaw();

            // Camera follows the moved hero (look from right-stick or mouse).
            if (camera_controller.mode == CameraMode::Follow) {
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
