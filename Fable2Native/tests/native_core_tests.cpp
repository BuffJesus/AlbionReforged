#include "f2/native_audio.h"
#include "f2/native_font.h"
#include "f2/native_frontend_config.h"
#include "f2/native_game.h"
#include "f2/native_input_state.h"
#include "f2/native_script_systems.h"
#include "f2/native_entity.h"
#include "f2/native_world.h"
#include "f2/native_gdb_hash.h"
#include "f2/native_gdb.h"
#include "f2/native_mod_menu.h"
#include "f2/native_text.h"
#include "f2/native_physics.h"
#include "f2/native_camera.h"
#include "f2/native_player.h"
#include "f2/native_animation.h"
#include "f2/native_save.h"
#include "f2/native_npc.h"
#include "f2/native_script.h"
#include "f2/native_bnk.h"
#include "f2/native_hero_anim.h"
#include "f2/native_install.h"
#include "f2/native_scene.h"
#include "f2/native_texture.h"
#include "f2/native_ui.h"
#include "f2/render/null_render_backend.h"
#include "f2/render/texture_registry.h"
#include "f2/render/ui_draw_list.h"

#include <windows.h>

#include <cassert>
#include <cmath>
#include <algorithm>
#include <memory>
#include <vector>
#include <array>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <iostream>
#include <string>
#include <cstdio>
#include <cstdlib>

// NDEBUG-independent check for the boot test below. The project builds RelWithDebInfo,
// which defines NDEBUG and strips assert() — so the boot test can't rely on assert to
// actually verify anything. F2_CHECK aborts with a message on failure in every config.
// (NOTE for maintainers: the same NDEBUG stripping means the assert()s elsewhere in this
// file are no-ops under RelWithDebInfo — the suite currently only proves "doesn't crash".
// Making the whole suite live-assert is a worthwhile follow-up but touches many blocks'
// stale expectations, so it's deliberately left out of this scripting change.)
#define F2_CHECK(cond)                                                                 \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            std::fprintf(stderr, "F2_CHECK failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
            std::abort();                                                              \
        }                                                                              \
    } while (0)

