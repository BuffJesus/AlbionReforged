#include "f2/native_bindings.h"

#include "f2/native_game.h"
#include "f2/native_script.h"

#include <array>
#include <cstdint>
#include <random>

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

}  // namespace f2
