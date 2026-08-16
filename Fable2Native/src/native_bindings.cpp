#include "f2/native_bindings.h"

#include "f2/native_bnk.h"
#include "f2/native_game.h"
#include "f2/native_gdb_hash.h"
#include "f2/native_script.h"

#include <array>
#include <cstdint>
#include <random>
#include <string>

namespace f2 {

namespace {
NativeGame* game_of(NativeScriptVM& vm) { return static_cast<NativeGame*>(vm.user_data()); }

HealthComponent* npc_health(NativeGame& g, int i) {
    if (i < 0 || i >= static_cast<int>(g.world.npcs.size())) return nullptr;
    NativeEntity* e = g.world.entities.find(g.world.npcs[static_cast<std::size_t>(i)].entity_uid);
    return e ? e->get<HealthComponent>(kTypeIdHealth) : nullptr;
}

// Deterministic host RNG for Debug.GetRandom* (no retail seed source — flagged).
std::mt19937& rng() {
    static std::mt19937 g(0xF2B2A123u);
    return g;
}
}  // namespace

void register_native_api(NativeScriptVM& vm, NativeGame& /*game*/) {
    // ---- Debug ----
    vm.register_native("Debug", "Log", [](NativeScriptVM& v) -> int {
        if (auto* g = game_of(v)) g->script_log.emplace_back(v.arg_string(1));
        return 0;
    });
    vm.register_native("Debug", "GetRandomFloat", [](NativeScriptVM& v) -> int {
        v.push_number(std::uniform_real_distribution<double>(0.0, 1.0)(rng()));  // FLAGGED: host RNG
        return 1;
    });
    vm.register_native("Debug", "GetRandomNumber", [](NativeScriptVM& v) -> int {
        const int lo = static_cast<int>(v.arg_number(1));
        const int hi = static_cast<int>(v.arg_number(2));
        v.push_number(lo <= hi ? std::uniform_int_distribution<int>(lo, hi)(rng()) : lo);
        return 1;
    });

    // ---- Game ----
    vm.register_native("Game", "Elapsed", [](NativeScriptVM& v) -> int {
        auto* g = game_of(v);
        v.push_number(g ? g->elapsed_seconds : 0.0);
        return 1;
    });
    vm.register_native("Game", "GetTimeStep", [](NativeScriptVM& v) -> int {
        v.push_number(1.0 / 60.0);  // FLAGGED: the fixed sim step (no per-call dt available)
        return 1;
    });
    vm.register_native("Game", "SetChapter", [](NativeScriptVM& v) -> int {
        if (auto* g = game_of(v)) g->game_state.header.chapter = static_cast<std::uint32_t>(v.arg_number(1));
        return 0;
    });
    vm.register_native("Game", "GetChapter", [](NativeScriptVM& v) -> int {
        auto* g = game_of(v);
        v.push_number(g ? static_cast<double>(g->game_state.header.chapter) : 0.0);
        return 1;
    });

    // ---- Player ----
    vm.register_native("Player", "GetPosition", [](NativeScriptVM& v) -> int {
        auto* g = game_of(v);
        const std::array<float, 3> p = g ? g->player.position() : std::array<float, 3>{};
        v.push_number(p[0]); v.push_number(p[1]); v.push_number(p[2]);
        return 3;
    });
    vm.register_native("Player", "SetPosition", [](NativeScriptVM& v) -> int {
        if (auto* g = game_of(v)) {
            g->player.set_position({static_cast<float>(v.arg_number(1)),
                                    static_cast<float>(v.arg_number(2)),
                                    static_cast<float>(v.arg_number(3))});
        }
        return 0;
    });

    // ---- World / NPCs ----
    vm.register_native("World", "NpcCount", [](NativeScriptVM& v) -> int {
        auto* g = game_of(v);
        v.push_number(g ? static_cast<double>(g->world.npcs.size()) : 0.0);
        return 1;
    });
    vm.register_native("World", "NpcPosition", [](NativeScriptVM& v) -> int {
        auto* g = game_of(v);
        const int i = static_cast<int>(v.arg_number(1));
        std::array<float, 3> p{0.0f, 0.0f, 0.0f};
        if (g && i >= 0 && i < static_cast<int>(g->world.npcs.size()))
            p = g->world.npcs[static_cast<std::size_t>(i)].controller.position();
        v.push_number(p[0]); v.push_number(p[1]); v.push_number(p[2]);
        return 3;
    });
    vm.register_native("World", "DamageNpc", [](NativeScriptVM& v) -> int {
        auto* g = game_of(v);
        const int i = static_cast<int>(v.arg_number(1));
        const float amount = static_cast<float>(v.arg_number(2));
        bool killed = false;
        if (g) {
            if (auto* h = npc_health(*g, i)) {
                killed = h->modify(-amount);
                if (killed) g->world.npcs[static_cast<std::size_t>(i)].controller.alive = false;
            }
        }
        v.push_bool(killed);
        return 1;
    });
    vm.register_native("World", "GetNpcHealth", [](NativeScriptVM& v) -> int {
        auto* g = game_of(v);
        auto* h = g ? npc_health(*g, static_cast<int>(v.arg_number(1))) : nullptr;
        v.push_number(h ? static_cast<double>(h->health) : 0.0);
        return 1;
    });
    vm.register_native("World", "SetNpcHealth", [](NativeScriptVM& v) -> int {
        auto* g = game_of(v);
        if (auto* h = g ? npc_health(*g, static_cast<int>(v.arg_number(1))) : nullptr)
            h->health = static_cast<float>(v.arg_number(2));
        return 0;
    });

    // ---- Camera ----
    vm.register_native("Camera", "GetPosition", [](NativeScriptVM& v) -> int {
        auto* g = game_of(v);
        const std::array<float, 3> p = g ? g->camera.position : std::array<float, 3>{};
        v.push_number(p[0]); v.push_number(p[1]); v.push_number(p[2]);
        return 3;
    });

    // ---- Quest (the 150-bit completion bitset) ----
    vm.register_native("Quest", "SetComplete", [](NativeScriptVM& v) -> int {
        auto* g = game_of(v);
        const int i = static_cast<int>(v.arg_number(1));
        const bool val = v.arg_count() >= 2 ? v.arg_bool(2) : true;
        if (g && i >= 0 && i < 150) g->game_state.quest_completion.set(static_cast<std::size_t>(i), val);
        return 0;
    });
    vm.register_native("Quest", "IsComplete", [](NativeScriptVM& v) -> int {
        auto* g = game_of(v);
        const int i = static_cast<int>(v.arg_number(1));
        v.push_bool(g && i >= 0 && i < 150 &&
                    g->game_state.quest_completion.test(static_cast<std::size_t>(i)));
        return 1;
    });
    vm.register_native("Quest", "CountComplete", [](NativeScriptVM& v) -> int {
        auto* g = game_of(v);
        v.push_number(g ? static_cast<double>(g->game_state.quest_completion.count()) : 0.0);
        return 1;
    });
}

namespace {
// Get-or-create the hero entity and return its uid. Prefers an existing scene-tagged hero
// (world.hero); otherwise makes a bare entity with a Transform seeded from the player, so
// GetPlayerHero() always yields a usable handle even in a headless/no-hero-mesh world.
std::uint64_t ensure_hero(NativeGame& g) {
    if (g.hero_uid != 0 && g.world.entities.find(g.hero_uid)) return g.hero_uid;
    if (g.world.hero) {
        g.hero_uid = g.world.hero->uid;
    } else {
        NativeEntity& e = g.world.entities.create_entity();
        auto tf = std::make_unique<TransformComponent>();
        tf->position = g.player.position();
        e.add_component(std::move(tf));
        g.hero_uid = e.uid;
    }
    g.entity_names[g.hero_uid] = "Hero";
    return g.hero_uid;
}

TransformComponent* entity_transform(NativeGame& g, std::uint64_t uid) {
    NativeEntity* e = g.world.entities.find(uid);
    return e ? e->get<TransformComponent>(kTypeIdTransform) : nullptr;
}
}  // namespace

void register_game_systems_api(NativeScriptVM& vm, NativeGame& /*game*/) {
    // ---- manager registration (the game's Lua managers hand themselves to the engine) ----
    // SetGeneralScriptManager(tbl)/SetAIManager(tbl) capture the table's Update method;
    // SetQuestUpdateFunction(fn) captures the function directly. The InWorld tick resumes
    // these each frame (NativeGame wires them to script_systems after boot).
    vm.register_global("SetGeneralScriptManager", [](NativeScriptVM& v) -> int {
        if (auto* g = game_of(v)) { v.unref(g->general_update_ref); g->general_update_ref = v.ref_arg_field(1, "Update"); }
        return 0;
    });
    vm.register_global("SetQuestUpdateFunction", [](NativeScriptVM& v) -> int {
        if (auto* g = game_of(v)) { v.unref(g->quest_update_ref); g->quest_update_ref = v.ref_arg(1); }
        return 0;
    });
    vm.register_global("SetAIManager", [](NativeScriptVM& v) -> int {
        if (auto* g = game_of(v)) { v.unref(g->ai_update_ref); g->ai_update_ref = v.ref_arg_field(1, "Update"); }
        return 0;
    });

    // ---- hero + entity object model ----
    vm.register_global("GetPlayerHero", [](NativeScriptVM& v) -> int {
        auto* g = game_of(v);
        v.push_handle("Entity", g ? ensure_hero(*g) : 0);
        return 1;
    });
    vm.register_global("GetIDFromEntity", [](NativeScriptVM& v) -> int {
        v.push_number(static_cast<double>(v.arg_handle(1)));
        return 1;
    });
    // Debug.CreateEntityAt(class, name, x, y, z) — spawn from a class name at a position
    // (gameflow.txt: Debug.CreateEntityAt("CreatureCrummi","Crummi", HeroEntity:GetPosition())
    // where GetPosition returns 3 numbers). Returns the new entity handle. FLAGGED: `class`
    // is recorded as the name but not yet resolved to a GDB archetype (no GDB cook here).
    vm.register_native("Debug", "CreateEntityAt", [](NativeScriptVM& v) -> int {
        auto* g = game_of(v);
        if (!g) { v.push_nil(); return 1; }
        NativeEntity& e = g->world.entities.create_entity();
        auto tf = std::make_unique<TransformComponent>();
        tf->position = {static_cast<float>(v.arg_number(3)), static_cast<float>(v.arg_number(4)),
                        static_cast<float>(v.arg_number(5))};
        e.add_component(std::move(tf));
        const char* name = v.arg_string(2);
        g->entity_names[e.uid] = (name && *name) ? name : v.arg_string(1);
        v.push_handle("Entity", e.uid);
        return 1;
    });

    vm.register_object_method("Entity", "GetPosition", [](NativeScriptVM& v) -> int {
        auto* g = game_of(v);
        TransformComponent* t = g ? entity_transform(*g, v.arg_handle(1)) : nullptr;
        const std::array<float, 3> p = t ? t->position : std::array<float, 3>{};
        v.push_number(p[0]); v.push_number(p[1]); v.push_number(p[2]);
        return 3;
    });
    vm.register_object_method("Entity", "SetPosition", [](NativeScriptVM& v) -> int {
        auto* g = game_of(v);
        if (TransformComponent* t = g ? entity_transform(*g, v.arg_handle(1)) : nullptr)
            t->position = {static_cast<float>(v.arg_number(2)), static_cast<float>(v.arg_number(3)),
                           static_cast<float>(v.arg_number(4))};
        return 0;
    });
    vm.register_object_method("Entity", "GetName", [](NativeScriptVM& v) -> int {
        auto* g = game_of(v);
        auto it = g ? g->entity_names.find(v.arg_handle(1)) : std::unordered_map<std::uint64_t, std::string>::iterator{};
        v.push_string((g && it != g->entity_names.end()) ? it->second.c_str() : "");
        return 1;
    });
    vm.register_object_method("Entity", "GetID", [](NativeScriptVM& v) -> int {
        v.push_number(static_cast<double>(v.arg_handle(1)));
        return 1;
    });
    vm.register_object_method("Entity", "IsAlive", [](NativeScriptVM& v) -> int {
        auto* g = game_of(v);
        const std::uint64_t uid = v.arg_handle(1);
        NativeEntity* e = g ? g->world.entities.find(uid) : nullptr;
        auto* h = e ? e->get<HealthComponent>(kTypeIdHealth) : nullptr;
        const bool killed = g && g->killed_entities.count(uid) != 0;
        v.push_bool(e != nullptr && !killed && (h == nullptr || !h->is_dead()));
        return 1;
    });
    vm.register_object_method("Entity", "Kill", [](NativeScriptVM& v) -> int {
        auto* g = game_of(v);
        const std::uint64_t uid = v.arg_handle(1);
        if (g) {
            g->killed_entities.insert(uid);  // marks the entity dead for IsAlive()
            if (NativeEntity* e = g->world.entities.find(uid))
                if (auto* h = e->get<HealthComponent>(kTypeIdHealth)) h->health = 0.0f;
        }
        // NOTE: the retail MESSAGE_EVENT_KILLED is posted by the combat/death system with the
        // enum id from MessageEventEnum.lua. We don't fabricate it with a guessed C++ constant
        // — producers post symbolically from Lua via MessageEvents.PostMessage.
        return 0;
    });
    vm.register_object_method("Entity", "GetCorpse", [](NativeScriptVM& v) -> int {
        v.push_nil();  // no corpse entity model yet (scripts guard with IsAlive first)
        return 1;
    });
    // Save-tagging + level-persistence hooks entity threads call at creation. No-ops until the
    // save subsystem is wired (FLAGGED) — they gate persistence, not gameplay logic.
    vm.register_object_method("Entity", "SetAsLevelSaving", [](NativeScriptVM&) -> int { return 0; });

    // ---- MessageEvents queue (the central quest poll) ----
    // IsMessagePosted/IsMessageSentTo/IsMessageSentBy return the newest matching Event
    // (id > lastSeenId), or nil — the questmanager.lua wait idiom. Event handles carry the
    // message id; Event methods read the bus.
    vm.register_native("MessageEvents", "GetMostRecentMessageID", [](NativeScriptVM& v) -> int {
        auto* g = game_of(v);
        v.push_number(g ? static_cast<double>(g->messages.most_recent_id()) : 0.0);
        return 1;
    });
    // These return TWO values: (posted, event). The retail bytecode tests the first as a
    // boolean and calls :GetID() on the SECOND (questmanager.lua QuestEntityThreadBase.Update:
    // `CALL IsMessageSentTo ret=2; TEST R1; SELF R2['GetID']`). We return the event in BOTH
    // slots (or nil,nil) so both 2-return sites and single-return sites (`local ev = ...`)
    // work. Any of type/to/by == 0 means "don't care".
    vm.register_native("MessageEvents", "IsMessagePosted", [](NativeScriptVM& v) -> int {
        auto* g = game_of(v);
        const int type = static_cast<int>(v.arg_number(1));
        const std::uint32_t after = static_cast<std::uint32_t>(v.arg_number(2));
        const GameMessage* m = g ? g->messages.find(type, after, 0, 0) : nullptr;
        if (m) { v.push_handle("Event", m->id); v.push_handle("Event", m->id); }
        else { v.push_nil(); v.push_nil(); }
        return 2;
    });
    vm.register_native("MessageEvents", "IsMessageSentTo", [](NativeScriptVM& v) -> int {
        auto* g = game_of(v);
        const int type = static_cast<int>(v.arg_number(1));
        const std::uint64_t to = v.arg_handle(2);
        const std::uint32_t after = static_cast<std::uint32_t>(v.arg_number(3));
        const GameMessage* m = g ? g->messages.find(type, after, to, 0) : nullptr;
        if (m) { v.push_handle("Event", m->id); v.push_handle("Event", m->id); }
        else { v.push_nil(); v.push_nil(); }
        return 2;
    });
    vm.register_native("MessageEvents", "IsMessageSentBy", [](NativeScriptVM& v) -> int {
        auto* g = game_of(v);
        const int type = static_cast<int>(v.arg_number(1));
        const std::uint64_t by = v.arg_handle(2);
        const std::uint32_t after = static_cast<std::uint32_t>(v.arg_number(3));
        const GameMessage* m = g ? g->messages.find(type, after, 0, by) : nullptr;
        if (m) { v.push_handle("Event", m->id); v.push_handle("Event", m->id); }
        else { v.push_nil(); v.push_nil(); }
        return 2;
    });
    // "Nothing happened" queries the managers poll every frame. These MUST return real
    // falsy/empty values, not the auto-stub's truthy black-hole — otherwise, e.g.,
    // IsEntityUnloaded sees a phantom "destroyed" message and terminates live entity threads.
    vm.register_native("MessageEvents", "GetDestroyedMessageFromEntity", [](NativeScriptVM& v) -> int {
        v.push_nil(); v.push_nil();   // (message, id) = (nil, nil): the entity was not destroyed
        return 2;
    });
    vm.register_native("MessageEvents", "GetActivatedEntityMessages", [](NativeScriptVM& v) -> int {
        v.push_new_table();           // empty {} — no activations (satisfies both pairs() and ~=0)
        return 1;
    });
    vm.register_native("MessageEvents", "GetAllMessages", [](NativeScriptVM& v) -> int {
        v.push_new_table();           // empty {} — nothing to iterate
        return 1;
    });

    // Producer side (engine/scripts post): PostMessage(type[, extra][, sentBy][, sentTo]).
    vm.register_native("MessageEvents", "PostMessage", [](NativeScriptVM& v) -> int {
        auto* g = game_of(v);
        if (g) {
            const int type = static_cast<int>(v.arg_number(1));
            const double extra = v.arg_number(2);
            g->messages.post(type, v.arg_handle(3), v.arg_handle(4), extra);
        }
        return 0;
    });

    vm.register_object_method("Event", "GetID", [](NativeScriptVM& v) -> int {
        v.push_number(static_cast<double>(v.arg_handle(1)));
        return 1;
    });
    vm.register_object_method("Event", "GetExtraDataAsNumber", [](NativeScriptVM& v) -> int {
        auto* g = game_of(v);
        const GameMessage* m = g ? g->messages.by_id(static_cast<std::uint32_t>(v.arg_handle(1))) : nullptr;
        v.push_number(m ? m->extra : 0.0);
        return 1;
    });
    vm.register_object_method("Event", "GetEntitySentBy", [](NativeScriptVM& v) -> int {
        auto* g = game_of(v);
        const GameMessage* m = g ? g->messages.by_id(static_cast<std::uint32_t>(v.arg_handle(1))) : nullptr;
        v.push_handle("Entity", m ? m->sent_by : 0);
        return 1;
    });

    // ---- gameflow / timing (grounded load-bearing natives) ----
    // IsToStartGameflow -> true = "fresh game" (gameflow_progression.txt:201); enters the
    // gameflow startup path rather than a save-load path.
    vm.register_global("IsToStartGameflow", [](NativeScriptVM& v) -> int { v.push_bool(true); return 1; });
    // GetPlatform() -> Platform.Win32. The value is arbitrary but MUST equal Platform.Win32
    // (defined in boot before the auto-stub) so questsetupscript's platform switch matches
    // and its "unknown platform" assert doesn't fire. FLAGGED engineering constant.
    vm.register_global("GetPlatform", [](NativeScriptVM& v) -> int { v.push_number(2.0); return 1; });
    // FNVHash(str) -> the game's FNV-1 hash (native_gdb_hash fnv1, basis 0x811C9DC5). Used by
    // the save/permanents machinery to key tables. Real (not a stub) so keys are stable.
    vm.register_global("FNVHash", [](NativeScriptVM& v) -> int {
        v.push_number(static_cast<double>(gdb::fnv1(v.arg_string(1))));
        return 1;
    });
    vm.register_native("Timing", "GetWorldFrame", [](NativeScriptVM& v) -> int {
        auto* g = game_of(v);
        v.push_number(g ? g->elapsed_seconds * 60.0 : 0.0);  // 60 Hz sim
        return 1;
    });
    vm.register_native("Timing", "GetTickRate", [](NativeScriptVM& v) -> int { v.push_number(60.0); return 1; });
    // Debug.Error surfaces script/coroutine errors the managers would otherwise swallow
    // (QuestManager.Update routes a failed coroutine.resume here). Captured to script_log.
    vm.register_native("Debug", "Error", [](NativeScriptVM& v) -> int {
        if (auto* g = game_of(v)) g->script_log.push_back(std::string("[Debug.Error] ") + v.arg_string(1));
        return 0;
    });

    // ---- SearchTools (entity queries) ----
    // QuestThreadBase.GetAllEntitiesWithName = StartNewSearch -> FilterWithName ->
    // GetSearchResults. We resolve the name filter against the world's named entities
    // (entity_names), so a quest's StartNewEntityThread spawns a real entity thread per
    // matching entity. FLAGGED: only a name filter is supported (no area/component filters
    // yet); entities acquire names from Debug.CreateEntityAt + the hero + scene tagging.
    vm.register_native("SearchTools", "StartNewSearch", [](NativeScriptVM& v) -> int {
        auto* g = game_of(v);
        if (!g) { v.push_number(0.0); return 1; }
        g->search_filters.emplace_back();               // new handle with an empty filter
        v.push_number(static_cast<double>(g->search_filters.size()));  // 1-based handle
        return 1;
    });
    vm.register_native("SearchTools", "FilterWithName", [](NativeScriptVM& v) -> int {
        auto* g = game_of(v);
        const int h = static_cast<int>(v.arg_number(1));
        const char* name = v.arg_string(2);
        if (g && h >= 1 && h <= static_cast<int>(g->search_filters.size()))
            g->search_filters[static_cast<std::size_t>(h - 1)] = name ? name : "";
        return 0;
    });
    // No script-filter support yet: a script filter narrows an existing set, so leaving it a
    // no-op keeps the (name-filtered) set intact rather than dropping it.
    vm.register_native("SearchTools", "FilterWithScriptFilter", [](NativeScriptVM&) -> int { return 0; });
    vm.register_native("SearchTools", "GetSearchResults", [](NativeScriptVM& v) -> int {
        auto* g = game_of(v);
        const int h = static_cast<int>(v.arg_number(1));
        std::vector<std::uint64_t> matches;
        if (g && h >= 1 && h <= static_cast<int>(g->search_filters.size())) {
            const std::string& want = g->search_filters[static_cast<std::size_t>(h - 1)];
            if (!want.empty())
                for (const auto& [uid, name] : g->entity_names)
                    if (name == want) matches.push_back(uid);
        }
        v.push_handle_list("Entity", matches.data(), matches.size());
        return 1;
    });

    // Capture the game's own print() output so quest progress is observable (and doesn't
    // spam stdout). Concatenates its args tab-separated, like Lua's print.
    vm.register_global("print", [](NativeScriptVM& v) -> int {
        auto* g = game_of(v);
        if (!g) return 0;
        std::string line;
        const int n = v.arg_count();
        for (int i = 1; i <= n; ++i) { if (i > 1) line += '\t'; line += v.arg_string(i); }
        g->script_log.push_back(std::move(line));
        return 0;
    });
}

void register_boot_api(NativeScriptVM& vm, NativeGame& /*game*/) {
    // RunScript(name): pull the named LuaQ chunk from the game's script BNK and run it.
    // De-duplicated (a script only loads once) with a hard cap as a runaway backstop.
    // This is the retail lhRunStartupScripts -> generalsetupscript -> RunScript chain.
    vm.register_global("RunScript", [](NativeScriptVM& v) -> int {
        auto* g = game_of(v);
        if (!g || !g->script_bnk) return 0;
        const char* name = v.arg_string(1);
        if (!name || !*name) return 0;
        const std::string key = BnkReader::normalize(name);
        // Skip the project's own mod-menu hook (MyConsoleHook0) — it is injected tooling that
        // polls Debug.Mod/right-stick before a world exists (the "stall" natives), not stock
        // game logic. Bringing up the real quests doesn't need it.
        if (key.find("myconsolehook") != std::string::npos) return 0;
        for (const auto& s : g->loaded_scripts) {
            if (s == key) return 0;  // already loaded
        }
        if (g->loaded_scripts.size() >= 600) return 0;  // runaway backstop
        g->loaded_scripts.push_back(key);
        std::vector<std::uint8_t> bytes = g->script_bnk->extract(name);
        if (!bytes.empty()) {
            v.run_bytecode(bytes.data(), bytes.size(), ("=" + key).c_str());
        }
        return 0;
    });
}

}  // namespace f2