// The full retail boot chain through the LIVE VM: generalsetupscript RUNS, its RunScript
// calls pull the gameplay scripts from the BNK and run each. Proves the whole path holds
// together — BNK inflate + float LuaQ undump + auto-stub catching every missing native —
// without a crash. (This RUNS bytecode, unlike the load_only proof.) Skipped if game data
// is absent. In its own function so the large NativeGame local stays out of main()'s frame.
static void test_boot_game_scripts() {
    const std::filesystem::path bnk_path =
        "D:/Documents/Fable2RE/Fable2Recomp/assets/game/data/gamescripts_r.bnk";
    // boot_game_scripts opens <data_root>/data/gamescripts_r.bnk, so data_root is the
    // parent of the "data" folder.
    const std::filesystem::path data_root = bnk_path.parent_path().parent_path();
    if (!std::filesystem::exists(bnk_path)) return;

    f2::NativeGame game;
    F2_CHECK(game.enable_scripting());
    // generalsetupscript RUNS and its RunScript list pulls the gameplay scripts from the BNK.
    // Getting here at all proves the whole chain: BNK inflate + the 32-bit/float LuaQ undump
    // patches (lundump.c LoadSize + luaconf LUA_NUMBER=float) + the auto-stub swallowing every
    // native the port doesn't implement yet.
    const int loaded = game.boot_game_scripts(data_root);
    F2_CHECK(loaded > 0);  // generalsetupscript's RunScript list pulled real scripts
    F2_CHECK(game.loaded_scripts.size() == static_cast<std::size_t>(loaded));
    // The managers resume the deferred boot coroutines for a few Update ticks. This is where
    // scripts actually CALL natives, so it exercises the auto-stub — and must not crash.
    for (int i = 0; i < 3; ++i) game.script_systems.tick(1.0 / 60.0);

    // Ranked missing-native worklist — the tangible STEP 4 artifact (what the port still owes
    // the game's scripts). Informational (may be empty if coroutines yield before calling an
    // unmet native), printed once so a maintainer can see the surface at a glance.
    std::map<std::string, int> freq;
    for (const auto& m : game.script_vm->stub_misses()) ++freq[m];
    std::vector<std::pair<std::string, int>> ranked(freq.begin(), freq.end());
    std::sort(ranked.begin(), ranked.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    std::cout << "[boot] scripts loaded = " << loaded
              << "  unique missing natives = " << ranked.size() << "\n";
    int shown = 0;
    for (const auto& [name, count] : ranked) {
        std::cout << "  " << count << "x  " << name << "\n";
        if (++shown >= 40) break;
    }
    std::cout << std::flush;
}

// ---- P6: run one of the game's OWN quests to completion on the native substrate ----
// Loads the real quest bank + the shipped example quest (quests/myfirstquest.lua), then
// drives it through the REAL QuestManager: the quest's actual Update coroutine runs, yields
// at its WaitFor, and — once its completion condition is set — resumes and prints its own
// "Terminating quest now". Proves the game's quest VM + scheduler + coroutine model execute
// its bytecode end-to-end. Skipped if game data is absent.
//
// Two of the quest's dependencies are neutralized as FLAGGED stand-ins for subsystems not
// wired yet (they are orthogonal to the quest's own logic): StartNewEntityThread (needs the
// world entity-search/streaming system) and the save/permanents registration (needs the
// save subsystem). The quest's Update/WaitFor/completion is 100% the game's real code.
static void test_run_real_quest() {
    const std::filesystem::path bnk_path =
        "D:/Documents/Fable2RE/Fable2Recomp/assets/game/data/gamescripts_r.bnk";
    const std::filesystem::path data_root = bnk_path.parent_path().parent_path();
    if (!std::filesystem::exists(bnk_path)) return;

    f2::NativeGame game;
    F2_CHECK(game.enable_scripting());
    F2_CHECK(game.boot_game_scripts(data_root) > 0);   // 159 misc scripts + real managers
    F2_CHECK(game.load_quest_scripts() > 0);           // QuestManager + quest modules + gameflow
    f2::NativeScriptVM& vm = *game.script_vm;

    // Load the shipped example quest; it self-registers MyFirstQuest via NewQuestThread and
    // defines its Update. (StartNewEntityThread stubbed first — see the note above.)
    vm.run_source("QuestThreadBase.StartNewEntityThread = function() end");
    F2_CHECK(vm.run_source("RunScript('quests/myfirstquest.lua')"));
    game.script_log.clear();
    F2_CHECK(vm.run_source("__f2ok = type(MyFirstQuest.Update) == 'function'; assert(__f2ok)"));

    // Instantiate + schedule the quest through the REAL QuestManager, then drive it.
    F2_CHECK(vm.run_source("__q = MyFirstQuest:new(); QuestManager.AddQuestThread(__q)"));
    vm.run_source("QuestManager.Update()");              // Update runs -> yields at WaitFor
    // WaitFor's predicate reads the quest TYPE's QuestOver; set it, then resume to completion.
    vm.run_source("MyFirstQuest.QuestOver = true");
    vm.run_source("QuestManager.Update()");              // WaitFor exits -> prints + ends

    bool completed = false;
    for (const auto& line : game.script_log)
        if (line.find("Terminating quest now") != std::string::npos) completed = true;
    F2_CHECK(completed);  // the game's quest ran its real logic to its own completion print
}

// ---- P7: the game's REAL entity threads spawn + interact on the native substrate ----
// MyFirstQuest calls StartNewEntityThread("QuestGiver", VillagerWithQuest); with real
// SearchTools over the world's named entities, that spawns a REAL VillagerWithQuest entity
// thread bound to the QuestGiver entity. Running its OnInteract executes the quest's own
// dialogue. Proves entity search + entity-thread spawning + the interaction callback — the
// layer above bare quest coroutines. Skipped if game data is absent.
static void test_entity_thread_spawns() {
    const std::filesystem::path bnk_path =
        "D:/Documents/Fable2RE/Fable2Recomp/assets/game/data/gamescripts_r.bnk";
    const std::filesystem::path data_root = bnk_path.parent_path().parent_path();
    if (!std::filesystem::exists(bnk_path)) return;
    f2::NativeGame game;
    F2_CHECK(game.enable_scripting());
    F2_CHECK(game.boot_game_scripts(data_root) > 0);
    F2_CHECK(game.load_quest_scripts() > 0);
    f2::NativeScriptVM& vm = *game.script_vm;

    // Spawn the named world entities the quest searches for, then load + start MyFirstQuest.
    vm.run_source("QuestGiverEnt = Debug.CreateEntityAt('Villager','QuestGiver',0,0,0)");
    vm.run_source("EvilTwinEnt   = Debug.CreateEntityAt('Bandit','EvilTwin',0,0,0)");
    F2_CHECK(vm.run_source("RunScript('quests/myfirstquest.lua')"));
    vm.run_source("__q = MyFirstQuest:new(); QuestManager.AddQuestThread(__q)");
    for (int i = 0; i < 2; ++i) vm.run_source("QuestManager.Update()");  // Update -> StartNewEntityThread

    // Real VillagerWithQuest + EnemyToKill entity threads, bound to the named entities, now
    // exist — spawned by the quest's own StartNewEntityThread via real SearchTools.
    vm.run_source(
        "local vt = QuestManager.EntitiesWithQuestThread[GetIDFromEntity(QuestGiverEnt)]; "
        "assert(vt ~= nil and vt.Entity ~= nil and type(vt.OnInteract) == 'function')");
    F2_CHECK(vm.last_error().empty());

    // Interact THROUGH the real QuestManager.Update: post an INTERACTED_WITH message to the
    // QuestGiver, tick, and the entity thread's Update coroutine polls it and runs the quest's
    // own OnInteract dialogue. (This is the manager-driven path — it exercises the entity
    // thread's real coroutine, not a direct call.)
    game.script_log.clear();
    vm.run_source("MessageEvents.PostMessage(EMessageEventType.MESSAGE_EVENT_INTERACTED_WITH,0,nil,QuestGiverEnt)");
    for (int i = 0; i < 2; ++i) vm.run_source("QuestManager.Update()");
    bool talked = false;
    for (const auto& line : game.script_log)
        if (line.find("kill this evil twin") != std::string::npos) talked = true;
    F2_CHECK(talked);  // the spawned entity thread ran the game's dialogue via the message poll

    // Kill the evil twin: mark it dead + post KILLED. The manager sees IsAlive() false and
    // terminates the EnemyToKill thread; its OnTerminated sees the KILLED message and sets
    // MyFirstQuest.KilledTwin — proving the kill -> OnTerminated -> quest-state chain.
    vm.run_source("EvilTwinEnt:Kill(); MessageEvents.PostMessage(EMessageEventType.MESSAGE_EVENT_KILLED,0,nil,EvilTwinEnt)");
    for (int i = 0; i < 3; ++i) vm.run_source("QuestManager.Update()");
    vm.run_source("assert(MyFirstQuest.KilledTwin == true)");
    F2_CHECK(vm.last_error().empty());  // kill detection propagated to quest state
}

// ---- P8: NEW GAME -> the self-starting gameflow reaches the childhood chapter ----
// The whole start sequence runs on the substrate: start_new_game() releases the gameflow's
// GAMEFLOW_START gate, then QuestManager.Update advances GAMEFLOW_START -> DebugQC010 and
// StartQuest loads QC010_Childhood FROM THE BNK (via the require-loader) and runs it.
//
// ⚠ "QC010_Childhood Starting" is a WEAK assertion and is kept only as a precondition: the game
// prints it in GameflowBase:StartQuest AFTER `Gameflow[name] = quest_type:new()` but BEFORE the
// quest's coroutine is ever resumed. It proves require + class resolution + :new(), nothing more —
// it passed on every run for the whole period the childhood was dying on its first frame. The
// load-bearing assertions are the Gameflow:Init branch + WeaponNames checks below.
// Skipped if game data is absent.
static void test_gameflow_starts_childhood() {
    const std::filesystem::path bnk_path =
        "D:/Documents/Fable2RE/Fable2Recomp/assets/game/data/gamescripts_r.bnk";
    const std::filesystem::path data_root = bnk_path.parent_path().parent_path();
    if (!std::filesystem::exists(bnk_path)) return;
    f2::NativeGame game;
    F2_CHECK(game.enable_scripting());
    F2_CHECK(game.boot_game_scripts(data_root) > 0);
    F2_CHECK(game.load_quest_scripts() > 0);
    f2::NativeScriptVM& vm = *game.script_vm;

    game.start_new_game(/*female=*/false);   // the new-game handoff

    // REGRESSION GUARD for the GameflowMode ordering bug. Gameflow:Init branches on
    // Gameflow.GameflowMode and prints which way it went (the BNK ships this file's original
    // source as scripts/quests/gameflow.txt: line 2250 `if not Gameflow.GameflowMode then`,
    // 2251 print("Gameflow INACTIVE"), 2289 print("Gameflow ACTIVE")). Only the ACTIVE branch
    // builds the state the childhood needs — e.g. Gameflow.WeaponNames at 2383-2385. If the port
    // sets the flag after calling Init, Init takes INACTIVE, WeaponNames is nil, and QC010's
    // Update dies on its first frame. Assert the branch, and assert the data it must produce.
    bool active = false, inactive = false;
    for (const auto& line : game.script_log) {
        if (line.find("Gameflow ACTIVE") != std::string::npos) active = true;
        if (line.find("Gameflow INACTIVE") != std::string::npos) inactive = true;
    }
    F2_CHECK(active && !inactive);
    vm.run_source("assert(Gameflow.WeaponNames ~= nil and "
                  "Gameflow.WeaponNames.ChildMelee == 'ChildSwordWooden')", "=t_weapons");
    F2_CHECK(vm.last_error().empty());  // gameflow.txt:2383-2384, verbatim from the game's data

    game.script_log.clear();
    // Drive the gameflow: QuestManager.Update resumes Gameflow.Update, which advances past the
    // GAMEFLOW_START gate into DebugQC010 and StartQuest("QC010_Childhood").
    for (int i = 0; i < 8; ++i) vm.run_source("QuestManager.Update()");

    bool started = false;
    for (const auto& line : game.script_log)
        if (line.find("QC010_Childhood Starting") != std::string::npos) started = true;
    F2_CHECK(started);  // the new-game gameflow loaded + started the real childhood quest

    // The gameflow position advanced past the initial GAMEFLOW_START gate.
    vm.run_source("assert(Gameflow.PositionInGameflow ~= ScriptEnum.GAMEFLOW_START)");
    F2_CHECK(vm.last_error().empty());
}

// Verifies the offline script cooker + runtime consumption: cook every entry into a package
// (cook_scripts.py's layout) and prove BnkReader::open_cooked reads it byte-identically to the
// raw BNK, and that the full runtime boots + reaches the childhood gameflow from the cooked
// package with NO raw .bnk present. (docs/NATIVE_PORT_PLAN.md "consume the native package".)
static void test_cook_scripts_package() {
    const std::filesystem::path bnk_path =
        "D:/Documents/Fable2RE/Fable2Recomp/assets/game/data/gamescripts_r.bnk";
    if (!std::filesystem::exists(bnk_path)) return;

    // 1. Cook: open the raw BNK and write each decompressed entry to <pkg>/<name>. Since
    //    BnkReader::extract mirrors cook_scripts.py's decompression, this C++ cook is
    //    byte-identical to the shipped tool's output.
    f2::BnkReader raw;
    F2_CHECK(raw.open(bnk_path.string()));
    const auto temp = std::filesystem::temp_directory_path() / "f2native_cook_test";
    std::error_code ec;
    std::filesystem::remove_all(temp, ec);
    const auto pkg = temp / "cooked";  // boot_game_scripts reads <data_root>/cooked/scripts
    std::size_t lua_count = 0;
    for (const auto& name : raw.names()) {
        if (name.size() < 4 || name.compare(name.size() - 4, 4, ".lua") != 0) continue;  // .lua only
        ++lua_count;
        std::vector<std::uint8_t> data = raw.extract(name);
        F2_CHECK(!data.empty());
        std::string rel = name;  // "scripts\..." normalized
        std::replace(rel.begin(), rel.end(), '\\', '/');
        const auto out = pkg / rel;
        std::filesystem::create_directories(out.parent_path(), ec);
        std::ofstream f(out, std::ios::binary);
        f.write(reinterpret_cast<const char*>(data.data()),
                static_cast<std::streamsize>(data.size()));
    }

    // 2. open_cooked indexes the package and extracts byte-identically to the raw BNK.
    f2::BnkReader cooked;
    F2_CHECK(cooked.open_cooked((pkg / "scripts").string()));
    F2_CHECK(cooked.entry_count() == lua_count);
    for (const char* probe : {"quests/qc010_childhood.lua",
                              "miscellaneous/generalsetupscript.lua", "quests/questmanager.lua"}) {
        const std::vector<std::uint8_t> a = raw.extract(probe);
        const std::vector<std::uint8_t> b = cooked.extract(probe);
        F2_CHECK(!b.empty() && a == b);  // cooked package == raw decompression
    }

    // 3. The full runtime consumes the cooked package (no raw .bnk under <temp>/data): boot the
    //    game scripts + drive the gameflow to the childhood quest, all from the cooked files.
    f2::NativeGame game;
    F2_CHECK(game.enable_scripting());
    F2_CHECK(game.boot_game_scripts(temp) > 0);
    F2_CHECK(game.load_quest_scripts() > 0);
    game.start_new_game(/*female=*/false);
    game.script_log.clear();
    for (int i = 0; i < 8; ++i) game.script_vm->run_source("QuestManager.Update()");
    bool started = false;
    for (const auto& line : game.script_log)
        if (line.find("QC010_Childhood Starting") != std::string::npos) started = true;
    F2_CHECK(started);  // gameflow reached childhood entirely from the cooked package

    std::filesystem::remove_all(temp, ec);
}

// Named quest entities: a cooked `.f2names` sidecar (tools/cook_quest_markers.py) seeds the
// world's named markers, and a quest's GetEntityWithName path (SearchTools StartNewSearch ->
// FilterWithName -> GetSearchResults over entity_names) resolves them to a handle whose
// GetPosition returns the cooked world position. Self-contained: writes its own sidecar, so it
// runs without game data.
static void test_named_entity_sidecar() {
    const auto dir = std::filesystem::temp_directory_path() / "f2native_names";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const auto file = dir / "test.f2names";
    {
        std::ofstream f(file);
        f << "F2NAMES 1\n"
          << "# name\tx\ty\tz\tyaw\n"
          << "QC010_HeroInCrowdMarker\t175.0739\t49.4009\t144.7233\t-3.1416\n"
          << "QC010_RoseInCrowdMarker\t173.9961\t49.4009\t144.9737\t-0.0000\n"
          << "malformed row with no tabs\n";   // skipped, not fatal
    }

    f2::NativeGame game;
    F2_CHECK(game.enable_scripting());
    F2_CHECK(game.load_named_entities(file) == 2);          // the malformed row is dropped
    F2_CHECK(game.load_named_entities(dir / "nope.f2names") == 0);  // absent = 0, not a crash

    // MOD OVERRIDE: a second .f2names listing only what it changes MOVES the existing entity
    // rather than adding a duplicate. Duplicating would silently break the game's own lookups —
    // StartNewEntityThread spawns one thread PER MATCHING ENTITY (questmanager.lua:942), so a
    // duplicated name would run a quest branch twice.
    const auto override_file = dir / "mod.f2names";
    {
        std::ofstream f(override_file);
        f << "F2NAMES 1\n"
          << "QC010_HeroInCrowdMarker\t1.0\t2.0\t3.0\t0.5\tmarker\n"
          << "ModAddedMarker\t9.0\t9.0\t9.0\t0.0\tmarker\n";
    }
    F2_CHECK(game.load_named_entities(override_file) == 2);
    std::size_t named_hero_markers = 0;
    for (const auto& [uid, nm] : game.entity_names)
        if (nm == "QC010_HeroInCrowdMarker") ++named_hero_markers;
    F2_CHECK(named_hero_markers == 1);   // moved, not duplicated
    bool mod_added = false;
    for (const auto& [uid, nm] : game.entity_names)
        if (nm == "ModAddedMarker") mod_added = true;
    F2_CHECK(mod_added);                 // a mod can also ADD names

    // SearchTools lives in register_game_systems_api, which boot_game_scripts installs — so the
    // script round-trip below needs the user's script bank. Without it, the C++ seeding above is
    // still verified and the rest is skipped.
    const std::filesystem::path bnk_path =
        "D:/Documents/Fable2RE/Fable2Recomp/assets/game/data/gamescripts_r.bnk";
    if (!std::filesystem::exists(bnk_path)) {
        std::filesystem::remove_all(dir, ec);
        return;
    }
    // The seeding above survives the boot (entity_names is game state, not script state), so the
    // search below runs against the markers already loaded — not a second copy of them.
    F2_CHECK(game.boot_game_scripts(bnk_path.parent_path().parent_path()) > 0);

    // The full search path a quest uses, through the real natives.
    F2_CHECK(game.script_vm->run_source(
        "local h = SearchTools.StartNewSearch(); "
        "SearchTools.FilterWithName(h, 'QC010_HeroInCrowdMarker'); "
        "local r = SearchTools.GetSearchResults(h); "
        "assert(r and #r == 1, 'marker not found'); "
        // GetPosition returns a CVector3 (the retail contract — see the binding's citation).
        // The mod override above moved this marker, so assert the OVERRIDDEN position — that is
        // the contract: the last .f2names loaded wins, in place.
        "local p = r[1]:GetPosition(); "
        "assert(math.abs(p:GetX() - 1.0) < 0.01 and math.abs(p:GetY() - 2.0) < 0.01 and "
        "       math.abs(p:GetZ() - 3.0) < 0.01, 'override not applied'); "
        "local x, y, z = r[1]:GetPositionXYZ(); "
        "assert(math.abs(x - 1.0) < 0.01 and math.abs(z - 3.0) < 0.01, 'wrong xyz'); "
        "assert(r[1]:GetName() == 'QC010_HeroInCrowdMarker')",
        "=t_names") || (std::fprintf(stderr, "names: %s\n",
                        game.script_vm->last_error().c_str()), false));
    F2_CHECK(game.script_vm->last_error().empty());
    std::filesystem::remove_all(dir, ec);
}

// The game's localised text: TextTag -> the real in-game string, through the real native.
// The strings below are the game's own words, decoded from the user's book.babel by
// tools/babel_text.py; the runtime ships no text. Skipped if the cooked package is absent.
static void test_game_text() {
    const std::filesystem::path data_root =
        "D:/Documents/Fable2RE/Fable2Recomp/assets/game";
    f2::TextTable tt;
    if (!tt.load(data_root / "cooked/en-uk.f2text")) return;
    F2_CHECK(tt.size() > 10000);                       // the shipped en-uk table is 56,000 strings
    F2_CHECK(std::string(tt.get("TEXT_LEVEL_FAIRFAX_CASTLE")) == "Castle Fairfax");
    F2_CHECK(std::string(tt.get("TEXT_CHARACTER_NAME_THERESA")) == "Theresa");
    // A cutscene's own dialogue line, reached by the TextTag its SayLine beat carries.
    F2_CHECK(std::string(tt.get("TEXT_QUEST_QC010_JEEVES_GREET_02"))
                 .find("Jeeves") != std::string::npos);
    // An unknown tag returns the TAG, never empty — a missing string stays visible.
    F2_CHECK(std::string(tt.get("TEXT_NO_SUCH_TAG_AT_ALL")) == "TEXT_NO_SUCH_TAG_AT_ALL");

    const std::filesystem::path bnk =
        "D:/Documents/Fable2RE/Fable2Recomp/assets/game/data/gamescripts_r.bnk";
    if (!std::filesystem::exists(bnk)) return;
    f2::NativeGame game;
    F2_CHECK(game.enable_scripting());
    F2_CHECK(game.boot_game_scripts(data_root) > 0);   // also loads the text package
    F2_CHECK(game.text.valid());
    F2_CHECK(game.script_vm->run_source(
        "assert(GetText('TEXT_LEVEL_FAIRFAX_CASTLE') == 'Castle Fairfax'); "
        "assert(Text.GetText('TEXT_CHARACTER_NAME_THERESA') == 'Theresa'); "
        "assert(GetText('TEXT_NOPE') == 'TEXT_NOPE')",
        "=t_text"));
    F2_CHECK(game.script_vm->last_error().empty());
}

// Performing a cutscene's authored beats: SceneElements -> SayLine -> the game's real dialogue.
// This is the whole chain end to end — GDB name table -> record -> enumerate beats -> TextTag ->
// localised text — on a real childhood cutscene. Skipped if game data is absent.
static void test_cutscene_saylines() {
    const std::filesystem::path data_root = "D:/Documents/Fable2RE/Fable2Recomp/assets/game";
    if (!std::filesystem::exists(data_root / "data/gamescripts_r.bnk")) return;

    f2::NativeGame game;
    F2_CHECK(game.enable_scripting());
    F2_CHECK(game.boot_game_scripts(data_root) > 0);
    const auto guid = game.gdb.guid_for_name("QC010_JeevesGreet");
    if (!guid) return;                       // no cutscene data on this install

    const int said = game.perform_cutscene(*guid);
    F2_CHECK(said >= 3);                     // that cutscene is a multi-line exchange
    F2_CHECK(game.spoken_lines.size() == static_cast<std::size_t>(said));

    // The first beat, in the game's own words.
    const auto& first = game.spoken_lines.front();
    F2_CHECK(first.speaker == "QC010_EscortGuardFairfax");
    F2_CHECK(first.listener == "QC010_Jeeves");
    F2_CHECK(first.tag == "TEXT_QUEST_QC010_JEEVES_GREET_02");
    if (game.text.valid())
        F2_CHECK(first.text.find("Jeeves") != std::string::npos);   // "Evening, Jeeves. ..."
    std::cout << "[cutscene] QC010_JeevesGreet spoke " << said << " lines; first = "
              << first.speaker << ": " << first.text << "\n";
}

// The mod / debug-jump menu, built by REFLECTION over the game's own tables (native_mod_menu.h).
// Asserts the three sources it discovers and that a failing action REPORTS rather than swallows.
// The game-data half is skipped when the script bank is absent; the mod half always runs.
static void test_mod_menu() {
    const std::filesystem::path bnk_path =
        "D:/Documents/Fable2RE/Fable2Recomp/assets/game/data/gamescripts_r.bnk";
    const std::filesystem::path data_root = bnk_path.parent_path().parent_path();

    f2::NativeGame game;
    F2_CHECK(game.enable_scripting());
    f2::ModMenu menu;

    if (std::filesystem::exists(bnk_path)) {
        F2_CHECK(game.boot_game_scripts(data_root) > 0);
        F2_CHECK(game.load_quest_scripts() > 0);   // quest modules define the ChildhoodVars skips
        game.start_new_game(/*female=*/false);     // Gameflow:Init fills DebugQuestStartTable
        // The ChildhoodVars.SkipTo* functions are defined by qc010_childhood.lua, which the
        // gameflow only require()s when it starts that quest — so drive a few ticks first. This
        // is also the honest behaviour of the menu: it lists what is actually loaded right now.
        for (int i = 0; i < 8; ++i) game.script_vm->run_source("QuestManager.Update()", "=t_mm");
        F2_CHECK(menu.rebuild(*game.script_vm) > 0);

        // 1. The game's OWN skip functions, discovered — not hardcoded here.
        //    (qc010_childhood.lua:174-237 defines SkipToWino/SkipToLL/SkipToLL2/
        //     SkipToBuyMusicBox/SkipToLuciensStudy on Gameflow.ChildhoodVars.)
        int skips = 0, quests = 0;
        bool has_lucien = false, has_childhood = false;
        for (const auto& e : menu.entries()) {
            if (e.category == "Skip") {
                ++skips;
                if (e.label == "SkipToLuciensStudy") has_lucien = true;
            } else if (e.category == "Quest") {
                ++quests;
                // The label is the GAME'S OWN name, resolved through its own data:
                // Quest_QC010_Childhood -> NameTag 'TEXT_QUEST_QC010_NAME' -> "Childhood".
                if (e.label == "Childhood") {
                    has_childhood = true;
                    // RegisterDebugQuest recorded the level + start marker (gameflow.lua:274-281),
                    // and the description comes from the same record's DescriptionTag.
                    F2_CHECK(e.detail.find("BWSSlums") != std::string::npos);
                    F2_CHECK(e.detail.find("QC010_ChildhoodStart") != std::string::npos);
                    F2_CHECK(e.detail.find("Bowerstone Old Town") != std::string::npos);
                }
            }
        }
        std::cout << "[modmenu] " << menu.entries().size() << " entries: " << skips
                  << " skips, " << quests << " jumpable quests\n";
        int shown = 0;
        for (const auto& e : menu.entries()) {
            if (e.category != "Skip" && e.category != "Quest") continue;
            std::cout << "  [" << e.category << "] " << e.label << "  (" << e.detail << ")\n";
            if (++shown >= 8) break;
        }
        F2_CHECK(skips >= 5 && has_lucien);       // the childhood's own skip family
        F2_CHECK(quests > 10 && has_childhood);   // the gameflow's jumpable-quest table

        // The shipped CHOICE entries (registered through the public API, so they also serve as a
        // mod template) write the game's own gameflow variables.
        std::size_t evil_choice = menu.entries().size();
        for (std::size_t i = 0; i < menu.entries().size(); ++i)
            if (menu.entries()[i].category == "Choice" &&
                menu.entries()[i].label.find("Arfur") != std::string::npos) evil_choice = i;
        F2_CHECK(evil_choice < menu.entries().size());
        F2_CHECK(menu.invoke(*game.script_vm, evil_choice));
        F2_CHECK(game.script_vm->run_source(
            "assert(Gameflow.ChildhoodResolutionEvil == true); "
            "assert(Gameflow.ChildhoodVars.WantedCompleted == true)", "=t_choice"));
        F2_CHECK(game.script_vm->last_error().empty());
    }

    // 2. A mod extends the menu from Lua, with no C++ change.
    F2_CHECK(game.script_vm->run_source(
        "__f2_modmenu_ran = false "
        "ModMenu.Register('Mod', 'Test Entry', function() __f2_modmenu_ran = true end, 'detail')",
        "=t_modreg"));
    F2_CHECK(menu.rebuild(*game.script_vm) > 0);
    std::size_t mod_index = menu.entries().size();
    for (std::size_t i = 0; i < menu.entries().size(); ++i)
        if (menu.entries()[i].label == "Test Entry") mod_index = i;
    F2_CHECK(mod_index < menu.entries().size());
    F2_CHECK(menu.invoke(*game.script_vm, mod_index));
    F2_CHECK(game.script_vm->run_source("assert(__f2_modmenu_ran == true)", "=t_modran"));
    F2_CHECK(game.script_vm->last_error().empty());

    // 3. A failing action REPORTS its error instead of failing silently.
    F2_CHECK(game.script_vm->run_source(
        "ModMenu.Register('Mod', 'Boom', function() error('kaboom') end)", "=t_modboom"));
    F2_CHECK(menu.rebuild(*game.script_vm) > 0);
    std::size_t boom = menu.entries().size();
    for (std::size_t i = 0; i < menu.entries().size(); ++i)
        if (menu.entries()[i].label == "Boom") boom = i;
    F2_CHECK(boom < menu.entries().size());
    F2_CHECK(!menu.invoke(*game.script_vm, boom));
    F2_CHECK(menu.last_error().find("kaboom") != std::string::npos);
    F2_CHECK(!menu.invoke(*game.script_vm, 99999));   // out of range is reported, not a crash
}

// GDB name resolution through the real natives: the two-step lookup the game uses.
// A record key is an opaque GUID, and each .gdb carries a NAME TABLE of {fnv1(name), GUID} that
// maps a script's string onto it (see native_gdb.h). This asserts the exact chain
// QuestEntityThreadBase.PlayCutscene walks (questmanager.lua:2103):
//     GDB.RecordExists(name) -> GDB.GetRecord(name) -> rec:GetFloat("MaxRangeFromPlayer")
// on a real childhood cutscene, plus a negative control. Skipped if game data is absent.
static void test_gdb_record_lookup() {
    const std::filesystem::path bnk_path =
        "D:/Documents/Fable2RE/Fable2Recomp/assets/game/data/gamescripts_r.bnk";
    const std::filesystem::path data_root = bnk_path.parent_path().parent_path();
    if (!std::filesystem::exists(bnk_path)) return;

    // The C++ reader directly: name -> GUID -> field, independent of Lua.
    f2::gdb::GdbFile ics;
    if (ics.open(data_root / "data/interactivecutscenes/interactivecutscenes.gdb")) {
        const auto guid = ics.guid_for_name("QC010_SetRoseMode");
        F2_CHECK(guid.has_value() && *guid == 0x94FA26B1u);   // verified byte-level
        const auto range = ics.field_float(*guid, f2::gdb::fnv1("MaxRangeFromPlayer"));
        F2_CHECK(range.has_value() && std::fabs(*range - 10.0f) < 1e-4f);
        F2_CHECK(!ics.guid_for_name("NoSuchCutsceneName").has_value());  // negative control
    }

    f2::NativeGame game;
    F2_CHECK(game.enable_scripting());
    F2_CHECK(game.boot_game_scripts(data_root) > 0);   // opens globals + interactivecutscenes
    F2_CHECK(game.gdb.file_count() >= 1);

    // ...and through the natives, exactly as the quest script calls them.
    F2_CHECK(game.script_vm->run_source(
        "assert(GDB.RecordExists('QC010_SetRoseMode'), 'cutscene record missing'); "
        "local rec = GDB.GetRecord('QC010_SetRoseMode'); "
        "assert(rec, 'GetRecord returned nil'); "
        "local r = rec:GetFloat('MaxRangeFromPlayer'); "
        "assert(r and math.abs(r - 10) < 0.001, 'MaxRangeFromPlayer wrong: ' .. tostring(r)); "
        // A name that is not a record must read as false/nil, not error — the game branches on it
        // and calls Debug.Error itself when a cutscene is missing.
        "assert(GDB.RecordExists('NoSuchCutsceneName') == false); "
        "assert(GDB.GetRecord('NoSuchCutsceneName') == nil)",
        "=t_gdb"));
    F2_CHECK(game.script_vm->last_error().empty());
}

// Phase 0 of the childhood recreation (docs/CHILDHOOD_RECREATION_HANDOFF.md): the EMPIRICAL
// stub-miss census. Boots the real script stack, starts a new game so the gameflow enters
// QC010_Childhood, drives the retail Quest->General->AI tick for a few simulated seconds, and
// ranks the missing natives BY CALL COUNT (stub_call_counts(), not the once-per-name
// stub_misses() surface). The ranking is the ordering signal for which natives to implement
// first; it is written to docs/childhood_stub_census.txt so the plan cites measurements, not
// the script-grep estimate. Informational (no assertions on the contents — the point is to
// MEASURE); skipped if game data is absent.
static void test_childhood_stub_census() {
    const std::filesystem::path bnk_path =
        "D:/Documents/Fable2RE/Fable2Recomp/assets/game/data/gamescripts_r.bnk";
    const std::filesystem::path data_root = bnk_path.parent_path().parent_path();
    if (!std::filesystem::exists(bnk_path)) return;

    f2::NativeGame game;
    F2_CHECK(game.enable_scripting());

    // OPTIONAL cooked world. Without a scene, entity searches come back empty and every
    // world-gated beat parks forever, so the census is a lower bound on the opening beats. Point
    // FABLE2NATIVE_CENSUS_SCENE at a cooked chapter2slums .f2scene to measure with the world live.
    // (Cooked scenes are large build artifacts, not in the repo, hence an env var and not a path.)
    std::string scene_note = "no scene loaded (set FABLE2NATIVE_CENSUS_SCENE to a cooked .f2scene)";
    if (const char* scene_env = std::getenv("FABLE2NATIVE_CENSUS_SCENE")) {
        const std::filesystem::path scene_path = scene_env;
        std::string scene_err;
        if (!std::filesystem::exists(scene_path)) {
            scene_note = "scene MISSING: " + scene_path.string();
        } else if (!game.load_scene(scene_path, scene_err)) {
            scene_note = "scene FAILED to load (" + scene_err + "): " + scene_path.string();
        } else {
            game.mode = f2::GameMode::InWorld;   // gameplay systems only tick InWorld
            game.external_input = true;          // headless: no device sampling
            scene_note = "scene loaded: " + scene_path.filename().string() + " (" +
                         std::to_string(game.scene.instances.size()) + " instances)";
        }
        std::cout << "[census] " << scene_note << "\n";
    }

    // OPTIONAL named markers (tools/cook_quest_markers.py). Without them GetEntityWithName finds
    // nothing and the world-gated beats cannot advance no matter what else is implemented.
    if (const char* names_env = std::getenv("FABLE2NATIVE_CENSUS_NAMES")) {
        const int seeded = game.load_named_entities(names_env);
        scene_note += "; named entities seeded: " + std::to_string(seeded);
        std::cout << "[census] named entities seeded: " << seeded << "\n";
    }

    F2_CHECK(game.boot_game_scripts(data_root) > 0);
    F2_CHECK(game.load_quest_scripts() > 0);
    f2::NativeScriptVM& vm = *game.script_vm;

    // NOTE ordering: every probe below is installed BEFORE start_new_game. A quest instance
    // snapshots its base-class methods when it is created, so wrapping QuestThreadBase after the
    // childhood already exists leaves that instance holding the UNWRAPPED originals — which is
    // why an earlier pass saw waits from QC070_Thag (created later) but none from QC010.
    // Ignore everything the BOOT hit: we want what the CHILDHOOD costs, not setup noise.
    const std::map<std::string, int> boot_baseline = vm.stub_call_counts();

    // TIMELINE probe: wrap __stub_call so each native's FIRST call is stamped with the frame it
    // happened on and routed through the real Debug.Log native (script_log). The frame at which
    // new names stop appearing is where the quest STALLS - the thing the ranking alone can't say.
    vm.run_source(
        // __f2_last also records the most recent stubbed native globally, so the resume-error
        // handler can name what the dying coroutine touched just before it raised.
        "__f2_frame = 0; __f2_last = '(none)'; local seen = {}; local orig = __stub_call; "
        "__stub_call = function(k) orig(k); __f2_last = k; "
        "if not seen[k] then seen[k] = true; "
        "Debug.Log('[first] ' .. __f2_frame .. ' ' .. k) end end",
        "=census_probe");
    F2_CHECK(vm.last_error().empty());

    // ERROR probe: the script managers resume their coroutines with coroutine.resume and
    // DISCARD the (false, err) result, so a quest that dies mid-beat dies SILENTLY — which is
    // exactly what "the sequence advances while nothing shows" looks like. Wrap resume so every
    // failure is reported. (Retail bytecode is stripped, so there are no line numbers; the
    // message text is the signal.)
    vm.run_source(
        "local raw = coroutine.resume\n"
        "coroutine.resume = function(co, ...)\n"
        "  local a, b = raw(co, ...)\n"
        // debug.traceback(co, msg) walks the DEAD coroutine's own stack. The bytecode is stripped
        // so there are no line numbers, but each frame still carries its chunk name (the port
        // names chunks after the script), which is what attributes an error to a script.
        "  if a == false then\n"
        "    local tb = debug.traceback(co, tostring(b)) or tostring(b)\n"
        "    Debug.Log('[resume-error] frame=' .. tostring(__f2_frame) ..\n"
        "             ' lastStub=' .. tostring(__f2_last) .. ' :: ' ..\n"
        "             string.gsub(tb, '\\n', ' | ')) end\n"
        "  return a, b\n"
        "end",
        "=census_resume");
    F2_CHECK(vm.last_error().empty());

    // WAIT probe: the quest threads park inside QuestThreadBase's wait primitives
    // (questmanager.lua sets WaitFor / WaitForMessage / WaitForTriggerToFire / ... on it). Wrap
    // them so each entry is logged with the calling thread and, for WaitFor, the predicate's
    // chunk. The LAST wait a thread enters and never leaves IS the gate — named, not inferred.
    // The wrapper is transparent: it forwards self + all args and propagates all returns.
    vm.run_source(
        "local names = {'WaitFor','WaitForMessage','WaitForTriggerToFire','WaitForQuestToFinish',"
        "'WaitForMyCutsceneToFinish','WaitForInteractiveCutsceneToFinish',"
        "'WaitForTimeInSeconds','WaitForCurrentActionToFinish','WaitUntilStarted','Wait',"
        // PlayCutscene is the childhood's real pacing primitive (135 call sites in
        // qc010_childhood.lua, each `self:PlayCutscene{Cutscene=..., UntilCondition=fn}`). It is a
        // SCRIPT method, not a native, so it never shows up in the stub census however long it
        // blocks — it has to be probed by name.
        "'PlayCutscene','PlayFullCutscene','PlayInteractiveCutscene'}\n"
        // Both bases: a quest's own thread derives from QuestThreadBase, but its sub-threads
        // (the ones that actually drive the childhood's beats) derive from QuestEntityThreadBase,
        // so wrapping only the former left every entity-thread wait invisible.
        "local bases = {QuestThreadBase, QuestEntityThreadBase}\n"
        "for _, base in ipairs(bases) do\n"
        "for _, n in ipairs(names) do\n"
        "  local f = base and rawget(base, n)\n"
        "  if type(f) == 'function' then\n"
        "    base[n] = function(self, a, ...)\n"
        "      local extra = ''\n"
        "      if type(a) == 'function' then\n"
        // linedefined survives stripping, so it identifies the predicate exactly (map it back
        // with tools/find_proto_by_line.py).
        "        local i = debug.getinfo(a, 'S')\n"
        "        extra = ' pred@' .. tostring(i and i.short_src) ..\n"
        "                ':' .. tostring(i and i.linedefined)\n"
        "      elseif type(a) == 'table' then extra = ' cutscene=' .. tostring(rawget(a,'Cutscene'))\n"
        "      elseif a ~= nil then extra = ' arg=' .. tostring(a) end\n"
        "      local who = '?'\n"
        "      if type(self) == 'table' then who = tostring(self.QuestName or self.Name or '?') end\n"
        "      Debug.Log('[wait] ' .. __f2_frame .. ' ' .. who .. ' :' .. n .. extra)\n"
        // Log the RETURN too: an entry with no matching exit is a primitive that never came back,
        // which is exactly what a parked beat looks like from the outside.
        "      local r1, r2 = f(self, a, ...)\n"
        "      Debug.Log('[wait] ' .. __f2_frame .. ' ' .. who .. ' :' .. n .. ' RETURNED')\n"
        "      return r1, r2\n"
        "    end\n"
        "  end\n"
        "end\n"
        "end",
        "=census_wait");
    F2_CHECK(vm.last_error().empty());

    // ENTITY-THREAD probe. A quest's beats live in entity threads, and
    // QuestThreadBase.StartNewEntityThread (questmanager.lua:942) spawns ONE PER ENTITY MATCHING
    // THE NAME: GetAllEntitiesWithName(name) -> for each match, ThreadClass:new(entity, ...) ->
    // StartNewThread. So a name that resolves to nothing silently starts NO thread and that
    // quest's whole branch never runs — with no error anywhere. Log the match count per name.
    vm.run_source(
        "local f = QuestThreadBase and rawget(QuestThreadBase, 'StartNewEntityThread')\n"
        "if type(f) == 'function' then\n"
        "  QuestThreadBase.StartNewEntityThread = function(self, name, cls, ...)\n"
        "    local n = 0\n"
        "    if type(name) == 'string' and self and self.GetAllEntitiesWithName then\n"
        "      local ok, list = pcall(function() return self:GetAllEntitiesWithName(name) end)\n"
        "      if ok and type(list) == 'table' then n = #list end\n"
        "    end\n"
        "    Debug.Log('[ethread] ' .. tostring(name) .. ' matched=' .. n)\n"
        "    return f(self, name, cls, ...)\n"
        "  end\n"
        "end",
        "=census_ethread");
    F2_CHECK(vm.last_error().empty());

    // DIAGNOSTIC ARM (opt-in, FABLE2NATIVE_CENSUS_SKIP_CUTSCENES=1): make PlayCutscene return
    // immediately instead of blocking. Every PlayCutscene currently parks forever — no cutscene
    // system exists to end it — which hides every beat BEHIND the first one. Returning at once is
    // NOT faithful (no camera, no dialogue, no timing, and any beat whose state the cutscene was
    // supposed to change is skipped), so this exists purely to MEASURE what the rest of the quest
    // then asks for. It is never on by default.
    if (const char* skip = std::getenv("FABLE2NATIVE_CENSUS_SKIP_CUTSCENES");
        skip && *skip == '1') {
        vm.run_source(
            "for _, base in ipairs({QuestThreadBase, QuestEntityThreadBase}) do\n"
            "  for _, n in ipairs({'PlayCutscene','PlayFullCutscene','PlayInteractiveCutscene'}) do\n"
            "    if base and type(rawget(base, n)) == 'function' then\n"
            "      base[n] = function(self, a)\n"
            "        Debug.Log('[wait] ' .. __f2_frame .. ' ' .. \n"
            "                 tostring(type(self)=='table' and (self.QuestName or self.Name) or '?')\n"
            "                 .. ' :' .. n .. ' SKIPPED cutscene=' ..\n"
            "                 tostring(type(a)=='table' and rawget(a,'Cutscene') or a))\n"
            "        return true\n"
            "      end\n"
            "    end\n"
            "  end\n"
            "end",
            "=census_skipcut");
        F2_CHECK(vm.last_error().empty());
        scene_note += "; CUTSCENES SKIPPED (diagnostic arm)";
        std::cout << "[census] DIAGNOSTIC: PlayCutscene returns immediately\n";
    }

    // With every probe live, hand off to the new game: the gameflow now enters QC010_Childhood
    // with wrapped wait primitives, so its own beats are observable.
    game.start_new_game(/*female=*/false);

    // The game prints its own branch marker inside Gameflow:Init ("Gameflow ACTIVE" / "Gameflow
    // INACTIVE", gameflow.txt:2251/2289 — the BNK ships that file's original source). Capture it
    // BEFORE clearing the log: which branch Init took decides whether the childhood can run at
    // all, and it is the one-line regression guard for the GameflowMode ordering bug.
    std::string init_branch = "(neither marker printed)";
    for (const auto& line : game.script_log) {
        if (line.find("Gameflow ACTIVE") != std::string::npos) init_branch = "ACTIVE";
        else if (line.find("Gameflow INACTIVE") != std::string::npos) init_branch = "INACTIVE (BUG)";
    }
    std::cout << "[census] Gameflow:Init branch = " << init_branch << "\n";
    game.script_log.clear();

    // Drive the real tick. QuestManager.Update resumes Gameflow -> QC010_Childhood's own
    // coroutine; script_systems.tick also resumes the General + AI managers (retail order),
    // so entity/AI-side natives the quest leans on are counted too.
    // 10 s at 60 Hz by default; FABLE2NATIVE_CENSUS_FRAMES raises the horizon for a long
    // diagnostic run (e.g. paired with SKIP_CUTSCENES to enumerate how far the quest would go).
    int kFrames = 600;
    if (const char* fr = std::getenv("FABLE2NATIVE_CENSUS_FRAMES")) {
        const int v = std::atoi(fr);
        if (v > 0 && v <= 200000) kFrames = v;
    }
    const int kTailWindow = 60;  // the last second: what the stalled quest is SPINNING on
    std::map<std::string, int> tail_baseline;
    for (int i = 0; i < kFrames; ++i) {
        if (i == kFrames - kTailWindow) tail_baseline = vm.stub_call_counts();
        vm.run_source(("__f2_frame = " + std::to_string(i)).c_str(), "=census_frame");
        vm.run_source("QuestManager.Update()", "=census");
        if (game.mode == f2::GameMode::InWorld) {
            game.tick(1.0 / 60.0);          // full frame: scripts + NPCs + player + camera
        } else {
            game.script_systems.tick(1.0 / 60.0);  // no world: scripts only
        }
    }

    // STALL REPORT: the ranking says WHAT is missing, the timeline says WHEN it stops, and this
    // says WHERE the quest coroutine is parked — walk every live quest thread and report the Lua
    // source:line each suspended coroutine yielded at. That line IS the gate to open next.
    vm.run_source(
        "local seen = {}\n"
        "local function scan(t, path, depth)\n"
        "  if depth > 9 or seen[t] then return end\n"
        "  seen[t] = true\n"
        "  for k, v in pairs(t) do\n"
        "    local tv = type(v)\n"
        "    if tv == 'thread' then\n"
        "      local where = '?'\n"
        // currentline is stripped from retail bytecode, but linedefined is NOT — so record the
        // whole frame stack as linedefined values. That names the exact function each coroutine is
        // parked in; map them back with tools/find_proto_by_line.py.
        "      local frames = {}\n"
        "      for lvl = 0, 8 do\n"
        "        local i = debug.getinfo(v, lvl, 'Sn')\n"
        "        if not i then break end\n"
        "        frames[#frames + 1] = tostring(i.name or '?') .. '@' .. tostring(i.linedefined)\n"
        "      end\n"
        "      if #frames > 0 then where = table.concat(frames, '<-') end\n"
        "      local id = tostring(rawget(t, 'QuestName') or rawget(t, 'Name') or '?')\n"
        "      local st = tostring(rawget(t, 'CurrentState'))\n"
        "      Debug.Log('[stall] ' .. path .. '.' .. tostring(k) .. ' status=' ..\n"
        "               coroutine.status(v) .. ' quest=' .. id .. ' state=' .. st ..\n"
        "               ' at ' .. where)\n"
        "    elseif tv == 'table' and type(k) ~= 'userdata' then\n"
        "      scan(v, path .. '.' .. tostring(k), depth + 1)\n"
        "    end\n"
        "  end\n"
        "end\n"
        "scan(_G, '_G', 0)\n"
        // Direct interrogation of the childhood itself: the auto-scan reports coroutines by
        // container path, which does not say WHICH quest a thread belongs to when the thread is
        // held under a generic key (Gameflow.ChildThreads.N). Find every table that identifies as
        // QC010 and report its state.
        "local function probe(t, path, depth, seen)\n"
        "  if depth > 9 or seen[t] then return end\n"
        "  seen[t] = true\n"
        "  for k, v in pairs(t) do\n"
        "    if type(v) == 'table' then\n"
        "      local id = rawget(v, 'QuestName') or rawget(v, 'Name')\n"
        "      if type(id) == 'string' and id:find('QC010') then\n"
        "        local co = rawget(v, 'co_update')\n"
        "        Debug.Log('[stall] QC010 OBJECT at ' .. path .. '.' .. tostring(k) ..\n"
        "                 ' id=' .. id .. ' state=' .. tostring(rawget(v, 'CurrentState')) ..\n"
        "                 ' co=' .. (co and coroutine.status(co) or 'none') ..\n"
        "                 ' update=' .. type(rawget(v, 'Update')))\n"
        "      end\n"
        "      probe(v, path .. '.' .. tostring(k), depth + 1, seen)\n"
        "    end\n"
        "  end\n"
        "end\n"
        "probe(_G, '_G', 0, {})\n"
        // The direct, authoritative checks. The gameflow keys a running quest by its GAMEFLOW
        // name (Gameflow.Childhood), and a quest instance identifies via `_Name`, not `QuestName`
        // — so the generic scan above cannot see it. Ask for it by name instead.
        "local c = Gameflow and rawget(Gameflow, 'Childhood')\n"
        "Debug.Log('[stall] Gameflow.Childhood = ' .. type(c) ..\n"
        "         (type(c) == 'table' and (' _Name=' .. tostring(rawget(c, '_Name')) ..\n"
        "          ' co=' .. (rawget(c, 'co_update') and coroutine.status(rawget(c,'co_update'))\n"
        "                     or 'none') ..\n"
        "          ' state=' .. tostring(rawget(c, 'CurrentState'))) or ''))\n"
        "Debug.Log('[stall] Gameflow.WeaponNames = ' .. type(Gameflow and Gameflow.WeaponNames))\n"
        "Debug.Log('[stall] scan done, QuestManager=' .. type(QuestManager) .. ' debug=' .. type(debug))",
        "=census_stall");
    if (!vm.last_error().empty())
        game.script_log.push_back("[stall] scan error: " + vm.last_error());

    // Rank by calls DURING the childhood (post-boot delta).
    std::vector<std::pair<std::string, int>> ranked;
    for (const auto& [name, count] : vm.stub_call_counts()) {
        const auto it = boot_baseline.find(name);
        const int delta = count - (it == boot_baseline.end() ? 0 : it->second);
        if (delta > 0) ranked.emplace_back(name, delta);
    }
    std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {
        return a.second != b.second ? a.second > b.second : a.first < b.first;
    });
    // Natives the childhood REFERENCED but never called (reads of a stubbed field, etc.) still
    // matter for the worklist, so record the reference-only tail separately.
    std::vector<std::string> referenced_only;
    for (const auto& name : vm.stub_misses()) {
        const auto it = vm.stub_call_counts().find(name);
        if (it == vm.stub_call_counts().end() || it->second == 0) referenced_only.push_back(name);
    }
    std::sort(referenced_only.begin(), referenced_only.end());
    referenced_only.erase(std::unique(referenced_only.begin(), referenced_only.end()),
                          referenced_only.end());

    const std::filesystem::path out =
        "D:/Documents/Fable2RE/docs/childhood_stub_census.txt";
    std::ofstream f(out);
    f << "# Childhood (QC010) stub-miss census - MEASURED, not estimated\n"
      << "# Source: f2native_core_tests test_childhood_stub_census (Phase 0 of\n"
      << "# docs/CHILDHOOD_RECREATION_HANDOFF.md). Boot: boot_game_scripts + load_quest_scripts\n"
      << "# + start_new_game(male), then " << kFrames << " frames of QuestManager.Update() +\n"
      << "# ScriptSystems::tick(1/60) (Quest->General->AI). Counts are the POST-BOOT delta, so\n"
      << "# setup-only natives are excluded.\n"
      << "# World: " << scene_note << "\n"
      << "# Gameflow:Init branch (the game's OWN print, gameflow.txt:2251/2289): "
      << init_branch << "\n"
      << "#   ACTIVE = the real new-game setup ran (WeaponNames, breadcrumbs, quest checkers).\n"
      << "#   INACTIVE = the debug/sandbox branch ran instead and the childhood dies frame 0.\n"
      << "# CAVEAT: with NO scene loaded, beats gated on world state (triggers, entity searches,\n"
      << "# streaming) park immediately - such a run is a LOWER BOUND on the quest's native\n"
      << "# surface, weighted toward the opening beats.\n#\n"
      << "# calls  native\n";
    for (const auto& [name, count] : ranked) f << count << "\t" << name << "\n";
    f << "\n# referenced but never called (" << referenced_only.size() << "):\n";
    for (const auto& name : referenced_only) f << "-\t" << name << "\n";

    // The timeline: first-call frame per native, in the order the quest reached them. The last
    // entry is the furthest beat this harness gets to; everything after it is the stall.
    // Silent coroutine deaths (a resumed quest/gameflow thread that raised) — ranked by
    // frequency. A dead gameflow/quest thread outranks any missing native: nothing downstream
    // of it can ever run.
    {
        std::map<std::string, int> errs;
        for (const auto& line : game.script_log) {
            const std::size_t tag = line.find("[resume-error] ");
            if (tag != std::string::npos) ++errs[line.substr(tag + 15)];
        }
        std::vector<std::pair<std::string, int>> ranked_errs(errs.begin(), errs.end());
        std::sort(ranked_errs.begin(), ranked_errs.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });
        f << "\n# SILENT coroutine errors (" << ranked_errs.size()
          << " distinct) - these kill beats outright:\n";
        for (const auto& [msg, count] : ranked_errs) f << "# " << count << "x  " << msg << "\n";
        std::cout << "[census] silent coroutine errors: " << ranked_errs.size() << " distinct\n";
        for (const auto& [msg, count] : ranked_errs)
            std::cout << "  " << count << "x  " << msg << "\n";
    }

    // What the STALLED quest is polling in the last second. Once the sequence has run as far as
    // the stubs allow, the natives still being hammered every frame ARE the gate: those are the
    // predicates a beat is waiting to turn true. Implementing one of these unblocks progress;
    // implementing something merely frequent overall does not.
    {
        std::vector<std::pair<std::string, int>> spin;
        for (const auto& [name, count] : vm.stub_call_counts()) {
            const auto it = tail_baseline.find(name);
            const int delta = count - (it == tail_baseline.end() ? 0 : it->second);
            if (delta > 0) spin.emplace_back(name, delta);
        }
        std::sort(spin.begin(), spin.end(), [](const auto& a, const auto& b) {
            return a.second != b.second ? a.second > b.second : a.first < b.first;
        });
        f << "\n# SPIN SET - calls in the last " << kTailWindow
          << " frames (per frame in brackets). These are the GATES:\n";
        for (const auto& [name, count] : spin)
            f << "# " << count << "\t[" << (count / kTailWindow) << "/frame]\t" << name << "\n";
        std::cout << "[census] spin set (last " << kTailWindow << " frames): " << spin.size()
                  << " natives\n";
        int shown = 0;
        for (const auto& [name, count] : spin) {
            std::cout << "  " << count << "x  " << name << "\n";
            if (++shown >= 12) break;
        }
    }

    // DIALOGUE actually spoken. Each line came from a cutscene's authored SayLine beat, with its
    // TextTag resolved through the game's own localised text — so this is the childhood speaking
    // in the game's words, not a stand-in.
    {
        f << "\n# DIALOGUE spoken (" << game.spoken_lines.size()
          << " lines) - speaker: line [tag]:\n";
        for (const auto& l : game.spoken_lines)
            f << "# " << l.speaker << ": " << l.text << "  [" << l.tag << "]\n";
        std::cout << "[census] dialogue spoken: " << game.spoken_lines.size() << " lines\n";
        int shown = 0;
        for (const auto& l : game.spoken_lines) {
            std::cout << "  " << l.speaker << ": " << l.text << "\n";
            if (++shown >= 6) break;
        }
    }

    // Entity threads: StartNewEntityThread spawns one per entity matching the name, so
    // `matched=0` means that branch of the quest silently never runs at all.
    {
        f << "\n# ENTITY THREADS started (name matched=N). matched=0 => that branch NEVER runs:\n";
        int zero = 0, total = 0;
        for (const auto& line : game.script_log) {
            const std::size_t tag = line.find("[ethread] ");
            if (tag == std::string::npos) continue;
            const std::string entry = line.substr(tag + 10);
            ++total;
            if (entry.find("matched=0") != std::string::npos) ++zero;
            f << "# " << entry << "\n";
        }
        std::cout << "[census] entity threads: " << total << " requested, " << zero
                  << " matched nothing\n";
    }

    // Every wait a quest thread ENTERED, in order. The tail is where each thread is stuck.
    {
        std::map<std::string, int> waits;
        f << "\n# WAITS entered (frame  thread  :primitive) - the tail entries are the GATES:\n";
        for (const auto& line : game.script_log) {
            const std::size_t tag = line.find("[wait] ");
            if (tag == std::string::npos) continue;
            const std::string entry = line.substr(tag + 7);
            if (++waits[entry] == 1) f << entry << "\n";   // first entry of each distinct wait
        }
        std::vector<std::pair<std::string, int>> ranked_waits(waits.begin(), waits.end());
        std::sort(ranked_waits.begin(), ranked_waits.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });
        f << "# re-entered most (a re-entered wait is polling; a once-entered wait is parked):\n";
        for (int i = 0; i < 12 && i < static_cast<int>(ranked_waits.size()); ++i)
            f << "# " << ranked_waits[i].second << "x  " << ranked_waits[i].first << "\n";
        std::cout << "[census] waits entered: " << waits.size() << " distinct\n";
    }

    f << "\n# where the quest coroutines are parked (the gate to open next):\n";
    for (const auto& line : game.script_log) {
        const std::size_t tag = line.find("[stall] ");
        if (tag != std::string::npos) f << "# " << line.substr(tag) << "\n";
    }

    f << "\n# timeline (frame  native, first call) - the tail is where the quest STALLS:\n";
    int timeline_rows = 0;
    for (const auto& line : game.script_log) {
        const std::size_t tag = line.find("[first] ");
        if (tag == std::string::npos) continue;
        f << line.substr(tag + 8) << "\n";
        ++timeline_rows;
    }
    f.flush();

    std::cout << "[census] childhood stub calls: " << ranked.size() << " distinct natives, "
              << referenced_only.size() << " reference-only, " << timeline_rows
              << " timeline rows -> " << out.string() << "\n";
    int shown = 0;
    for (const auto& [name, count] : ranked) {
        std::cout << "  " << count << "x  " << name << "\n";
        if (++shown >= 25) break;
    }
    std::cout << std::flush;
}

