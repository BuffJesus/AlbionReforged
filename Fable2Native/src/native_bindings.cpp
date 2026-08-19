#include "f2/native_bindings.h"

#include "f2/native_bnk.h"
#include "f2/native_game.h"
#include "f2/native_gdb_hash.h"
#include "f2/native_script.h"

#include <array>
#include <cmath>
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

NpcAgent* find_agent(NativeGame& g, std::uint64_t uid) {
    for (NpcAgent& a : g.world.npcs)
        if (a.entity_uid == uid) return &a;
    return nullptr;
}

// A script that navigates an entity (Navigation.MoveTo*) lazily gets a nav agent: a controller
// seeded from the entity's current transform. This lets script-created entities (Debug.
// CreateEntityAt'd allies/dog) move, not just the baked-instance NPCs from spawn_from_scene.
NpcAgent& ensure_agent(NativeGame& g, std::uint64_t uid) {
    if (NpcAgent* a = find_agent(g, uid)) return *a;
    NpcAgent agent;
    agent.entity_uid = uid;
    if (TransformComponent* t = entity_transform(g, uid)) agent.controller.set_position(t->position);
    g.world.npcs.push_back(agent);
    return g.world.npcs.back();
}

// Map an ENavigationSpeed tier (0..8) to a ground speed (wu/s). GROUNDED anchors: WALK=0.77,
// RUN=4.20 (measured clip root speeds, anim_runtime_sampler_re.txt §B). FLAGGED: the other
// tiers are an engineering interpolation (the retail per-tier speeds aren't RE'd).
float speed_for_nav_tier(int tier) {
    switch (tier) {
        case 0: return 0.0f;   // NAV_SPEED_HALT
        case 1: return 0.40f;  // SLOW_WALK  (FLAGGED)
        case 2: return 0.77f;  // WALK       (GROUNDED)
        case 3: return 1.20f;  // FAST_WALK  (FLAGGED)
        case 4: return 2.50f;  // SLOW_RUN   (FLAGGED)
        case 5: return 4.20f;  // RUN        (GROUNDED)
        case 6: return 5.00f;  // FAST_RUN   (FLAGGED)
        default: return 6.00f; // SPRINT / MAX (FLAGGED)
    }
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
    // Debug.CreateEntityAt(class, name, position) — spawn from a class name at a position. The
    // third argument is a CVector3 in the game's own calls (gameflow.txt:
    // `Debug.CreateEntityAt("ObjectLimboInventory", "", CVector3(0, 0, 0))` and
    // `Debug.CreateEntityAt("CreatureCrummi", "Crummi", QuestManager.HeroEntity:GetPosition())`),
    // so arg_vector3 reads that form — and still accepts three loose scalars for the port's own
    // callers. Returns the new entity handle. FLAGGED: `class` is recorded as the name but not yet
    // resolved to a GDB archetype (no GDB cook here), so the entity has no mesh/AI/appearance.
    vm.register_native("Debug", "CreateEntityAt", [](NativeScriptVM& v) -> int {
        auto* g = game_of(v);
        if (!g) { v.push_nil(); return 1; }
        NativeEntity& e = g->world.entities.create_entity();
        auto tf = std::make_unique<TransformComponent>();
        double pos[3] = {0.0, 0.0, 0.0};
        v.arg_vector3(3, pos);
        tf->position = {static_cast<float>(pos[0]), static_cast<float>(pos[1]),
                        static_cast<float>(pos[2])};
        e.add_component(std::move(tf));
        const char* name = v.arg_string(2);
        g->entity_names[e.uid] = (name && *name) ? name : v.arg_string(1);
        v.push_handle("Entity", e.uid);
        return 1;
    });

    // Entity:GetPosition() -> CVector3 (ONE value, not three scalars). Grounded in the game's own
    // code, which does vector arithmetic straight on the result:
    //     QuestManager.HeroEntity:GetPosition() + CVector3(0, 0, 24)     (qc010_childhood PooCam)
    // `number + table` dispatches to CVector3's __add with a NUMBER left operand, which is exactly
    // the measured failure ("attempt to index local 'a' (a number value)") when this returned 3
    // scalars. The same contract shows up in gameflow.txt, which passes a position straight into a
    // vector parameter: Debug.CreateEntityAt("CreatureCrummi", "Crummi", HeroEntity:GetPosition())
    // alongside Debug.CreateEntityAt("ObjectLimboInventory", "", CVector3(0, 0, 0)).
    vm.register_object_method("Entity", "GetPosition", [](NativeScriptVM& v) -> int {
        auto* g = game_of(v);
        TransformComponent* t = g ? entity_transform(*g, v.arg_handle(1)) : nullptr;
        const std::array<float, 3> p = t ? t->position : std::array<float, 3>{};
        v.push_vector3(p[0], p[1], p[2]);
        return 1;
    });
    // The scalar form, for the port's own code/tests that want the components without a vector.
    vm.register_object_method("Entity", "GetPositionXYZ", [](NativeScriptVM& v) -> int {
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
    // utils.lua redefines GetIDFromEntity(e) as e:GetTableKey() — the entity's hashable key.
    // Return the uid (same identity space as GetID) so it keys tables like
    // QuestManager.EntitiesWithQuestThread correctly.
    vm.register_object_method("Entity", "GetTableKey", [](NativeScriptVM& v) -> int {
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

    // ---- Control: Physics.* movement / facing / velocity primitives (scalar) ----
    // The public Physics.TeleportToPosition/SetFacingVector/GetFacingVector/GetVelocity all
    // traffic in CVector3 objects; the boot-installed Lua shim unpacks vectors and calls these
    // scalar primitives (keeps C++ free of a vector-handle subsystem). Grounded: qc010/aibase
    // teleport + face the hero on every setup (aibase.lua:481/541/550); the follow behaviours
    // poll hero velocity to pick catch-up vs hold (behaviourallyfollowhero.lua:31-37).
    vm.register_native("Physics", "IsAvailable", [](NativeScriptVM& v) -> int {
        v.push_bool(true);  // FLAGGED: no Havok-availability gate; entities always accept motion
        return 1;
    });
    vm.register_native("Physics", "__Teleport", [](NativeScriptVM& v) -> int {
        auto* g = game_of(v);
        if (!g) return 0;
        const std::uint64_t uid = v.arg_handle(1);
        const std::array<float, 3> pos = {static_cast<float>(v.arg_number(2)),
                                          static_cast<float>(v.arg_number(3)),
                                          static_cast<float>(v.arg_number(4))};
        if (TransformComponent* t = entity_transform(*g, uid)) t->position = pos;
        if (uid == g->hero_uid) g->player.set_position(pos);  // keep player + entity coherent
        return 0;
    });
    vm.register_native("Physics", "__SetFacing", [](NativeScriptVM& v) -> int {
        auto* g = game_of(v);
        if (!g) return 0;
        const std::uint64_t uid = v.arg_handle(1);
        const float x = static_cast<float>(v.arg_number(2));
        const float z = static_cast<float>(v.arg_number(4));
        const float yaw = std::atan2(x, z);  // heading from a facing vector (x,_,z): yaw=atan2(x,z)
        if (TransformComponent* t = entity_transform(*g, uid)) t->rotation[1] = yaw;
        if (uid == g->hero_uid) g->player.set_facing_yaw(yaw);
        return 0;
    });
    vm.register_native("Physics", "__GetFacingRaw", [](NativeScriptVM& v) -> int {
        auto* g = game_of(v);
        const std::uint64_t uid = g ? v.arg_handle(1) : 0;
        float yaw = 0.0f;
        if (g) {
            if (uid == g->hero_uid) yaw = g->player.facing_yaw();
            else if (TransformComponent* t = entity_transform(*g, uid)) yaw = t->rotation[1];
        }
        // Unit facing vector matching __SetFacing's yaw=atan2(x,z) convention.
        v.push_number(std::sin(yaw)); v.push_number(0.0); v.push_number(std::cos(yaw));
        return 3;
    });
    vm.register_native("Physics", "__GetVelocityRaw", [](NativeScriptVM& v) -> int {
        auto* g = game_of(v);
        std::array<float, 3> vel{0.0f, 0.0f, 0.0f};
        // Hero velocity is the controller's last-step velocity; non-hero entities have no live
        // motor readback yet (FLAGGED: NPC velocity awaits the nav-goal motor, Slice 2).
        if (g && v.arg_handle(1) == g->hero_uid) vel = g->player.controller.velocity;
        v.push_number(vel[0]); v.push_number(vel[1]); v.push_number(vel[2]);
        return 3;
    });

    // ---- Navigation: scripted path movement (NpcAgent nav goal motor) ----
    // Navigation.MoveToPosition(entity, {position, radius, speed}) is the public native; the boot
    // Lua shim unpacks the opts table + CVector3 and calls __MoveTo(e, x,y,z, radius, tier). The
    // agent is created on demand (ensure_agent) so script-spawned allies/dog move too. Grounded:
    // aibase.lua:631-633 (MoveToPosition), behaviourfollow.lua:248/260 (StopMoving/GetCurrentSpeed).
    vm.register_native("Navigation", "__MoveTo", [](NativeScriptVM& v) -> int {
        auto* g = game_of(v);
        if (!g) return 0;
        NpcAgent& a = ensure_agent(*g, v.arg_handle(1));
        a.goal = {static_cast<float>(v.arg_number(2)), static_cast<float>(v.arg_number(3)),
                  static_cast<float>(v.arg_number(4))};
        const float radius = static_cast<float>(v.arg_number(5));
        a.arrive_radius = radius >= 1.0f ? radius : 0.6f;  // MOVE_TO_POS_BODGE_DIST default (FLAGGED)
        a.goal_speed = speed_for_nav_tier(static_cast<int>(v.arg_number(6)));
        a.has_goal = true;
        return 0;
    });
    vm.register_native("Navigation", "StopMoving", [](NativeScriptVM& v) -> int {
        auto* g = game_of(v);
        if (g) if (NpcAgent* a = find_agent(*g, v.arg_handle(1))) a->has_goal = false;
        return 0;
    });
    vm.register_native("Navigation", "GetCurrentSpeed", [](NativeScriptVM& v) -> int {
        auto* g = game_of(v);
        NpcAgent* a = g ? find_agent(*g, v.arg_handle(1)) : nullptr;
        v.push_number(a ? static_cast<double>(a->last_speed) : 0.0);
        return 1;
    });
    vm.register_native("Navigation", "GetMovementPaused", [](NativeScriptVM& v) -> int {
        v.push_bool(false);  // FLAGGED: no movement-pause system; behaviours gate on this
        return 1;
    });

    // ---- Camera: scripted pose override ----
    // Direct pose setters (grounded in camerabase.lua Camera.MoveTo/SetAngles/SetDirection/SetFOV)
    // take the camera off the follow-cam and hold the scripted pose until ClearCameraOverride.
    // MoveTo/SetDirection take CVector3 (a boot Lua shim unpacks them to these scalar primitives).
    vm.register_native("Camera", "__MoveTo", [](NativeScriptVM& v) -> int {
        auto* g = game_of(v);
        if (g) {
            g->camera.position = {static_cast<float>(v.arg_number(1)),
                                  static_cast<float>(v.arg_number(2)),
                                  static_cast<float>(v.arg_number(3))};
            g->camera_scripted = true;
        }
        return 0;
    });
    vm.register_native("Camera", "SetAngles", [](NativeScriptVM& v) -> int {
        auto* g = game_of(v);
        if (g) {  // FLAGGED: assumes (yaw, pitch) radians; the exact arg convention isn't RE'd
            g->camera.yaw = static_cast<float>(v.arg_number(1));
            g->camera.pitch = static_cast<float>(v.arg_number(2));
            g->camera_scripted = true;
        }
        return 0;
    });
    vm.register_native("Camera", "__SetDirection", [](NativeScriptVM& v) -> int {
        auto* g = game_of(v);
        if (g) {
            const float x = static_cast<float>(v.arg_number(1));
            const float y = static_cast<float>(v.arg_number(2));
            const float z = static_cast<float>(v.arg_number(3));
            g->camera.yaw = std::atan2(x, z);
            g->camera.pitch = std::atan2(y, std::sqrt(x * x + z * z));
            g->camera_scripted = true;
        }
        return 0;
    });
    vm.register_native("Camera", "GetAngles", [](NativeScriptVM& v) -> int {
        auto* g = game_of(v);
        v.push_number(g ? g->camera.yaw : 0.0); v.push_number(g ? g->camera.pitch : 0.0);
        return 2;
    });
    vm.register_native("Camera", "SetFOV", [](NativeScriptVM&) -> int {
        return 0;  // FLAGGED inert: no fov field flows through camera -> set_free_camera (either backend)
    });
    vm.register_native("Camera", "GetFOV", [](NativeScriptVM& v) -> int {
        v.push_number(70.0);  // FLAGGED: default ~70deg; retail FOV is region-authored CameraValues data
        return 1;
    });
    // CameraManager.SetCameraOverride(entity, mode, scope, params?) is the childhood cutscene entry
    // (qc010:231/1467). FLAGGED: the closure-driven cage (params.PositionFunction/FocusFunction) is
    // a follow-up; this records the request but does NOT gate the follow-cam, so the camera never
    // freezes waiting on a cage we don't yet evaluate. ClearCameraOverride releases a direct override.
    vm.register_native("CameraManager", "SetCameraOverride", [](NativeScriptVM&) -> int { return 0; });
    vm.register_native("CameraManager", "ClearCameraOverride", [](NativeScriptVM& v) -> int {
        if (auto* g = game_of(v)) g->camera_scripted = false;
        return 0;
    });

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

    // ---- Timing day counter ----
    // Needed because it is COMPARED and SUBTRACTED, so a stub value is a crash, not a no-op:
    // GameflowDayChecker:Update (gameflow.txt:1828) does `local LastDay = Timing.GetDayCount()`
    // then, in its loop, `local CurrentDay = Timing.GetDayCount()` / `if CurrentDay > LastDay` /
    // `CurrentDay - LastDay`. With the auto-stub's black-hole value that raised "attempt to
    // compare two table values" and killed the checker thread silently (measured:
    // docs/childhood_stub_census.txt).
    // Only the DIFFERENCE is ever consumed, so the absolute value needs no retail grounding.
    vm.register_native("Timing", "GetDayCount", [](NativeScriptVM& v) -> int {
        auto* g = game_of(v);
        v.push_number(g ? static_cast<double>(g->day_count) : 0.0);
        return 1;
    });
    vm.register_native("Timing", "SetDayCount", [](NativeScriptVM& v) -> int {
        if (auto* g = game_of(v)) g->day_count = static_cast<int>(v.arg_number(1));
        return 0;
    });
    vm.register_native("Timing", "AdvanceDayCount", [](NativeScriptVM& v) -> int {
        if (auto* g = game_of(v)) ++g->day_count;
        return 0;
    });

    // GetRandomNumber(n) -> integer in [1, n] INCLUSIVE. The range is pinned by the game's own
    // use of it as a 1-based table index with an explicit "none" sentinel one past the end
    // (gameflow.txt:304-306):
    //     ChosenClothing = GetRandomNumber(GetTableSize(Gameflow.HeroCoats) + 1)
    //     if (ChosenClothing ~= (GetTableSize(Gameflow.HeroCoats) + 1)) then ... HeroCoats[ChosenClothing]
    // That idiom only works if n itself is attainable and 1 is the lowest index, i.e. [1, n].
    // It must return a REAL number: GameflowQuestUnlocker:Update (gameflow.txt:1666) does
    // `local KidnapTiming = GetRandomNumber(100)` then `if KidnapTiming > 50`, which raised
    // "attempt to compare number with table" against the auto-stub and killed that thread.
    // ⚠ FLAGGED: only the RANGE is data-backed. Retail's generator (algorithm, seeding, sequence)
    // is not RE'd, so draws are NOT retail-identical — anything depending on the exact sequence
    // matching the 360 will differ.
    vm.register_global("GetRandomNumber", [](NativeScriptVM& v) -> int {
        const int n = static_cast<int>(v.arg_number(1));
        v.push_number(n >= 1 ? std::uniform_int_distribution<int>(1, n)(rng()) : 1);
        return 1;
    });
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
    // ---- GDB (the game database) ----
    // A record NAME resolves in two steps — name table (fnv1(name) -> GUID), then the record GUID
    // table — see native_gdb.h for the layout and its verification. The scripts' usage that drives
    // this is QuestEntityThreadBase.PlayCutscene (questmanager.lua:2103):
    //     if GDB.RecordExists(name) then rec = GDB.GetRecord(name) end
    //     rec:GetFloat("MaxRangeFromPlayer")
    // GetRecord returns a HANDLE (the record's GUID) carrying the record methods, which is how the
    // script calls rec:GetFloat(...).
    vm.register_native("GDB", "RecordExists", [](NativeScriptVM& v) -> int {
        auto* g = game_of(v);
        const char* name = v.arg_string(1);
        v.push_bool(g && name && *name && g->gdb.record_exists(name));
        return 1;
    });
    vm.register_native("GDB", "GetRecord", [](NativeScriptVM& v) -> int {
        auto* g = game_of(v);
        const char* name = v.arg_string(1);
        if (!g || !name || !*name) { v.push_nil(); return 1; }
        const auto guid = g->gdb.guid_for_name(name);
        if (!guid) { v.push_nil(); return 1; }   // nil, so `if rec then` reads false in script
        v.push_handle("GdbRecord", *guid);
        return 1;
    });
    vm.register_object_method("GdbRecord", "GetFloat", [](NativeScriptVM& v) -> int {
        auto* g = game_of(v);
        const char* field = v.arg_string(2);
        const auto val = (g && field && *field)
                             ? g->gdb.field_float(static_cast<std::uint32_t>(v.arg_handle(1)), field)
                             : std::nullopt;
        if (!val) { v.push_nil(); return 1; }
        v.push_number(*val);
        return 1;
    });
    // Integer-ish accessors over the same record. FLAGGED: the game's exact GDB accessor set is
    // wider than this (bools, strings, record refs); these are the ones reached so far. A string
    // getter needs the file's string table, which native_gdb.h does not parse yet.
    vm.register_object_method("GdbRecord", "GetInt", [](NativeScriptVM& v) -> int {
        auto* g = game_of(v);
        const char* field = v.arg_string(2);
        const auto raw = (g && field && *field)
                             ? g->gdb.field_raw(static_cast<std::uint32_t>(v.arg_handle(1)), field,
                                                gdb::kTypeS32)
                             : std::nullopt;
        if (!raw) { v.push_nil(); return 1; }
        v.push_number(static_cast<double>(static_cast<std::int32_t>(*raw)));
        return 1;
    });
    vm.register_object_method("GdbRecord", "GetBool", [](NativeScriptVM& v) -> int {
        auto* g = game_of(v);
        const char* field = v.arg_string(2);
        const auto raw = (g && field && *field)
                             ? g->gdb.field_raw(static_cast<std::uint32_t>(v.arg_handle(1)), field,
                                                gdb::kTypeBool)
                             : std::nullopt;
        v.push_bool(raw.has_value() && *raw != 0);
        return 1;
    });

    // AddCameraScriptFile(name) — the camera-script loader. `camera/camerasetupscript.lua` is a
    // list of calls to this native ("CameraFunctions.lua", "CameraValues.lua", "SimpleCamera.lua",
    // …), i.e. the camera scripts are loaded by the ENGINE, not by any game script: nothing in the
    // 552 shipped scripts references camerasetupscript by name, and the hot-reload dispatcher in
    // generalsetupscript routes a changed `camera/` file to the native Debug.ReloadCameras
    // (generalsetupscript main.proto[0] instr 20-26). Resolving the name against `camera/` in the
    // BNK reproduces that ownership.
    // Why it matters: CameraFunctions.CreateGenericClosure builds the camera cages for every
    // scripted cutscene — QC010's PooCam cold-open dies immediately without it (measured:
    // "attempt to index global 'CameraFunctions'").
    // ⚠ FLAGGED: the engine's exact camera-boot entry point is not RE'd; this mirrors the observed
    // mechanism (run the setup script, let it name its files) rather than a decompiled call site.
    vm.register_global("AddCameraScriptFile", [](NativeScriptVM& v) -> int {
        auto* g = game_of(v);
        const char* name = v.arg_string(1);
        if (!g || !g->script_bnk || !name || !*name) return 0;
        const std::string path = std::string("camera/") + name;
        const std::string key = BnkReader::normalize(path);
        for (const auto& s : g->loaded_scripts) {
            if (s == key) return 0;  // already loaded
        }
        std::vector<std::uint8_t> bytes = g->script_bnk->extract(path);
        if (bytes.empty()) return 0;
        g->loaded_scripts.push_back(key);
        v.run_bytecode(bytes.data(), bytes.size(), ("=" + key).c_str());
        return 0;
    });

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

    // __bnk_chunk(modname): compile a quest module from the BNK and return its chunk function
    // (WITHOUT running it) — the backend for a require() loader. QuestManager.LoadQuestModule
    // uses Lua's require(), which searches the filesystem; our scripts live in the BNK, so a
    // custom package.loaders entry (installed in boot_game_scripts) calls this. Returns nil if
    // the module isn't a BNK entry (require then tries the next loader).
    vm.register_global("__bnk_chunk", [](NativeScriptVM& v) -> int {
        auto* g = game_of(v);
        const char* name = v.arg_string(1);
        if (!g || !g->script_bnk || !name || !*name) return 0;
        // Quest modules live under quests/. BnkReader::normalize lowercases + resolves the path.
        std::vector<std::uint8_t> bytes = g->script_bnk->extract(std::string("quests/") + name + ".lua");
        if (bytes.empty()) return 0;
        if (!v.push_loaded_chunk(bytes.data(), bytes.size(), (std::string("=") + name).c_str()))
            return 0;
        return 1;  // the compiled module chunk
    });
}

}  // namespace f2