// Stage 2 (Slice 1): the Physics control natives the childhood scripts call — CVector3 vector
// math + teleport/facing/velocity wired to NativePlayer + the hero entity transform.
static void test_stage2_control() {
    const std::filesystem::path bnk_path =
        "D:/Documents/Fable2RE/Fable2Recomp/assets/game/data/gamescripts_r.bnk";
    const std::filesystem::path data_root = bnk_path.parent_path().parent_path();
    if (!std::filesystem::exists(bnk_path)) return;
    f2::NativeGame game;
    F2_CHECK(game.enable_scripting());
    F2_CHECK(game.boot_game_scripts(data_root) > 0);
    f2::NativeScriptVM& vm = *game.script_vm;

    // CVector3 is a real object with vector math (not the black-hole stub).
    vm.run_source(
        "local a=CVector3(3,0,4); assert(math.abs(a:GetLength()-5)<1e-4); "
        "assert(a:GetSquaredLength()==25); assert(a:GetX()==3 and a:GetZ()==4); "
        "local d=CVector3(3,0,4)-CVector3(0,0,1); assert(d:GetSquaredLength()==18)",
        "=t_vec");
    F2_CHECK(vm.last_error().empty());

    // Teleport moves BOTH the player and the hero entity transform (coherence).
    vm.run_source("Physics.TeleportToPosition(GetPlayerHero(), CVector3(10,0,5))", "=t_tp");
    F2_CHECK(vm.last_error().empty());
    F2_CHECK(std::fabs(game.player.position()[0] - 10.0f) < 1e-4f);
    F2_CHECK(std::fabs(game.player.position()[2] - 5.0f) < 1e-4f);
    // GetPosition yields a CVector3 (retail contract), so read the component off the vector.
    vm.run_source("assert(math.abs(GetPlayerHero():GetPosition():GetX()-10) < 1e-3)", "=t_tp2");
    F2_CHECK(vm.last_error().empty());

    // Facing: SetFacingVector -> player.facing_yaw = atan2(x,z); GetFacingVector round-trips.
    vm.run_source("Physics.SetFacingVector(GetPlayerHero(), CVector3(1,0,0))", "=t_face");
    F2_CHECK(std::fabs(game.player.facing_yaw() - std::atan2(1.0f, 0.0f)) < 1e-4f);
    vm.run_source(
        "local f=Physics.GetFacingVector(GetPlayerHero()); "
        "assert(math.abs(f:GetX()-1)<1e-3 and math.abs(f:GetZ())<1e-3)",
        "=t_face2");
    F2_CHECK(vm.last_error().empty());

    // Velocity readback: hero at rest -> ~0 squared length (feeds the follow-behaviour branch).
    vm.run_source("assert(Physics.GetVelocity(GetPlayerHero()):GetSquaredLength() < 1e-4)", "=t_vel");
    F2_CHECK(vm.last_error().empty());
}

// Stage 2 (Slice 2): Navigation.* scripted movement — nav goal motor + ENavigationSpeed enum.
static void test_stage2_navigation() {
    const std::filesystem::path bnk_path =
        "D:/Documents/Fable2RE/Fable2Recomp/assets/game/data/gamescripts_r.bnk";
    const std::filesystem::path data_root = bnk_path.parent_path().parent_path();
    if (!std::filesystem::exists(bnk_path)) return;
    f2::NativeGame game;
    F2_CHECK(game.enable_scripting());
    F2_CHECK(game.boot_game_scripts(data_root) > 0);
    F2_CHECK(game.load_quest_scripts() > 0);  // loads navigationspeedenum (ENavigationSpeed)
    f2::NativeScriptVM& vm = *game.script_vm;

    // ENavigationSpeed is a real enum table (not black-holed).
    vm.run_source("assert(ENavigationSpeed and ENavigationSpeed.NAV_SPEED_WALK == 2 "
                  "and ENavigationSpeed.NAV_SPEED_RUN == 5)", "=t_navenum");
    F2_CHECK(vm.last_error().empty());

    // Give a spawned NPC a nav goal via the public native; an agent is created on demand.
    vm.run_source(
        "__nav = Debug.CreateEntityAt('CreatureVillager','Bob', 0,0,0); "
        "Navigation.MoveToPosition(__nav, { position = CVector3(5,0,0), radius = 1, "
        "  speed = ENavigationSpeed.NAV_SPEED_RUN })", "=t_navgo");
    F2_CHECK(vm.last_error().empty());
    F2_CHECK(game.world.npcs.size() == 1 && game.world.npcs[0].has_goal);

    // Drive the motor directly (no gameflow) until it arrives at the goal.
    game.collision.build_from_scene(game.scene);  // empty scene: planar move only, no walls
    for (int i = 0; i < 400 && game.world.npcs[0].has_goal; ++i)
        game.world.update_npcs(game.collision, game.player.position(), 1.0f / 60.0f);
    F2_CHECK(!game.world.npcs[0].has_goal);  // arrived -> goal cleared
    F2_CHECK(std::fabs(game.world.npcs[0].controller.position()[0] - 5.0f) < 1.5f);
    vm.run_source("assert(Navigation.GetCurrentSpeed(__nav) < 0.5)", "=t_navspeed");  // stopped
    F2_CHECK(vm.last_error().empty());

    // StopMoving clears an active goal.
    vm.run_source("Navigation.MoveToPosition(__nav, {position=CVector3(20,0,0), radius=1}); "
                  "Navigation.StopMoving(__nav)", "=t_navstop");
    F2_CHECK(!game.world.npcs[0].has_goal);
}

// Stage 2 (Slice 3): the live locomotion AnimationPlayer, driven by the tick from hero speed.
static f2::AnimClip make_loco_clip(std::uint32_t hash) {
    f2::AnimClip c;
    c.hash = hash;
    c.bone_count = 1;
    c.frame_count = 40;  // ~1.33s @30fps: long enough that time() doesn't loop within the test
    c.fps = 30.0f;
    c.skin.assign(40, std::array<float, 12>{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0});  // identity 3x4
    return c;
}
static void test_stage2_animation() {
    // Synthetic idle/walk/run clips — the cook doesn't emit runtime clips yet, so this proves the
    // live driver + selection integration in tick() headlessly (measured hashes/speeds).
    f2::NativeGame game;
    game.hero_clips.push_back(make_loco_clip(0x1B78A889));  // idle
    game.hero_clips.push_back(make_loco_clip(0x02EE1AA7));  // walk
    game.hero_clips.push_back(make_loco_clip(0x8C7D7F7E));  // run
    game.hero_locomotion = {{&game.hero_clips[0], 0.0f},
                            {&game.hero_clips[1], 0.77f},
                            {&game.hero_clips[2], 4.20f}};
    game.mode = f2::GameMode::InWorld;
    game.external_input = true;
    game.collision.build_from_scene(game.scene);  // empty: planar move only

    // At rest the driver selects the idle clip (root speed 0).
    game.input = f2::InputState{};
    game.tick(1.0 / 60.0);
    F2_CHECK(game.hero_anim.clip() == &game.hero_clips[0]);

    // Moving forward: planar speed rises -> a faster (non-idle) clip; the player advances.
    for (int i = 0; i < 60; ++i) {
        game.input = f2::InputState{};
        game.input.move = {0.0f, 1.0f};
        game.tick(1.0 / 60.0);
    }
    F2_CHECK(game.hero_anim.clip() != nullptr && game.hero_anim.clip() != &game.hero_clips[0]);
    // The clip TIME must accumulate across steady ticks — a regression guard against re-setting
    // the clip every step (set_clip resets time, which would pin the animation to frame 0).
    F2_CHECK(game.hero_anim.time() > 0.1f);
    const float t_before = game.hero_anim.time();
    game.input = f2::InputState{};
    game.input.move = {0.0f, 1.0f};
    game.tick(1.0 / 60.0);
    F2_CHECK(game.hero_anim.time() > t_before);  // advances by ~one step, not reset to 0

    // Stub-flag safety: with NO clips the driver holds bind pose and does not crash.
    f2::NativeGame bare;
    bare.mode = f2::GameMode::InWorld;
    bare.external_input = true;
    bare.collision.build_from_scene(bare.scene);
    bare.tick(1.0 / 60.0);
    F2_CHECK(bare.hero_anim.clip() == nullptr);  // explicit bind-pose fallback
}

// Stage 2 (Slice 4): scripted camera pose override + the follow-cam authority gate.
static void test_stage2_camera() {
    const std::filesystem::path bnk_path =
        "D:/Documents/Fable2RE/Fable2Recomp/assets/game/data/gamescripts_r.bnk";
    const std::filesystem::path data_root = bnk_path.parent_path().parent_path();
    if (!std::filesystem::exists(bnk_path)) return;
    f2::NativeGame game;
    F2_CHECK(game.enable_scripting());
    F2_CHECK(game.boot_game_scripts(data_root) > 0);
    f2::NativeScriptVM& vm = *game.script_vm;

    // Drive only the camera portion of the tick (managers off, empty world).
    game.script_systems.quest.enabled = false;
    game.script_systems.general.enabled = false;
    game.script_systems.ai.enabled = false;
    game.mode = f2::GameMode::InWorld;
    game.external_input = true;
    game.collision.build_from_scene(game.scene);
    game.camera_controller.mode = f2::CameraMode::Follow;
    game.player.set_position({100.0f, 0.0f, 100.0f});  // far from any follow pose

    // Script takes the camera: MoveTo holds its pose, the follow-cam yields.
    vm.run_source("Camera.MoveTo(CVector3(0,5,0))", "=t_cammove");
    F2_CHECK(vm.last_error().empty());
    F2_CHECK(game.camera_scripted);
    F2_CHECK(std::fabs(game.camera.position[1] - 5.0f) < 1e-4f);
    game.tick(1.0 / 60.0);
    F2_CHECK(std::fabs(game.camera.position[1] - 5.0f) < 1e-4f);  // follow did NOT stomp it

    // Clearing the override returns authority to the follow-cam, which reframes off the hero.
    vm.run_source("CameraManager.ClearCameraOverride()", "=t_camclear");
    F2_CHECK(!game.camera_scripted);
    game.tick(1.0 / 60.0);
    F2_CHECK(game.camera.position[1] != 5.0f ||
             game.camera.position[0] != 0.0f);  // follow-cam moved the camera off the scripted pose
}

// Stage 3: the cooked hero skeletal-animation package + a BIT-EXACT runtime-convention check.
// Skinning the bind vertices with the idle clip at frame 0 via AnimationPlayer must reproduce the
// validated fable_pose reference pose (proves the cook's transposed 3x4 emit == the runtime skin
// convention). Skipped if the .heroanim isn't cooked (game data absent).
static void test_stage3_hero_anim() {
    const std::filesystem::path pkg =
        "D:/Documents/Fable2RE/Fable2Recomp/assets/game/cooked/hero.heroanim";
    if (!std::filesystem::exists(pkg)) return;
    f2::HeroAnimData d = f2::load_hero_anim(pkg.string());
    F2_CHECK(d.ok);
    F2_CHECK(d.bone_count == 143);
    // idle + data-backed walk + run (walk = FootstepWalk events; run = both-feet FOOT_PLANT
    // events; both straight forward root motion). clips[0]=idle, [1]=walk, [2]=run.
    F2_CHECK(d.clips.size() == 3);
    F2_CHECK(d.geom_bind.size() == d.ref_pose.size() && !d.geom_bind.empty());

    // Idle = clips[0] (root speed 0). Skin each geom at idle@0 and match the reference bit-exactly.
    f2::AnimationPlayer player;
    player.set_clip(&d.clips[0]);  // time 0 -> frame 0
    float max_err = 0.0f;
    for (std::size_t g = 0; g < d.geom_bind.size(); ++g) {
        std::vector<std::array<float, 3>> out;
        player.skin(d.geom_bind[g], out);
        F2_CHECK(out.size() == d.ref_pose[g].size());
        for (std::size_t v = 0; v < out.size(); ++v)
            for (int k = 0; k < 3; ++k) {
                const float e = std::fabs(out[v][k] - d.ref_pose[g][v][k]);
                if (e > max_err) max_err = e;
            }
    }
    F2_CHECK(max_err < 1e-3f);  // runtime skinner == validated fable_pose baker

    // The single idle clip is selected at every speed (until real walk/run clips are identified).
    std::vector<f2::LocomotionClip> loco;
    for (std::size_t i = 0; i < d.clips.size(); ++i)
        loco.push_back({&d.clips[i], d.root_speeds[i]});
    F2_CHECK(f2::select_locomotion_clip(0.0f, loco) == &d.clips[0]);            // idle
    F2_CHECK(f2::select_locomotion_clip(d.root_speeds[1], loco) == &d.clips[1]);  // walk at its speed
    F2_CHECK(f2::select_locomotion_clip(d.root_speeds[2], loco) == &d.clips[2]);  // run at its speed
    F2_CHECK(d.root_speeds[1] > 0.5f && d.root_speeds[2] > d.root_speeds[1]);  // walk < run, real motion

    // The app-forward math (compute_hero_pose): at delta_yaw=0 the render pose is the reference
    // with the {x,z,y} render swap; a +90deg delta additionally Y-rotates (place_vertex convention).
    std::vector<std::vector<std::array<float, 3>>> pose0, pose90;
    f2::compute_hero_pose(player, d.geom_bind, 0.0f, pose0);
    f2::compute_hero_pose(player, d.geom_bind, 1.5707963f, pose90);
    F2_CHECK(pose0.size() == d.geom_bind.size());
    float swap_err = 0.0f, rot_err = 0.0f;
    for (std::size_t g = 0; g < d.ref_pose.size(); ++g)
        for (std::size_t v = 0; v < d.ref_pose[g].size(); ++v) {
            const auto& r = d.ref_pose[g][v];
            const std::array<float, 3> sw{r[0], r[2], r[1]};       // MDL {x,y,z} -> render {x,z,y}
            const std::array<float, 3> rot{sw[2], sw[1], -sw[0]};  // +90deg: {x,y,z}->{z,y,-x}
            for (int k = 0; k < 3; ++k) {
                const float e0 = std::fabs(pose0[g][v][k] - sw[k]);
                const float e9 = std::fabs(pose90[g][v][k] - rot[k]);
                if (e0 > swap_err) swap_err = e0;
                if (e9 > rot_err) rot_err = e9;
            }
        }
    F2_CHECK(swap_err < 1e-4f);  // render swap correct
    F2_CHECK(rot_err < 1e-3f);   // Y-rotation matches place_vertex convention
}

int main() {
    const std::array<std::uint8_t, 136> dxt1 = [] {
        std::array<std::uint8_t, 136> bytes{};
        bytes[0] = 'D'; bytes[1] = 'D'; bytes[2] = 'S'; bytes[3] = ' ';
        auto write = [&](std::size_t offset, std::uint32_t value) {
            std::memcpy(bytes.data() + offset, &value, sizeof(value));
        };
        write(4, 124); write(12, 4); write(16, 4); write(76, 32);
        bytes[80] = 4; bytes[84] = 'D'; bytes[85] = 'X'; bytes[86] = 'T'; bytes[87] = '1';
        bytes[128] = 0xff; bytes[129] = 0xff; // white endpoint
        bytes[130] = 0; bytes[131] = 0; // black endpoint
        return bytes;
    }();
    f2::NativeTexture decoded_texture;
    std::string texture_error;
    assert(f2::decode_dds_rgba8(dxt1, decoded_texture, texture_error));
    assert(decoded_texture.width == 4 && decoded_texture.height == 4);
    assert(decoded_texture.rgba8.size() == 64);

    const auto ui_root = std::filesystem::temp_directory_path() / "f2native_ui_test";
    std::filesystem::create_directories(ui_root);
    std::ofstream(ui_root / "title_background.dds", std::ios::binary)
        .write(reinterpret_cast<const char*>(dxt1.data()), static_cast<std::streamsize>(dxt1.size()));
    auto black_dxt1 = dxt1;
    black_dxt1[128] = 0; black_dxt1[129] = 0;
    black_dxt1[130] = 0; black_dxt1[131] = 0;
    std::ofstream(ui_root / "ambient_atlas.dds", std::ios::binary)
        .write(reinterpret_cast<const char*>(black_dxt1.data()),
               static_cast<std::streamsize>(black_dxt1.size()));
    std::ofstream(ui_root / "ambient_baseline.dds", std::ios::binary)
        .write(reinterpret_cast<const char*>(dxt1.data()), static_cast<std::streamsize>(dxt1.size()));
    std::ofstream(ui_root / "ambient_detail.dds", std::ios::binary)
        .write(reinterpret_cast<const char*>(dxt1.data()), static_cast<std::streamsize>(dxt1.size()));
    std::ofstream(ui_root / "title_font.ttf", std::ios::binary).put('F');
    std::ofstream(ui_root / "ui_manifest.ini")
        << "title_background=title_background.dds\n"
        << "ambient_atlas=ambient_atlas.dds\n"
        << "ambient_baseline=ambient_baseline.dds\n"
        << "ambient_detail=ambient_detail.dds\n"
        << "title_font=title_font.ttf\n";
    f2::NativeUiAssets ui_assets;
    std::string ui_error;
    assert(ui_assets.load(ui_root, ui_error));
    assert(ui_assets.has(f2::NativeUiAsset::TitleBackground));
    assert(ui_assets.texture(f2::NativeUiAsset::TitleBackground)->width == 4);
    assert(ui_assets.has(f2::NativeUiAsset::AmbientAtlas));
    assert(ui_assets.texture(f2::NativeUiAsset::AmbientAtlas)->height == 4);
    assert(ui_assets.has(f2::NativeUiAsset::AmbientBaseline));
    assert(ui_assets.texture(f2::NativeUiAsset::AmbientBaseline)->width == 4);
    assert(ui_assets.has(f2::NativeUiAsset::AmbientDetail));
    assert(ui_assets.texture(f2::NativeUiAsset::AmbientAtlas)->rgba8[0] == 255);
    std::ofstream(ui_root / "ambient_detail_frame0.dds", std::ios::binary)
        .write(reinterpret_cast<const char*>(dxt1.data()), static_cast<std::streamsize>(dxt1.size()));
    std::ofstream(ui_root / "ambient_detail_frame1.dds", std::ios::binary)
        .write(reinterpret_cast<const char*>(black_dxt1.data()),
               static_cast<std::streamsize>(black_dxt1.size()));
    std::ofstream(ui_root / "ui_manifest.ini", std::ios::trunc)
        << "ambient_atlas=ambient_atlas.dds\n"
        << "ambient_detail_frames=ambient_detail_frame0.dds,ambient_detail_frame1.dds\n";
    f2::NativeUiAssets frame_assets;
    assert(frame_assets.load(ui_root, ui_error));
    assert(frame_assets.ambient_detail_frame_count() == 2);
    assert(frame_assets.texture(f2::NativeUiAsset::AmbientAtlas)->rgba8[0] == 0);
    f2::NativeTexture composed_ambient;
    assert(f2::compose_native_ambient_atlas(
        *frame_assets.texture(f2::NativeUiAsset::AmbientAtlas),
        *frame_assets.ambient_detail_frame(0), composed_ambient));
    assert(composed_ambient.rgba8[0] == 255);
    assert(ui_assets.title_font_path().filename() == "title_font.ttf");
    std::filesystem::remove_all(ui_root);

    const auto path = std::filesystem::temp_directory_path() / "f2native_core_test.f2scene";
    {
        std::ofstream out(path);
        out << "F2SCENE 1\n"
            << "material stone 0.7 0.7 0.7 1 albedo=pubgames/common/bar_focus.dds\n"
            << "mesh floor 3 3 0\n"
            << "vertex 0 0 0 0 1 0 0 0\n"
            << "vertex 1 0 0 0 1 0 1 0\n"
            << "vertex 0 0 1 0 1 0 0 1\n"
            << "index 0\nindex 1\nindex 2\n"
            << "instance floor 0 0 0 0 0 0 1\n";
    }

    f2::NativeGame game;
    std::string error;
    assert(game.load_scene(path, error));
    assert(game.scene.meshes.size() == 1);
    assert(game.scene.instances.size() == 1);
    assert(game.scene.materials[0].albedo == "pubgames/common/bar_focus.dds");
    game.tick(1.0 / 30.0);
    assert(game.elapsed_seconds > 0.0);
    game.tick(0.5);
    assert(game.frontend.state() == f2::FrontendState::IntroVideo);
    assert(game.frontend.intro_videos().current_clip() != nullptr);
    assert(game.frontend.intro_videos().current_clip()->id == "microsoft_logo");
    game.frontend.dispatch(f2::FrontendAction::Skip);
    assert(game.frontend.state() == f2::FrontendState::Title);
    assert(game.frontend.frontend_music_active());
    game.frontend.dispatch(f2::FrontendAction::Accept);
    assert(game.frontend.state() == f2::FrontendState::MainMenu);
    assert(game.frontend.frontend_music_active());
    assert(!game.frontend.select_card(true));
    assert(game.frontend.select_menu_item("new_game"));
    game.frontend.dispatch(f2::FrontendAction::Accept);
    assert(game.frontend.state() == f2::FrontendState::ChooseCard);
    assert(game.frontend.frontend_music_active());
    assert(!game.frontend.girl_selected());
    assert(game.frontend.select_card(true));
    assert(game.frontend.girl_selected());
    assert(game.frontend.select_card(false));
    assert(!game.frontend.girl_selected());
    game.frontend.dispatch(f2::FrontendAction::Right);
    assert(game.frontend.girl_selected());
    game.frontend.dispatch(f2::FrontendAction::Left);
    assert(!game.frontend.girl_selected());
    game.frontend.dispatch(f2::FrontendAction::Back);
    assert(game.frontend.state() == f2::FrontendState::MainMenu);
    assert(game.frontend.options_items().size() == 4);
    assert(game.frontend.select_menu_item("options"));
    game.frontend.dispatch(f2::FrontendAction::Accept);
    assert(game.frontend.state() == f2::FrontendState::Options);
    assert(game.frontend.frontend_music_active());
    assert(game.frontend.sound_enabled());
    assert(game.frontend.select_menu_item("game"));
    game.frontend.dispatch(f2::FrontendAction::Accept);
    assert(!game.frontend.subtitles_enabled());
    game.frontend.dispatch(f2::FrontendAction::Accept);
    assert(game.frontend.subtitles_enabled());
    assert(game.frontend.select_menu_item("controls"));
    game.frontend.dispatch(f2::FrontendAction::Back);
    assert(game.frontend.state() == f2::FrontendState::MainMenu);
    assert(game.frontend.select_menu_item("options"));
    game.frontend.dispatch(f2::FrontendAction::Accept);
    assert(game.frontend.state() == f2::FrontendState::Options);
    assert(game.frontend.remove_menu_item("options"));

    const auto source_root = std::filesystem::temp_directory_path() / "f2native_source_test";
    const auto data_root = source_root / "data";
    std::filesystem::create_directories(data_root / "art/videos");
    for (const auto* relative_path : {"dir.manifest", "art/videos/microsoft_logo.bik",
                                      "art/videos/lionhead_logo.bik", "art/videos/intro.bik"}) {
        std::ofstream(data_root / relative_path) << "test";
    }
    std::string source_error;
    const auto source = f2::detect_game_source(source_root, source_error);
    assert(source.has_value());
    assert(source->data_root == std::filesystem::weakly_canonical(data_root));
    std::filesystem::remove_all(source_root);

    // Frontend sound identities are keyed to the retail SE_GUI event names recovered from the
    // XEX (docs/RETAIL_FRONTEND_SPEC.md §8). Lock the mapping so it cannot silently drift.
    using S = f2::NativeFrontendSound;
    assert(f2::se_gui_event_name(S::NavigateUp) == "SE_GUI_SLIDE_MENU_UP");
    assert(f2::se_gui_event_name(S::NavigateDown) == "SE_GUI_SLIDE_MENU_DOWN");
    assert(f2::se_gui_event_name(S::SelectionLeft) == "SE_GUI_SELECTION_LEFT");
    assert(f2::se_gui_event_name(S::SelectionRight) == "SE_GUI_SELECTION_RIGHT");
    assert(f2::se_gui_event_name(S::Accept) == "SE_GUI_MENU_BOX_SELECT");
    assert(f2::se_gui_event_name(S::Back) == "SE_GUI_MENU_BOX_CANCEL");
    assert(f2::se_gui_event_name(S::Music).empty());

    // Options "Resolution" maps each index to a concrete PC pixel size the app applies to the
    // window/swapchain (docs §6). resolution_width/height are pure functions of the index.
    assert(f2::FrontendController::resolution_option_count() == 3);
    {
        const f2::FrontendController fc;  // default index → a valid, real resolution
        assert(fc.resolution_width() > 0 && fc.resolution_height() > 0);
        assert(fc.resolution_width() >= fc.resolution_height());  // landscape
    }

    // The render-backend contract (f2::render) must be drivable with no GPU — proves the frontend
    // scene layer is decoupled from D3D12/Vulkan. Exercise it through the headless NullRenderBackend.
    {
        f2::render::NullRenderBackend backend;
        std::string backend_error;
        assert(backend.initialize(nullptr, 1280, 720, backend_error));
        assert(backend.initialized() && backend.width() == 1280 && backend.height() == 720);
        const std::array<std::uint8_t, 4> white{255, 255, 255, 255};
        const auto tex = backend.create_texture(white.data(), 1, 1);
        assert(tex != f2::render::kInvalidTexture);
        const std::array<f2::render::UiQuad, 2> quads{{
            {tex, 0, 0, 100, 100},
            {tex, 100, 100, 200, 200},
        }};
        backend.begin_frame();
        backend.draw_ui(quads, 1280, 720);
        backend.present();
        assert(backend.last_quad_count() == 2);
        assert(backend.frames_presented() == 1);
        assert(backend.texture_count() == 1);
        backend.resize(1920, 1080);
        assert(backend.width() == 1920);

        // The backend-neutral scene (UiDrawList) is what a shared scene builder produces and any
        // backend consumes — exercise that path end-to-end.
        f2::render::UiDrawList scene;
        scene.add_sprite(tex, 0, 0, 640, 480);
        scene.add_rect(tex, 0, 0, 10, 10, 0xff0000ffu);
        scene.add_detail_sprite(tex, tex, 0, 0, 128, 128, 0, 0, 1, 1, 0, 0, 1, 1, 0xffffffffu, true);
        scene.set_last_rotation(1.5f);
        assert(scene.size() == 3);
        assert(scene.quads()[2].combine_detail && scene.quads()[2].key_black_matte);
        assert(scene.quads()[2].rotation_radians == 1.5f);
        backend.begin_frame();
        backend.draw_ui(scene.quads(), 1920, 1080);
        backend.present();
        assert(backend.last_quad_count() == 3 && backend.frames_presented() == 2);

        // TextureRegistry: stable key -> TextureId -> backend handle (the resolver indirection).
        f2::render::TextureRegistry registry;
        const auto id_a = registry.id_for_key(7);
        const auto id_b = registry.id_for_key(42);
        assert(id_a != f2::render::kInvalidTexture && id_b != id_a);
        assert(registry.id_for_key(7) == id_a);  // stable
        registry.set_handle(id_a, 0xDEADBEEFull);
        assert(registry.handle(id_a) == 0xDEADBEEFull);
        assert(registry.handle(id_b) == 0);                       // unset
        assert(registry.handle(f2::render::kInvalidTexture) == 0);  // invalid
        assert(registry.size() == 2);
    }

    {
        // NativeFont: rasterize a real TTF into an RGBA8 atlas + ImGui-compatible glyph metrics.
        // Uses a Windows system font (this is a win32-only frontend); skips cleanly if none exist.
        const std::array<const char*, 3> candidates = {
            "C:/Windows/Fonts/arial.ttf", "C:/Windows/Fonts/segoeui.ttf",
            "C:/Windows/Fonts/tahoma.ttf"};
        std::filesystem::path font_path;
        for (const char* c : candidates) {
            if (std::filesystem::exists(c)) { font_path = c; break; }
        }
        if (!font_path.empty()) {
            f2::NativeFont font;
            std::string font_error;
            assert(font.load(font_path, 48.0f, font_error));
            assert(font.ready());
            assert(font.pixel_height() == 48.0f);
            assert(font.atlas_width() == 1024 && font.atlas_height() == 1024);
            assert(font.atlas_rgba8().size() == 1024u * 1024u * 4u);
            // 'A' must be a valid glyph with a positive advance and a non-degenerate UV rect.
            const auto& glyph_a = font.glyph(static_cast<std::uint32_t>('A'));
            assert(glyph_a.valid);
            assert(glyph_a.advance > 0.0f);
            assert(glyph_a.u1 > glyph_a.u0 && glyph_a.v1 > glyph_a.v0);
            // Space advances but draws nothing; out-of-range codepoints are invalid.
            assert(font.glyph(static_cast<std::uint32_t>(' ')).advance > 0.0f);
            assert(!font.glyph(0u).valid && !font.glyph(300u).valid);  // out of Latin-1 range
            // measure() scales advances by size/pixel_height and sums; wider text => wider measure.
            assert(font.measure("A", 48.0f) > 0.0f);
            assert(font.measure("AA", 48.0f) > font.measure("A", 48.0f));
            assert(font.measure("A", 96.0f) > font.measure("A", 48.0f));
        }
    }

    {
        // Options navigation: inside a tab you move a row cursor (Up/Down) and change the focused
        // value (Left/Right) — the fix for "can't cycle options within a tab". Also covers the FPS toggle.
        using A = f2::FrontendAction;
        using S = f2::FrontendState;
        f2::FrontendController fc;
        fc.dispatch(A::Skip);  // Boot -> Title
        assert(fc.state() == S::Title);
        fc.dispatch(A::Accept);  // Title -> MainMenu
        assert(fc.state() == S::MainMenu);
        assert(fc.select_menu_item("options"));
        fc.dispatch(A::Accept);  // -> Options (tabs), page closed
        assert(fc.state() == S::Options);
        assert(!fc.options_page_open());

        // Move to the Video tab (Up/Down cycles tabs while the page is closed).
        for (int i = 0; i < 8 && fc.options_items()[fc.selected_item()].id != "video"; ++i) {
            fc.dispatch(A::Down);
        }
        assert(fc.options_items()[fc.selected_item()].id == "video");

        // Open the tab: the row cursor starts at 0 and Up/Down now moves it (not the tab).
        fc.dispatch(A::Accept);
        assert(fc.options_page_open());
        assert(fc.option_row() == 0);
        assert(fc.option_row_count() == 5);  // Gamma, Resolution, AA, FPS, Renderer
        fc.dispatch(A::Down);
        assert(fc.option_row() == 1);
        fc.dispatch(A::Down);
        fc.dispatch(A::Down);
        assert(fc.option_row() == 3);  // FPS Display row

        // FPS toggle via Left/Right on its row.
        assert(!fc.fps_display_enabled());
        fc.dispatch(A::Right);
        assert(fc.fps_display_enabled());
        fc.dispatch(A::Left);
        assert(!fc.fps_display_enabled());

        // Cursor clamps at the last row.
        fc.dispatch(A::Down);
        assert(fc.option_row() == 4);
        fc.dispatch(A::Down);
        assert(fc.option_row() == 4);

        // Resolution (row 1) changes with Left/Right.
        while (fc.option_row() > 1) fc.dispatch(A::Up);
        assert(fc.option_row() == 1);
        const int res0 = fc.resolution_index();
        fc.dispatch(A::Right);
        assert(fc.resolution_index() == (res0 < 2 ? res0 + 1 : 2));

        // Back closes the page but stays in Options.
        fc.dispatch(A::Back);
        assert(!fc.options_page_open());
        assert(fc.state() == S::Options);

        // The Game tab exposes 5 rows including the toggles reachable by the cursor.
        while (fc.options_items()[fc.selected_item()].id != "game") fc.dispatch(A::Up);
        fc.dispatch(A::Accept);
        assert(fc.option_row_count() == 5);
        const bool subs0 = fc.subtitles_enabled();
        fc.dispatch(A::Left);  // row 0 = Subtitles -> Off
        assert(fc.subtitles_enabled() == false);
        (void)subs0;
    }

    {
        // Options persistence: save_options/read_options round-trip + FrontendController reload,
        // isolated to a temp config dir (FABLE2NATIVE_CONFIG_DIR) so the real options.ini is untouched.
        const auto cfg = std::filesystem::temp_directory_path() / "f2native_cfg_test";
        std::filesystem::create_directories(cfg);
        SetEnvironmentVariableW(L"FABLE2NATIVE_CONFIG_DIR", cfg.wstring().c_str());
        std::filesystem::remove(cfg / "options.ini");
        f2::FrontendOptions written;
        written.subtitles = false;
        written.anti_aliasing_index = 3;
        written.resolution_index = 2;
        written.gamma_percent = 73;
        written.sounds_volume = 42;
        written.music_volume = 17;
        written.fps_display = true;
        f2::save_options(written);
        const f2::FrontendOptions read_back = f2::read_options();
        assert(read_back.subtitles == false);
        assert(read_back.anti_aliasing_index == 3);
        assert(read_back.resolution_index == 2);
        assert(read_back.gamma_percent == 73);
        assert(read_back.sounds_volume == 42);
        assert(read_back.music_volume == 17);
        assert(read_back.fps_display == true);
        // A controller picks the persisted options up on reset().
        f2::FrontendController fc;
        fc.reset();
        assert(fc.anti_aliasing_index() == 3);
        assert(fc.resolution_index() == 2);
        assert(fc.gamma_percent() == 73);
        assert(fc.sounds_volume() == 42);
        assert(fc.fps_display_enabled() == true);
        assert(fc.subtitles_enabled() == false);
        SetEnvironmentVariableW(L"FABLE2NATIVE_CONFIG_DIR", nullptr);
        std::filesystem::remove(cfg / "options.ini");
        std::filesystem::remove(cfg / "renderer.txt");
    }

    {
        // Legacy F2SCENE water materials omit water_params=. They must still use the retail
        // defaults instead of uploading an all-zero constant block to either backend.
        const auto legacy_path = std::filesystem::temp_directory_path() /
                                 "f2native_legacy_water.f2scene";
        {
            std::ofstream output(legacy_path);
            output << "F2SCENE 1\n"
                   << "material water 0.14 0.34 0.52 1\n";
        }
        f2::NativeScene legacy;
        std::string legacy_error;
        assert(f2::load_native_scene(legacy_path, legacy, legacy_error));
        assert(legacy.materials.size() == 1);
        assert(!legacy.materials[0].has_water_params);
        assert(std::abs(legacy.materials[0].water_params[0] - 0.20f) < 1e-5f);
        assert(std::abs(legacy.materials[0].water_params[6] - 0.188f) < 1e-5f);
        assert(std::abs(legacy.materials[0].water_params[29] - 0.75f) < 1e-5f);
        assert(std::abs(legacy.materials[0].water_params[36] - 128.0f) < 1e-5f);
        std::filesystem::remove(legacy_path);
    }

    {
        // F2SCENE writer round-trip: the level cooker's output stage must reload identically.
        f2::NativeScene written;
        written.sun_direction = {0.1f, -0.9f, 0.4f};
        written.sky_color = {0.2f, 0.3f, 0.4f, 1.0f};
        written.has_hero_start = true;
        written.hero_start = {12.5f, 1.25f, -8.25f};
        written.hero_yaw = -0.75f;
        f2::NativeMaterial mat;
        mat.name = "wall_stone";
        mat.base_color = {0.8f, 0.7f, 0.6f, 1.0f};
        mat.albedo = "worlds/albion/bwsslums/wall.dds";
        mat.has_water_params = true;
        for (std::size_t i = 0; i < mat.water_params.size(); ++i)
            mat.water_params[i] = static_cast<float>(i) * 0.125f - 1.0f;
        mat.water_opacity = 0.42f;
        written.materials.push_back(mat);
        f2::NativeMesh mesh;
        mesh.name = "house_01";
        mesh.material = 0;
        mesh.vertices.push_back({{0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f}});
        mesh.vertices.push_back({{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {1.0f, 0.0f}});
        mesh.vertices.push_back({{0.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 1.0f}});
        mesh.indices = {0, 1, 2};
        written.meshes.push_back(mesh);
        written.instances.push_back({0, {12.5f, 0.0f, -8.25f}, {0.0f, 1.5708f, 0.0f}, 2.0f});
        // Cloud layers (theme Clouds) must round-trip through the F2SCENE writer/reader too.
        written.cloud_global_brightness = 1.0f;
        written.cloud_alpha_ref = 0.019608f;
        f2::NativeCloudLayer cloud;
        cloud.density_map = "clouds/cloud_03.dds";
        cloud.height = 500.0f;
        cloud.size_x = 3000.0f;
        cloud.size_y = 3000.0f;
        cloud.texture_scale_x = 3.0f;
        cloud.texture_scale_y = 5.0f;
        cloud.velocity_x = 10.0f;
        cloud.velocity_y = 20.0f;
        cloud.transparency = 0.5f;
        cloud.brightness = 0.25f;
        cloud.ambient = 0.9f;
        cloud.normal_strength = 0.5f;
        written.clouds.push_back(cloud);
        // Celestial moon billboard must round-trip too.
        written.has_moon = true;
        written.moon.texture = "sky/moonphases.dds";
        written.moon.glare_texture = "sky/sunglare.dds";
        written.moon.direction = {0.566f, 0.766f, -0.305f};
        written.moon.intensity = 1.0f;
        written.moon.size = 1.7f;
        written.moon.transparency = 3.0f;
        written.moon.glare_intensity = 50.0f;
        written.moon.glare_size = 1.4f;
        written.moon.exposure = 10.0f;
        written.moon.phase = 4;
        written.star_brightness = 1.0f;

        const auto scene_path = std::filesystem::temp_directory_path() / "f2native_scene_roundtrip.f2scene";
        std::string scene_error;
        assert(f2::save_native_scene(scene_path, written, scene_error));
        f2::NativeScene reloaded;
        assert(f2::load_native_scene(scene_path, reloaded, scene_error));
        assert(reloaded.materials.size() == 1);
        assert(reloaded.materials[0].name == "wall_stone");
        assert(reloaded.materials[0].albedo == "worlds/albion/bwsslums/wall.dds");
        assert(reloaded.materials[0].has_water_params);
        assert(reloaded.has_hero_start);
        assert(std::abs(reloaded.hero_start[0] - 12.5f) < 1e-5f);
        assert(std::abs(reloaded.hero_start[1] - 1.25f) < 1e-5f);
        assert(std::abs(reloaded.hero_start[2] + 8.25f) < 1e-5f);
        assert(std::abs(reloaded.hero_yaw + 0.75f) < 1e-5f);
        for (std::size_t i = 0; i < reloaded.materials[0].water_params.size(); ++i)
            assert(approx(reloaded.materials[0].water_params[i],
                          static_cast<float>(i) * 0.125f - 1.0f));
        assert(approx(reloaded.materials[0].water_opacity, 0.42f));
        assert(reloaded.meshes.size() == 1);
        assert(reloaded.meshes[0].name == "house_01");
        assert(reloaded.meshes[0].vertices.size() == 3);
        assert(reloaded.meshes[0].indices == std::vector<std::uint32_t>({0, 1, 2}));
        assert(reloaded.instances.size() == 1);
        assert(reloaded.instances[0].mesh == 0);
        const auto approx = [](float a, float b) { return std::abs(a - b) < 1e-5f; };
        assert(approx(reloaded.instances[0].position[0], 12.5f));
        assert(approx(reloaded.instances[0].position[2], -8.25f));
        assert(approx(reloaded.instances[0].rotation[1], 1.5708f));
        assert(approx(reloaded.instances[0].scale, 2.0f));
        assert(approx(reloaded.sun_direction[1], -0.9f));
        assert(approx(reloaded.sky_color[2], 0.4f));
        assert(reloaded.clouds.size() == 1);
        assert(reloaded.clouds[0].density_map == "clouds/cloud_03.dds");
        assert(approx(reloaded.clouds[0].height, 500.0f));
        assert(approx(reloaded.clouds[0].size_x, 3000.0f));
        assert(approx(reloaded.clouds[0].texture_scale_y, 5.0f));
        assert(approx(reloaded.clouds[0].velocity_y, 20.0f));
        assert(approx(reloaded.clouds[0].transparency, 0.5f));
        assert(approx(reloaded.clouds[0].ambient, 0.9f));
        assert(approx(reloaded.clouds[0].normal_strength, 0.5f));
        assert(approx(reloaded.cloud_alpha_ref, 0.019608f));
        assert(reloaded.has_moon);
        assert(reloaded.moon.texture == "sky/moonphases.dds");
        assert(reloaded.moon.glare_texture == "sky/sunglare.dds");
        assert(approx(reloaded.moon.direction[1], 0.766f));
        assert(approx(reloaded.moon.intensity, 1.0f));
        assert(approx(reloaded.moon.glare_intensity, 50.0f));
        assert(reloaded.moon.phase == 4);
        assert(approx(reloaded.star_brightness, 1.0f));
        std::filesystem::remove(scene_path);
    }

    // ---- P0: master gameplay tick order (Quest -> General -> AI) ----
    {
        f2::ScriptSystems ss;
        std::string order;
        ss.quest.enabled = true;
        ss.quest.update = [&](double) { order += 'Q'; };
        ss.general.enabled = true;
        ss.general.update = [&](double) { order += 'G'; };
        ss.ai.enabled = true;
        ss.ai.update = [&](double) { order += 'A'; };
        ss.tick(1.0 / 60.0);
        assert(order == "QGA");  // retail-recovered order (Function_82281FE0)

        // Disabled managers are skipped; order of the rest preserved.
        order.clear();
        ss.general.enabled = false;
        ss.tick(1.0 / 60.0);
        assert(order == "QA");
    }

    // ---- P0: analog stick deadzone/extent curve (decomp-grounded thresholds) ----
    {
        f2::InputAnalogConfig cfg;  // deadzone 0.4, extent 0.8 (shipped defaults)
        // Inside the deadzone reads as zero.
        auto z = f2::InputSampler::apply_stick_curve({0.3f, 0.0f}, cfg);
        assert(approx(z[0], 0.0f) && approx(z[1], 0.0f));
        // At/above the extent threshold saturates to magnitude 1.0.
        auto s = f2::InputSampler::apply_stick_curve({0.8f, 0.0f}, cfg);
        assert(approx(s[0], 1.0f) && approx(s[1], 0.0f));
        // Halfway between deadzone(0.4) and extent(0.8) -> 0.5 magnitude.
        auto m = f2::InputSampler::apply_stick_curve({0.6f, 0.0f}, cfg);
        assert(approx(m[0], 0.5f) && approx(m[1], 0.0f));
        // Direction preserved on a diagonal past the deadzone.
        auto d = f2::InputSampler::apply_stick_curve({0.8f, 0.8f}, cfg);
        assert(d[0] > 0.0f && approx(d[0], d[1]));  // symmetric input -> symmetric output
    }

    // ---- P0: button edge-detect helpers ----
    {
        f2::InputState in;
        in.buttons = static_cast<std::uint16_t>(f2::PadButton::A);
        in.buttons_pressed = static_cast<std::uint16_t>(f2::PadButton::A);
        assert(in.down(f2::PadButton::A));
        assert(in.pressed(f2::PadButton::A));
        assert(!in.down(f2::PadButton::B));
        assert(!in.pressed(f2::PadButton::B));
        // Default active device + a Frontend-mode game does not tick script systems.
        f2::NativeGame g;
        assert(g.mode == f2::GameMode::Frontend);
    }

    // ---- P1: FNV-1 hasher matches byte-verified GDB hashes ----
    {
        assert(f2::gdb::fnv1("RemoveComponent") == 0x9B41D00Au);
        assert(f2::gdb::fnv1("Position") == 0xBD7C27D4u);
        assert(f2::gdb::fnv1("GraphicAppearanceStaticMeshComponent") == 0x29CF50D1u);
        assert(f2::gdb::fnv1("") == f2::gdb::kFnvBasis);
    }

    // ---- P1: component registry lookup (sorted bsearch) ----
    {
        f2::ComponentRegistry reg;
        reg.seed_defaults();
        const auto* d = reg.lookup(f2::gdb::kCompGraphicAppearanceStaticMesh);
        assert(d != nullptr);
        assert(d->type_id == f2::kTypeIdGraphicAppearanceStaticMesh);
        assert(reg.lookup(0xDEADBEEFu) == nullptr);  // unregistered -> null
    }

    // ---- P1: entity factory dup-gate + sorted-by-typeId insert ----
    {
        f2::EntityManager em;
        f2::NativeEntity& e = em.create_entity(0x1234);
        assert(e.uid == 1);
        auto* c1 = em.create_component_by_hash(e, f2::gdb::kCompGraphicAppearanceStaticMesh);
        assert(c1 != nullptr && e.component_count() == 1);
        // Second create of the same typeId is gated (entity+0x24 dup mask): no dup.
        auto* c2 = em.create_component_by_hash(e, f2::gdb::kCompGraphicAppearanceStaticMesh);
        assert(c2 == c1 && e.component_count() == 1);
        // A second, distinct entity gets the next UID.
        assert(em.create_entity().uid == 2);
        // Direct add of Transform (engine-special) coexists, sorted by typeId.
        e.add_component(std::make_unique<f2::TransformComponent>());
        assert(e.component_count() == 2);
        assert(e.component_by_typeid(f2::kTypeIdTransform) != nullptr);
        assert(e.component_by_typeid(f2::kTypeIdGraphicAppearanceStaticMesh) != nullptr);
    }

    // ---- P1: collect_component_hashes (parent-first union minus RemoveComponent) ----
    {
        // Synthetic 3-level chain: grandparent -> parent -> child.
        // grandparent declares {A, B}; parent adds {C}; child removes {B}, adds {D}.
        struct FakeSource : f2::GdbRecordSource {
            bool parent_of(std::uint32_t guid, std::uint32_t& out) const override {
                if (guid == 3) { out = 2; return true; }  // child -> parent
                if (guid == 2) { out = 1; return true; }  // parent -> grandparent
                return false;                              // grandparent -> none
            }
            std::vector<std::uint32_t> component_fields(std::uint32_t guid) const override {
                if (guid == 1) return {0xA, 0xB};
                if (guid == 2) return {0xC};
                if (guid == 3) return {0xD};
                return {};
            }
            std::vector<std::uint32_t> removed_components(std::uint32_t guid) const override {
                if (guid == 3) return {0xB};  // child prunes inherited B
                return {};
            }
        } src;
        auto set = f2::collect_component_hashes(3, src);
        // Expect A, C, D (B pruned); ancestors first.
        assert(set.size() == 3);
        assert(std::find(set.begin(), set.end(), 0xAu) != set.end());
        assert(std::find(set.begin(), set.end(), 0xCu) != set.end());
        assert(std::find(set.begin(), set.end(), 0xDu) != set.end());
        assert(std::find(set.begin(), set.end(), 0xBu) == set.end());
    }

    // ---- P1: entity -> scene.instances transform bridge ----
    {
        f2::NativeScene s;
        f2::NativeInstance inst;
        inst.position = {1.0f, 2.0f, 3.0f};
        s.instances.push_back(inst);

        f2::NativeWorld w;
        w.spawn_from_scene(s);
        assert(w.entities.entity_count() == 1);

        // Move the entity's transform, sync, and confirm the drawn instance moved.
        f2::NativeEntity* e = w.entities.find(1);
        assert(e != nullptr);
        auto* t = e->get<f2::TransformComponent>(f2::kTypeIdTransform);
        assert(t != nullptr);
        assert(approx(t->position[0], 1.0f));  // seeded from the instance
        t->position = {10.0f, 20.0f, 30.0f};
        w.sync_to_scene(s);
        assert(approx(s.instances[0].position[0], 10.0f));
        assert(approx(s.instances[0].position[1], 20.0f));
        assert(approx(s.instances[0].position[2], 30.0f));
    }

    // ---- P2: collision heightfield ground-clamp + collide-and-slide ----
    {
        // A flat 20x20 terrain quad at y=0 (two triangles), plus a wall box.
        f2::NativeScene s;
        f2::NativeMesh ground;
        auto add_v = [&](float x, float y, float z) {
            f2::NativeVertex v; v.position = {x, y, z}; ground.vertices.push_back(v);
        };
        add_v(-10.0f, 0.0f, -10.0f); add_v(10.0f, 0.0f, -10.0f);
        add_v(10.0f, 0.0f, 10.0f);   add_v(-10.0f, 0.0f, 10.0f);
        ground.indices = {0, 1, 2, 0, 2, 3};
        s.meshes.push_back(ground);
        f2::NativeInstance gi; gi.mesh = 0; s.instances.push_back(gi);
        // A small wall block mesh (unit cube) placed at x=5.
        f2::NativeMesh cube;
        for (float xx : {-0.5f, 0.5f}) for (float yy : {0.0f, 2.0f}) for (float zz : {-0.5f, 0.5f}) {
            f2::NativeVertex v; v.position = {xx, yy, zz}; cube.vertices.push_back(v);
        }
        cube.indices = {0, 1, 2};  // geometry irrelevant; AABB is what matters
        s.meshes.push_back(cube);
        f2::NativeInstance ci; ci.mesh = 1; ci.position = {5.0f, 0.0f, 0.0f}; s.instances.push_back(ci);

        f2::NativeCollisionWorld world;
        world.build_from_scene(s);
        assert(world.has_terrain());
        // Ground sample over the flat quad returns ~0.
        float gy = world.sample_ground(0.0f, 0.0f, 3.0f);
        assert(std::isfinite(gy) && approx(gy, 0.0f));

        // Controller starts above ground, falls, and clamps to the terrain.
        f2::CharacterController cc;
        assert(approx(cc.config.capsule_radius, 1.0f));  // GROUNDED value
        cc.position = {0.0f, 0.5f, 0.0f};
        for (int i = 0; i < 30; ++i) cc.move(world, {0.0f, 0.0f}, 1.0f / 60.0f);
        assert(cc.on_ground && approx(cc.position[1], 0.0f));

        // Walking east into the wall box gets stopped/slid (x cannot pass the box face).
        cc.position = {0.0f, 0.0f, 0.0f};
        for (int i = 0; i < 120; ++i) cc.move(world, {5.0f, 0.0f}, 1.0f / 60.0f);  // +x
        assert(cc.position[0] < 5.0f - 0.5f + 0.001f);  // blocked before the box interior
    }

    // ---- Combat foundation: HealthComponent (Health.Modify verb) ----
    {
        f2::ComponentRegistry reg; reg.seed_defaults();
        assert(reg.lookup(f2::gdb::kCompHealth)->type_id == f2::kTypeIdHealth);  // 36

        f2::EntityManager em;
        f2::NativeEntity& e = em.create_entity();
        auto* h = static_cast<f2::HealthComponent*>(
            em.create_component_by_hash(e, f2::gdb::kCompHealth));
        assert(h != nullptr && approx(h->health, 70.0f) && !h->is_dead());

        // Damage clamps and reports death on the killing blow only.
        assert(!h->modify(-20.0f) && approx(h->health, 50.0f));
        assert(h->modify(-60.0f) && h->is_dead() && approx(h->health, 0.0f));  // alive->0 = died
        assert(!h->modify(-10.0f));  // already dead: no second "died"
        // Heal clamps to max.
        h->health = 65.0f; h->modify(100.0f); assert(approx(h->health, 70.0f));
        // Invulnerable ignores damage but allows healing.
        h->invulnerable = true; h->modify(-50.0f); assert(approx(h->health, 70.0f));
        h->health = 40.0f; h->modify(10.0f); assert(approx(h->health, 50.0f));

        // Persists through the save archive.
        h->health = 33.0f; h->max_health = 80.0f; h->invulnerable = false;
        f2::WorldArchive w(f2::ArchiveMode::Write); h->serialize(w);
        f2::HealthComponent h2; f2::WorldArchive r(w.take()); h2.serialize(r);
        assert(approx(h2.health, 33.0f) && approx(h2.max_health, 80.0f) && !h2.invulnerable);
    }

    // ---- P4: NPC agents wired into the world ----
    {
        f2::NativeScene s;
        f2::NativeMesh ground;
        auto gv = [&](float x, float y, float z) {
            f2::NativeVertex vv; vv.position = {x, y, z}; ground.vertices.push_back(vv);
        };
        gv(-30, 0, -30); gv(30, 0, -30); gv(30, 0, 30); gv(-30, 0, 30);
        ground.indices = {0, 1, 2, 0, 2, 3};
        s.meshes.push_back(ground);
        f2::NativeMesh npc_mesh; npc_mesh.name = "npc0_0"; s.meshes.push_back(npc_mesh);
        f2::NativeInstance gi; gi.mesh = 0; s.instances.push_back(gi);
        f2::NativeInstance ni; ni.mesh = 1; ni.position = {2.0f, 0.0f, 0.0f}; s.instances.push_back(ni);

        f2::NativeCollisionWorld collision; collision.build_from_scene(s);
        f2::NativeWorld w;
        w.spawn_from_scene(s);
        assert(w.npcs.size() == 1);                    // one agent for the villager geom
        assert(w.npcs[0].entity_uid == 2);
        // The villager entity carries Villager + Health (+ Transform + GraphicAppearance).
        auto* npc_ent = w.entities.find(2);
        assert(npc_ent->get<f2::VillagerComponent>(f2::kTypeIdVillager) != nullptr);
        auto* npc_hp = npc_ent->get<f2::HealthComponent>(f2::kTypeIdHealth);
        assert(npc_hp != nullptr && !npc_hp->is_dead());
        assert(approx(w.npcs[0].controller.position()[0], 2.0f));  // placed at its instance

        // Player standing next to the NPC with clear LOS -> the agent notices.
        w.update_npcs(collision, {3.0f, 0.0f, 0.0f}, 1.0f / 60.0f);
        assert(w.npcs[0].controller.state == f2::NpcState::Notice);
        // The agent's transform synced back onto its entity (so sync_to_scene moves it).
        auto* npc_e = w.entities.find(2);
        auto* npc_tf = npc_e->get<f2::TransformComponent>(f2::kTypeIdTransform);
        assert(approx(npc_tf->position[0], w.npcs[0].controller.position()[0]));

        // Player far away -> LOD gate makes the agent inactive (no state work).
        f2::NativeWorld w2; w2.spawn_from_scene(s);
        w2.update_npcs(collision, {200.0f, 0.0f, 200.0f}, 1.0f / 60.0f);
        assert(!w2.npcs[0].controller.active);
    }

    // ---- P6: embedded Lua VM (native bindings + coroutines + manager wiring) ----
    {
        f2::NativeScriptVM vm;
        assert(vm.valid());
        struct Obs { int calls = 0; std::string last; };
        Obs obs; vm.set_user_data(&obs);
        // register-class-method: Debug.Log records, Math.Add round-trips args<->results.
        vm.register_native("Debug", "Log", [](f2::NativeScriptVM& v) -> int {
            auto* o = static_cast<Obs*>(v.user_data());
            o->last = v.arg_string(1); o->calls++;
            return 0;
        });
        vm.register_native("Math", "Add", [](f2::NativeScriptVM& v) -> int {
            v.push_number(v.arg_number(1) + v.arg_number(2)); return 1;
        });
        assert(vm.run_source("Debug.Log('hi'); Debug.Log('there')"));
        assert(obs.calls == 2 && obs.last == "there");
        assert(vm.run_source("assert(Math.Add(2, 3) == 5)"));  // native round-trip through Lua

        // Coroutine driven from the native side (the manager scheduling model).
        assert(vm.run_source(
            "co = coroutine.create(function() Debug.Log('a'); coroutine.yield(); Debug.Log('b') end)\n"
            "function Step(dt) coroutine.resume(co) end"));
        obs.calls = 0;
        assert(vm.call_global("Step", 0.0) && obs.calls == 1 && obs.last == "a");  // ran to yield
        assert(vm.call_global("Step", 0.0) && obs.calls == 2 && obs.last == "b");  // resumed past yield

        // Errors are reported, not crashed.
        assert(!vm.run_source("this is not valid lua") && !vm.last_error().empty());
        assert(!vm.call_global("NoSuchFunction", 0.0));
    }

    // ---- P6: auto-stub (missing natives don't crash) + call_method ----
    {
        f2::NativeScriptVM vm;
        vm.register_native("World", "NpcCount",
                           [](f2::NativeScriptVM& v) -> int { v.push_number(3); return 1; });
        assert(vm.install_autostub());
        // Real native still works; unknown natives resolve to chainable black-hole (no
        // error); predicates (Is*/Has*/...) return a real false to avoid truthiness drift.
        assert(vm.run_source(
            "assert(World.NpcCount() == 3)\n"
            "local x = Foo.Bar()\n"                                 // unknown class -> stub table
            "local y = World.SomethingMissing():AndChain().Deep\n"  // chainable, no crash
            "assert(World.IsWhatever() == false)\n"                 // predicate -> false
            "assert(Debug.IsThing() == false)\n"));
        assert(vm.last_error().empty());
        bool saw_foo = false, saw_missing = false;
        for (const auto& m : vm.stub_misses()) {
            if (m == "Foo.Bar") saw_foo = true;
            if (m == "World.SomethingMissing") saw_missing = true;
        }
        assert(saw_foo && saw_missing);

        // call_method drives a manager's :Update(dt) method.
        assert(vm.run_source("Mgr = { n = 0, Update = function(self, dt) self.n = self.n + 1 end }"));
        assert(vm.call_method("Mgr", "Update", 0.016));
        assert(vm.run_source("assert(Mgr.n == 1)"));

        // Without the auto-stub, call_method reports not-found (so the manager wiring can
        // fall back to a free *Update global).
        f2::NativeScriptVM plain;
        assert(!plain.call_method("NoSuchManager", "Update", 0.0));
    }

    // ---- P6: script managers wired into the NativeGame tick ----
    {
        f2::NativeGame game;
        assert(game.enable_scripting());
        // A manager script: AIUpdate logs + can query the world via a native.
        assert(game.script_vm->run_source(
            "function AIUpdate(dt) Debug.Log('ai:' .. tostring(World.NpcCount())) end"));
        game.mode = f2::GameMode::InWorld;
        game.external_input = true;
        game.input = f2::InputState{};
        game.tick(1.0 / 60.0);  // one fixed step -> Quest/General/AI -> AIUpdate runs
        assert(!game.script_log.empty() && game.script_log.back() == "ai:0");
        // Frontend mode does NOT tick the managers.
        game.mode = f2::GameMode::Frontend;
        const std::size_t before = game.script_log.size();
        game.tick(1.0 / 60.0);
        assert(game.script_log.size() == before);
    }

    // ---- P6: a mod-style Lua FILE drives the gameplay systems via action natives ----
    {
        f2::NativeScene ms;
        f2::NativeMesh ground; f2::NativeVertex mv;
        const float quad[4][2] = {{-20, -20}, {20, -20}, {20, 20}, {-20, 20}};
        for (const auto& c : quad) { mv.position = {c[0], 0.0f, c[1]}; ground.vertices.push_back(mv); }
        ground.indices = {0, 1, 2, 0, 2, 3}; ms.meshes.push_back(ground);
        f2::NativeMesh nm; nm.name = "npc0_0"; ms.meshes.push_back(nm);
        f2::NativeInstance mgi; mgi.mesh = 0; ms.instances.push_back(mgi);
        f2::NativeInstance mni; mni.mesh = 1; mni.position = {3.0f, 0.0f, 0.0f}; ms.instances.push_back(mni);

        f2::NativeGame game; game.scene = ms; game.prepare_world();
        assert(game.enable_scripting());

        const auto mod_path = std::filesystem::temp_directory_path() / "f2native_mod.lua";
        std::ofstream(mod_path)
            << "Player.SetPosition(11, 0, 22)\n"
            << "Game.SetChapter(5)\n"
            << "assert(World.NpcCount() == 1)\n"
            << "assert(World.DamageNpc(0, 30) == false)\n";  // 70-30=40, survives
        assert(game.script_vm->run_file(mod_path.string().c_str()));

        assert(approx(game.player.position()[0], 11.0f) && approx(game.player.position()[2], 22.0f));
        assert(game.game_state.header.chapter == 5);
        auto* mnpc = game.world.entities.find(game.world.npcs[0].entity_uid);
        assert(approx(mnpc->get<f2::HealthComponent>(f2::kTypeIdHealth)->health, 40.0f));
        // Expanded natives (native_bindings module): NPC health query + Camera + quest count.
        assert(game.script_vm->run_source(
            "assert(World.GetNpcHealth(0) == 40)\n"
            "Quest.SetComplete(5); Quest.SetComplete(9)\n"
            "assert(Quest.CountComplete() == 2)\n"
            "assert(Game.GetChapter() == 5)\n"
            "local cx, cy, cz = Camera.GetPosition()\n"
            "assert(type(cx) == 'number')\n"
            "assert(Debug.GetRandomNumber(3, 3) == 3)"));
        std::filesystem::remove(mod_path);

        // Missing file is reported, not crashed.
        assert(!game.script_vm->run_file("does_not_exist.lua") && !game.script_vm->last_error().empty());
    }

    // ---- P6: BnkReader + LuaQ load (real game BNK; skipped if game data absent) ----
    {
        const std::filesystem::path bnk_path =
            "D:/Documents/Fable2RE/Fable2Recomp/assets/game/data/gamescripts_r.bnk";
        if (std::filesystem::exists(bnk_path)) {
            f2::BnkReader bnk;
            assert(bnk.open(bnk_path.string()));
            assert(bnk.entry_count() >= 500);  // ~555 entries
            assert(bnk.has("miscellaneous/generalsetupscript.lua"));
            auto boot = bnk.extract("miscellaneous/generalsetupscript.lua");
            assert(boot.size() > 6);
            // LuaQ signature 1B 4C 75 61 51 00.
            assert(boot[0] == 0x1b && boot[1] == 'L' && boot[2] == 'u' && boot[3] == 'a' &&
                   boot[4] == 'Q' && boot[5] == 0x00);
            // STEP 0 proof: the float-configured VM ACCEPTS the game's 4-byte-Number LuaQ
            // header (no "bad header in precompiled chunk"). load_only doesn't run it.
            f2::NativeScriptVM vm;
            assert(vm.load_only(boot.data(), boot.size(), "=generalsetupscript"));
            // A two-chunk entry also decodes + loads (exercises the 0x8000-stride path).
            if (bnk.has("quests/qc010_childhood.lua")) {
                auto q = bnk.extract("quests/qc010_childhood.lua");
                assert(q.size() > 6 && q[0] == 0x1b);
                assert(vm.load_only(q.data(), q.size(), "=qc010"));
            }
        }
    }

    // ---- P6: boot_game_scripts end-to-end (real BNK; skipped if game data absent) ----
    // Runs in its own function (test_boot_game_scripts, above main) so the large NativeGame
    // local doesn't add to main()'s already-deep stack frame.
    test_boot_game_scripts();

    // ---- P6: run one of the game's OWN quests to completion (real BNK; skipped if absent) ----
    test_run_real_quest();

    // ---- P7: the game's real entity threads spawn + interact (real BNK; skipped if absent) ----
    test_entity_thread_spawns();

    // ---- P8: New Game -> gameflow reaches the childhood chapter (real BNK; skipped if absent) ----
    test_gameflow_starts_childhood();
    test_cook_scripts_package();
    test_gdb_record_lookup();
    test_game_text();
    test_cutscene_saylines();
    test_mod_menu();
    test_named_entity_sidecar();
    test_childhood_stub_census();
    test_stage2_control();
    test_stage2_navigation();
    test_stage2_animation();
    test_stage2_camera();
    test_stage3_hero_anim();

    // ---- P6: mods folder loader + Quest natives (150-bit bitset) ----
    {
        f2::NativeGame game;
        assert(game.enable_scripting());
        assert(game.script_vm->run_source(
            "Quest.SetComplete(10); Quest.SetComplete(20, true); Quest.SetComplete(20, false)"));
        assert(game.game_state.quest_completion.test(10) && !game.game_state.quest_completion.test(20));
        assert(game.script_vm->run_source("assert(Quest.IsComplete(10) and not Quest.IsComplete(11))"));

        const auto dir = std::filesystem::temp_directory_path() / "f2native_mods";
        std::filesystem::create_directories(dir);
        std::ofstream(dir / "01_quest.lua") << "Quest.SetComplete(30)\n";
        std::ofstream(dir / "02_chapter.lua") << "Game.SetChapter(7)\n";
        std::ofstream(dir / "03_broken.lua") << "this is not lua\n";  // skipped, not fatal
        const int n = game.load_mods(dir);
        assert(n == 2);  // two good mods loaded, the broken one skipped
        assert(game.game_state.quest_completion.test(30) && game.game_state.header.chapter == 7);
        std::filesystem::remove_all(dir);
        assert(game.load_mods("no_such_dir") == 0);
    }

    // ---- Integration: full NativeGame InWorld tick (movement+NPC+combat+save) ----
    {
        f2::NativeScene s;
        f2::NativeMesh ground;
        auto gv = [&](float x, float y, float z) {
            f2::NativeVertex vv; vv.position = {x, y, z}; ground.vertices.push_back(vv);
        };
        gv(-40, 0, -40); gv(40, 0, -40); gv(40, 0, 40); gv(-40, 0, 40);
        ground.indices = {0, 1, 2, 0, 2, 3};
        s.meshes.push_back(ground);
        f2::NativeMesh hero_mesh; hero_mesh.name = "hero0"; s.meshes.push_back(hero_mesh);
        f2::NativeMesh npc_mesh; npc_mesh.name = "npc0_0"; s.meshes.push_back(npc_mesh);
        f2::NativeInstance gi; gi.mesh = 0; s.instances.push_back(gi);
        f2::NativeInstance hi; hi.mesh = 1; hi.position = {0.0f, 0.0f, 0.0f}; s.instances.push_back(hi);
        f2::NativeInstance ni; ni.mesh = 2; ni.position = {0.0f, 0.0f, 2.0f}; s.instances.push_back(ni);
        s.has_hero_start = true; s.hero_start = {0.0f, 0.0f, 0.0f}; s.hero_yaw = 0.0f;

        f2::NativeGame game;
        game.scene = s;
        game.prepare_world();
        game.mode = f2::GameMode::InWorld;
        game.external_input = true;
        game.camera_controller.yaw = 0.0f;  // aim +z (toward the NPC)

        // Drive "hold forward" for ~0.5s: the hero walks +z (camera-relative).
        game.input = f2::InputState{};
        game.input.last_active_device = f2::InputDevice::Controller;
        game.input.move = {0.0f, 1.0f};
        const float z_before = game.player.position()[2];
        for (int i = 0; i < 30; ++i) game.tick(1.0 / 60.0);
        assert(game.player.position()[2] > z_before + 0.2f);   // moved forward
        // The follow camera repositioned behind the moved hero (camera writes NativeCamera).
        assert(game.camera.position[2] < game.player.position()[2]);

        // NPC saw the approaching hero -> Notice; its transform synced into the scene.
        assert(game.world.npcs.size() == 1);
        assert(game.world.npcs[0].controller.state == f2::NpcState::Notice);

        // Attack once (X press edge): the NPC takes damage and flees.
        game.input.move = {0.0f, 0.0f};
        game.input.buttons_pressed = static_cast<std::uint16_t>(f2::PadButton::X);
        game.tick(1.0 / 60.0);
        game.input.buttons_pressed = 0;
        auto* npc_e = game.world.entities.find(game.world.npcs[0].entity_uid);
        auto* npc_hp = npc_e->get<f2::HealthComponent>(f2::kTypeIdHealth);
        assert(npc_hp->health < 70.0f);                        // damaged by the swing
        assert(game.world.npcs[0].controller.state == f2::NpcState::Flee);

        // Save the whole live state, then load it into a fresh game on the same baseline.
        game.game_state.header.chapter = 3;
        auto blob = game.save_state();
        f2::NativeGame game2; game2.scene = s; game2.prepare_world();
        assert(game2.load_state(blob));
        assert(game2.game_state.header.chapter == 3);
        assert(approx(game2.player.position()[2], game.player.position()[2]));
        auto* npc_e2 = game2.world.entities.find(game2.world.npcs[0].entity_uid);
        assert(approx(npc_e2->get<f2::HealthComponent>(f2::kTypeIdHealth)->health, npc_hp->health));
    }

    // ---- Melee attack (Health.Modify via the attack volume) ----
    {
        f2::NativeScene s;
        f2::NativeMesh ground;
        auto gv = [&](float x, float y, float z) {
            f2::NativeVertex vv; vv.position = {x, y, z}; ground.vertices.push_back(vv);
        };
        gv(-30, 0, -30); gv(30, 0, -30); gv(30, 0, 30); gv(-30, 0, 30);
        ground.indices = {0, 1, 2, 0, 2, 3};
        s.meshes.push_back(ground);
        f2::NativeMesh npc_mesh; npc_mesh.name = "npc0_0"; s.meshes.push_back(npc_mesh);
        // Two villagers: one 1.5 units in +z (in front), one 1.5 in -z (behind).
        f2::NativeInstance gi; gi.mesh = 0; s.instances.push_back(gi);
        f2::NativeInstance front; front.mesh = 1; front.position = {0.0f, 0.0f, 1.5f}; s.instances.push_back(front);
        f2::NativeInstance behind; behind.mesh = 1; behind.position = {0.0f, 0.0f, -1.5f}; s.instances.push_back(behind);

        f2::NativeCollisionWorld collision; collision.build_from_scene(s);
        f2::NativeWorld w; w.spawn_from_scene(s);
        assert(w.npcs.size() == 2);
        const auto close_to = [](float a, float b) { return std::abs(a - b) < 1e-4f; };
        // npcs[0] = front (+z, instance order), npcs[1] = behind (-z).
        auto* front_e = w.entities.find(w.npcs[0].entity_uid);
        auto* front_hp = front_e->get<f2::HealthComponent>(f2::kTypeIdHealth);

        // Swing facing +z (yaw 0 -> forward (0,0,1)): hits only the one in front.
        auto res = w.melee_attack(collision, {0, 0, 0}, 0.0f);
        assert(res.hits == 1 && res.kills == 0);
        assert(close_to(front_hp->health, 45.0f));   // 70 - 25; behind NPC untouched

        // Out of range: nobody hit.
        assert(w.melee_attack(collision, {50, 0, 50}, 0.0f).hits == 0);

        // Keep swinging the front NPC to death; it becomes !alive and stops being hit.
        assert(w.melee_attack(collision, {0, 0, 0}, 0.0f).hits == 1);  // 45 -> 20
        assert(w.melee_attack(collision, {0, 0, 0}, 0.0f).kills == 1); // 20 -> 0 = kill
        assert(front_hp->is_dead() && !w.npcs[0].controller.alive);
        assert(w.melee_attack(collision, {0, 0, 0}, 0.0f).hits == 0);  // already dead

        // Non-lethal hit makes a survivor flee AWAY from the attacker.
        f2::NativeWorld w2; w2.spawn_from_scene(s);
        auto hit = w2.melee_attack(collision, {0, 0, 0}, 0.0f);  // hits front NPC (survives)
        assert(hit.hits == 1 && hit.kills == 0);
        assert(w2.npcs[0].controller.state == f2::NpcState::Flee);
        const float z0 = w2.npcs[0].controller.position()[2];   // starts at +1.5
        for (int i = 0; i < 60; ++i) w2.update_npcs(collision, {0, 0, 0}, 1.0f / 60.0f);
        assert(w2.npcs[0].controller.position()[2] > z0);        // moved further from attacker (+z)
    }

    // ---- P4 ACT: line-of-sight + NPC perception/LOD/motor ----
    {
        // Ground + a wall quad in the x=5 plane (z in [-3,3], y in [0,3]).
        f2::NativeScene s;
        f2::NativeMesh ground;
        auto gv = [&](float x, float y, float z) {
            f2::NativeVertex v; v.position = {x, y, z}; ground.vertices.push_back(v);
        };
        gv(-30, 0, -30); gv(30, 0, -30); gv(30, 0, 30); gv(-30, 0, 30);
        ground.indices = {0, 1, 2, 0, 2, 3};
        s.meshes.push_back(ground);
        f2::NativeInstance gi; gi.mesh = 0; s.instances.push_back(gi);
        f2::NativeMesh wall;
        auto wv = [&](float x, float y, float z) {
            f2::NativeVertex v; v.position = {x, y, z}; wall.vertices.push_back(v);
        };
        wv(5, 0, -3); wv(5, 0, 3); wv(5, 3, 3); wv(5, 3, -3);
        wall.indices = {0, 1, 2, 0, 2, 3};
        s.meshes.push_back(wall);
        f2::NativeInstance wi; wi.mesh = 1; s.instances.push_back(wi);
        f2::NativeCollisionWorld world; world.build_from_scene(s);

        // line_of_sight: clear across open ground; blocked through the wall.
        assert(world.line_of_sight({0, 1, 0}, {3, 1, 0}));          // both left of wall
        assert(!world.line_of_sight({0, 1, 0}, {10, 1, 0}));        // crosses x=5 wall
        assert(world.line_of_sight({0, 1, 0}, {0, 1, 10}));         // parallel, no wall

        // NPC perception: sees a near target with clear LOS; not one behind the wall.
        f2::NpcController npc;
        npc.set_position({0, 0, 0});
        assert(npc.can_see(world, {3, 0, 0}));      // clear + in range
        assert(!npc.can_see(world, {10, 0, 0}));    // wall blocks
        assert(!npc.can_see(world, {0, 0, 25}));    // out of sight_range (20)

        // LOD gate: active near the centre, inactive far away.
        assert(npc.update_lod({0, 0, 0}, 10.0f));
        npc.set_position({0, 0, 50});
        assert(!npc.update_lod({0, 0, 0}, 10.0f));

        // Stand-in brain: player near + visible -> Notice (faces the player, holds).
        npc.set_position({0, 0, 0});
        npc.update(world, {0, 0, 3}, {0, 0, 0}, 50.0f, 1.0f / 60.0f);
        assert(npc.state == f2::NpcState::Notice);
        assert(approx(npc.position()[0], 0.0f));  // did not move while noticing

        // Motor: patrol to a goal on open ground; arrives within a bounded number of steps.
        npc.set_position({-8, 0, 8});
        npc.set_patrol_goal({-2, 0, 8});
        bool arrived = false;
        for (int i = 0; i < 600 && !arrived; ++i)
            arrived = npc.move_to(world, {-2, 0, 8}, 1.0f / 60.0f);
        assert(arrived && npc.position()[0] > -3.0f);
    }

    // ---- P3: WorldArchive bidirectional primitives ----
    {
        f2::WorldArchive w(f2::ArchiveMode::Write);
        std::uint32_t u = 0xDEADBEEFu;
        float f = 3.5f;
        std::array<float, 3> v{1.0f, 2.0f, 3.0f};
        std::string s = "childhood";
        std::bitset<150> bits;
        bits.set(3); bits.set(77); bits.set(149);
        bool flag = true;
        int enumv = 7;
        w.visit(u); w.visit(f); w.visit(v); w.visit(s); w.visit(bits); w.visit(flag); w.visit_i32(enumv);
        auto blob = w.take();

        f2::WorldArchive r(blob);
        std::uint32_t u2 = 0; float f2v = 0; std::array<float, 3> v2{}; std::string s2;
        std::bitset<150> bits2; bool flag2 = false; int enumv2 = 0;
        r.visit(u2); r.visit(f2v); r.visit(v2); r.visit(s2); r.visit(bits2); r.visit(flag2); r.visit_i32(enumv2);
        assert(r.ok());
        assert(u2 == 0xDEADBEEFu && approx(f2v, 3.5f));
        assert(approx(v2[0], 1.0f) && approx(v2[2], 3.0f) && s2 == "childhood");
        assert(bits2.test(3) && bits2.test(77) && bits2.test(149) && !bits2.test(0));
        assert(flag2 && enumv2 == 7);
        // Reading past the end sets ok()=false (graceful truncation).
        std::uint32_t overrun = 0; r.visit(overrun); assert(!r.ok());
    }

    // ---- P3: game save/restore round-trip (delta over the baseline) ----
    {
        f2::NativeScene s;
        f2::NativeMesh hero_mesh; hero_mesh.name = "hero0"; s.meshes.push_back(hero_mesh);
        f2::NativeMesh npc_mesh;  npc_mesh.name = "npc0_0"; s.meshes.push_back(npc_mesh);
        f2::NativeInstance hi; hi.mesh = 0; hi.position = {5.0f, 0.0f, 5.0f}; s.instances.push_back(hi);
        f2::NativeInstance ni; ni.mesh = 1; ni.position = {0.0f, 0.0f, 0.0f}; s.instances.push_back(ni);

        f2::NativeGame game;
        game.scene = s;
        game.world.spawn_from_scene(game.scene);      // baseline
        game.player.set_position({5.0f, 0.0f, 5.0f});

        // Mutate: move player + a villager's job + set quest bits + chapter.
        game.player.set_position({42.0f, 1.0f, -7.0f});
        game.game_state.header.chapter = 2;
        game.game_state.quest_completion.set(10);
        game.game_state.quest_completion.set(120);
        auto* npc = game.world.entities.find(2);
        assert(npc != nullptr);
        auto* v = npc->get<f2::VillagerComponent>(f2::kTypeIdVillager);
        assert(v != nullptr);
        v->job = 4; v->age = 2; v->job_tag = "BLACKSMITH";
        auto* npc_tf = npc->get<f2::TransformComponent>(f2::kTypeIdTransform);
        npc_tf->position = {9.0f, 0.0f, 9.0f};

        auto blob = game.save_state();
        assert(!blob.empty());

        // Fresh game, same baseline, then load the delta.
        f2::NativeGame game2;
        game2.scene = s;
        game2.world.spawn_from_scene(game2.scene);
        assert(game2.player.position()[0] == 5.0f);  // baseline before load
        assert(game2.load_state(blob));

        // Restored: player, chapter, quest bits, villager fields, moved transform.
        assert(approx(game2.player.position()[0], 42.0f) && approx(game2.player.position()[2], -7.0f));
        assert(game2.game_state.header.chapter == 2);
        assert(game2.game_state.quest_completion.test(10) && game2.game_state.quest_completion.test(120));
        assert(!game2.game_state.quest_completion.test(11));
        auto* npc2 = game2.world.entities.find(2);
        auto* v2b = npc2->get<f2::VillagerComponent>(f2::kTypeIdVillager);
        assert(v2b->job == 4 && v2b->age == 2 && v2b->job_tag == "BLACKSMITH");
        auto* tf2 = npc2->get<f2::TransformComponent>(f2::kTypeIdTransform);
        assert(approx(tf2->position[0], 9.0f) && approx(tf2->position[2], 9.0f));
        // The NPC agent controller resynced to the restored transform (not the spawn pos).
        assert(game2.world.npcs.size() == 1);
        assert(approx(game2.world.npcs[0].controller.position()[0], 9.0f));
        assert(approx(game2.world.npcs[0].controller.position()[2], 9.0f));

        // A corrupt/short blob is rejected, not crashed.
        std::vector<std::uint8_t> bad{1, 2, 3};
        f2::NativeGame game3; game3.scene = s; game3.world.spawn_from_scene(game3.scene);
        assert(!game3.load_state(bad));
    }

    // ---- P5: runtime animation player (skin math + playback + interpolation) ----
    {
        // Identity skin matrix leaves a point unchanged; a translation moves it.
        const std::array<float, 12> identity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
        auto p = f2::AnimationPlayer::transform_point(identity, {2.0f, 3.0f, 4.0f});
        assert(approx(p[0], 2.0f) && approx(p[1], 3.0f) && approx(p[2], 4.0f));
        const std::array<float, 12> translate{1, 0, 0, 5, 0, 1, 0, 0, 0, 0, 1, 0};  // +5 x
        p = f2::AnimationPlayer::transform_point(translate, {2.0f, 3.0f, 4.0f});
        assert(approx(p[0], 7.0f));

        // A 1-bone, 2-frame clip: frame0 identity, frame1 translate +10 x. 1 fps so each
        // frame is 1s; duration 2s.
        f2::AnimClip clip;
        clip.hash = 0x1B78A889; clip.bone_count = 1; clip.frame_count = 2; clip.fps = 1.0f;
        clip.skin = { {1,0,0,0, 0,1,0,0, 0,0,1,0},      // frame 0 identity
                      {1,0,0,10, 0,1,0,0, 0,0,1,0} };   // frame 1 +10 x
        f2::SkinnedVertex v; v.position = {0.0f, 0.0f, 0.0f}; v.bones = {0,0,0,0}; v.weights = {1,0,0,0};
        std::vector<f2::SkinnedVertex> base{v};
        std::vector<std::array<float, 3>> out;

        f2::AnimationPlayer player;
        player.set_clip(&clip);
        player.skin(base, out);
        assert(approx(out[0][0], 0.0f));    // t=0 -> frame 0 -> at origin
        player.update(0.5f);                // t=0.5 -> halfway frame0->frame1
        player.skin(base, out);
        assert(approx(out[0][0], 5.0f));    // linear blend -> +5
        player.update(0.5f);                // t=1.0 -> frame 1
        player.skin(base, out);
        assert(approx(out[0][0], 10.0f));   // +10
        player.update(1.5f);               // t=2.5 -> loops to t=0.5 -> +5 again
        player.skin(base, out);
        assert(approx(out[0][0], 5.0f));

        // Locomotion selection by nearest measured root speed (idle 0 / walk 0.77 / run 4.20).
        f2::AnimClip idle; idle.hash = 0x1B78A889;
        f2::AnimClip walk; walk.hash = 0x02EE1AA7;
        f2::AnimClip run;  run.hash = 0x8C7D7F7E;
        std::vector<f2::LocomotionClip> set{{&idle, 0.0f}, {&walk, 0.77f}, {&run, 4.20f}};
        assert(f2::select_locomotion_clip(0.0f, set)->hash == 0x1B78A889u);   // standing -> idle
        assert(f2::select_locomotion_clip(0.8f, set)->hash == 0x02EE1AA7u);   // ~walk speed -> walk
        assert(f2::select_locomotion_clip(4.0f, set)->hash == 0x8C7D7F7Eu);   // sprint -> run
        assert(f2::select_locomotion_clip(2.0f, set)->hash == 0x02EE1AA7u);   // mid -> nearer walk
        assert(f2::select_locomotion_clip(3.0f, set)->hash == 0x8C7D7F7Eu);   // faster -> nearer run
        assert(f2::select_locomotion_clip(1.0f, {}) == nullptr);

        // Weighted blend across two bones (0.5 each): identity + translate -> +2.5 x.
        f2::AnimClip clip2;
        clip2.bone_count = 2; clip2.frame_count = 1; clip2.fps = 30.0f;
        clip2.skin = { {1,0,0,0, 0,1,0,0, 0,0,1,0},  {1,0,0,5, 0,1,0,0, 0,0,1,0} };
        f2::SkinnedVertex v2; v2.position = {0,0,0}; v2.bones = {0,1,0,0}; v2.weights = {0.5f,0.5f,0,0};
        std::vector<f2::SkinnedVertex> base2{v2};
        f2::AnimationPlayer p2; p2.set_clip(&clip2); p2.skin(base2, out);
        assert(approx(out[0][0], 2.5f));
    }

    // ---- P4: NPC/villager entity substrate (registry identities + tagging) ----
    {
        f2::ComponentRegistry reg;
        reg.seed_defaults();
        // Villager + the AI family register with their retail typeIds.
        assert(reg.lookup(f2::gdb::kCompVillager)->type_id == f2::kTypeIdVillager);      // 26
        assert(reg.lookup(f2::gdb::kCompAIBrain)->type_id == f2::kTypeIdAIBrain);        // 71
        assert(reg.lookup(f2::gdb::kCompPerception)->type_id == f2::kTypeIdPerception);  // 61
        assert(reg.lookup(f2::gdb::kCompNavigation)->type_id == f2::kTypeIdNavigation);  // 60
        assert(reg.lookup(f2::gdb::kCompCreatureGenerator)->type_id == f2::kTypeIdCreatureGenerator); // 51

        // spawn_from_scene tags hero + villager meshes by their cooked names.
        f2::NativeScene s;
        f2::NativeMesh hero_mesh; hero_mesh.name = "hero0"; s.meshes.push_back(hero_mesh);
        f2::NativeMesh npc_mesh;  npc_mesh.name = "npc0_0"; s.meshes.push_back(npc_mesh);
        f2::NativeInstance hi; hi.mesh = 0; s.instances.push_back(hi);
        f2::NativeInstance ni; ni.mesh = 1; s.instances.push_back(ni);

        f2::NativeWorld w;
        w.spawn_from_scene(s);
        assert(w.entities.entity_count() == 2);
        assert(w.hero != nullptr);  // hero pointer now set from the "hero0" mesh
        assert(w.hero->component_by_typeid(f2::kTypeIdVillager) == nullptr);  // hero is not a villager
        // The npc entity (uid 2) carries a VillagerComponent.
        f2::NativeEntity* npc = w.entities.find(2);
        assert(npc != nullptr);
        auto* v = npc->get<f2::VillagerComponent>(f2::kTypeIdVillager);
        assert(v != nullptr);
    }

    // ---- P2: wall raycast (camera-collision primitive) ----
    {
        f2::NativeScene s;
        f2::NativeMesh ground;
        auto gv = [&](float x, float y, float z) {
            f2::NativeVertex v; v.position = {x, y, z}; ground.vertices.push_back(v);
        };
        gv(-20, 0, -20); gv(20, 0, -20); gv(20, 0, 20); gv(-20, 0, 20);
        ground.indices = {0, 1, 2, 0, 2, 3};
        s.meshes.push_back(ground);
        f2::NativeInstance gi; gi.mesh = 0; s.instances.push_back(gi);
        // Vertical wall quad in the x=5 plane, z in [-2,2], y in [0,3].
        f2::NativeMesh wall;
        auto wv = [&](float x, float y, float z) {
            f2::NativeVertex v; v.position = {x, y, z}; wall.vertices.push_back(v);
        };
        wv(5, 0, -2); wv(5, 0, 2); wv(5, 3, 2); wv(5, 3, -2);
        wall.indices = {0, 1, 2, 0, 2, 3};
        s.meshes.push_back(wall);
        f2::NativeInstance wi; wi.mesh = 1; s.instances.push_back(wi);

        f2::NativeCollisionWorld world;
        world.build_from_scene(s);
        assert(world.wall_triangle_count() >= 2);
        // Ray toward +x at wall height hits at ~5.
        assert(approx(world.raycast_walls({0, 1, 0}, {1, 0, 0}, 20.0f), 5.0f));
        // Opposite direction misses -> returns max_dist.
        assert(approx(world.raycast_walls({0, 1, 0}, {-1, 0, 0}, 20.0f), 20.0f));
        // Above the wall (y=5 > top 3) misses.
        assert(approx(world.raycast_walls({0, 5, 0}, {1, 0, 0}, 20.0f), 20.0f));

        // Camera boom pulls in when a wall is behind the hero.
        f2::NativeScene s2;
        s2.meshes.push_back(ground);
        f2::NativeInstance gi2; gi2.mesh = 0; s2.instances.push_back(gi2);
        f2::NativeMesh backwall;  // z = -2 plane, x in [-2,2], y in [0,3]
        auto bv = [&](float x, float y, float z) {
            f2::NativeVertex v; v.position = {x, y, z}; backwall.vertices.push_back(v);
        };
        bv(-2, 0, -2); bv(2, 0, -2); bv(2, 3, -2); bv(-2, 3, -2);
        backwall.indices = {0, 1, 2, 0, 2, 3};
        s2.meshes.push_back(backwall);
        f2::NativeInstance wi2; wi2.mesh = 1; s2.instances.push_back(wi2);
        f2::NativeCollisionWorld world2;
        world2.build_from_scene(s2);

        f2::CameraController cam;
        cam.yaw = 0.0f; cam.pitch = 0.0f;  // forward = +z, so eye pulls back to -z
        cam.config.distance = 4.5f;
        cam.update({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f}, false, 1.0f / 60.0f, &world2);
        // Ideal eye would be z=-4.5; the wall at z=-2 pulls it in to ~-1.7 (2 - margin 0.3).
        assert(cam.position[2] > -2.0f);           // in front of the wall (not clipped)
        assert(approx(cam.position[2], -1.7f));
    }

    // ---- P2: follow-camera pose matches the renderer forward convention ----
    {
        f2::CameraController cam;
        cam.yaw = 0.0f; cam.pitch = 0.0f;
        cam.config.distance = 4.0f; cam.config.height = 1.6f;
        std::array<float, 3> hero{0.0f, 0.0f, 0.0f};
        cam.update(hero, {0.0f, 0.0f}, /*mouse*/ false, 1.0f / 60.0f);
        // yaw=pitch=0 -> forward = (0,0,1); camera pulls back along -z from target.
        assert(approx(cam.target[1], 1.6f));
        assert(approx(cam.position[2], -4.0f));
        assert(approx(cam.position[1], 1.6f));
        assert(approx(cam.fov_y(), 1.22171938f));  // GROUNDED 70deg
        // Right-stick yaw integrates over time.
        float y0 = cam.yaw;
        cam.update(hero, {1.0f, 0.0f}, false, 0.1f);
        assert(cam.yaw > y0);
    }

    // ---- P2: player camera-relative desired velocity ----
    {
        // Facing yaw=0 (forward = +z). Pushing forward (move.y=1) -> +z velocity.
        auto v = f2::NativePlayer::desired_velocity({0.0f, 1.0f}, 0.0f, 2.5f);
        assert(approx(v[0], 0.0f) && approx(v[1], 2.5f));
        // Strafe right (move.x=1) at yaw=0 -> +x velocity.
        v = f2::NativePlayer::desired_velocity({1.0f, 0.0f}, 0.0f, 2.5f);
        assert(approx(v[0], 2.5f) && approx(v[1], 0.0f));
        // Yaw 90deg rotates "forward" to +x.
        v = f2::NativePlayer::desired_velocity({0.0f, 1.0f}, 3.14159265f / 2.0f, 2.0f);
        assert(approx(v[0], 2.0f) && std::abs(v[1]) < 1e-4f);
    }

    std::filesystem::remove(path);
    return 0;
}
