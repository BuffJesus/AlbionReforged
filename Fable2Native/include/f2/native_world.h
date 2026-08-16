#pragma once

// Live game-world container — the top-level entity graph the master tick drives,
// sitting on top of the read-only NativeScene render snapshot.
//
// The retail world owns live entities whose GraphicAppearance component drives the
// draw; here NativeWorld owns the entities and PUSHES their transforms into
// scene.instances[] each tick (the additive pattern env used for clouds/moon).
//
// P1 seeds entities directly from the cooked baseline (spawn_from_scene wraps each
// baked NativeInstance as an entity with a Transform + GraphicAppearanceStaticMesh
// bound to that instance). A real per-entity GDB component cook (record GUID ->
// component fields -> InitFromGdbRecord) replaces this interim path in a later pass.

#include "native_entity.h"
#include "native_scene.h"
#include "native_npc.h"

#include <cstdint>
#include <vector>

namespace f2 {

class WorldArchive;         // native_save.h
class NativeCollisionWorld; // native_physics.h

// A live NPC = a villager entity + its ACT-layer controller (native_npc.h). The brain
// DECIDE layer is a flagged stand-in until the Lua VM (P6).
struct NpcAgent {
    std::uint64_t entity_uid = 0;
    NpcController controller;
};

struct NativeWorld {
    EntityManager entities;
    NativeEntity* hero = nullptr;
    std::vector<NpcAgent> npcs;   // one per villager entity, ticked each InWorld step
    float lod_radius = 60.0f;     // ENGINEERING: NPC active radius (IsEntityWithinDistanceOfLODCentre)

    // Build the entity graph from the cooked scene: one entity per NativeInstance,
    // carrying a TransformComponent (seeded from the instance transform) and a
    // GraphicAppearanceStaticMeshComponent bound to that instance index.
    void spawn_from_scene(const NativeScene& scene);

    // Write every bound entity's transform back into scene.instances[] (the master
    // tick's final "sync entity transforms -> renderer" step). Cheap; only touches
    // instances an entity is bound to.
    void sync_to_scene(NativeScene& scene) const;

    // A melee swing: damage all live NPCs inside the attack volume. range/cone/damage are
    // FLAGGED engineering (the retail attack-volume geometry IsEntityInAttackerAttackVolume
    // @0x8277A988 is a Ghidra gap; weapon damage is item data). See melee_attack.
    struct MeleeAttackConfig {
        float range = 2.5f;         // ENGINEERING (attack-volume reach is a gap)
        float cone_cos = -0.2f;     // ENGINEERING: dot(forward,toTarget) >= this to hit (~fwd 100deg arc)
        float damage = 25.0f;       // ENGINEERING (retail = weapon damage window)
        float eye_height = 1.2f;    // LOS/attack ray height above feet
    };
    struct MeleeResult {
        int hits = 0;
        int kills = 0;
    };

    // Swing from `origin` facing `yaw`: every live NPC within range, inside the facing
    // cone, and with clear line-of-sight takes Health.Modify(-damage) via its
    // HealthComponent (the single retail damage verb, combat_system.txt A). NPCs whose
    // health reaches 0 are killed (agent.alive=false). Returns hit/kill counts.
    MeleeResult melee_attack(const NativeCollisionWorld& collision,
                             const std::array<float, 3>& origin, float yaw,
                             const MeleeAttackConfig& config = {});

    // Tick the live NPCs (the master order's "entity/brain" step): each agent runs its
    // ACT layer against `target` (the player) with the LOD centre = target, then its
    // resolved position is written back into the entity's TransformComponent so the
    // next sync_to_scene moves the drawn instance.
    void update_npcs(const NativeCollisionWorld& collision,
                     const std::array<float, 3>& target, float dt);

    // Re-seed each NPC agent's controller position from its entity's (possibly restored)
    // TransformComponent. Called after a save is loaded so agents resume at their saved
    // spot instead of snapping back to the spawn transform.
    void resync_npcs_from_entities();

    // Bidirectional entity-graph delta (retail provider vtbl+0x10; gamestate_save_restore
    // §A.3/§B.1). WRITE: walk live entities, emit per-entity {uid, per-component
    // length-framed blob}. READ: reconstruct the baseline first (spawn_from_scene), then
    // overlay each record onto the entity matched by UID; unknown entities/components are
    // skipped via the length frame. Assumes the SAME cooked baseline on load (the delta
    // premise, §D).
    void serialize(WorldArchive& ar);

    void clear();
};

}  // namespace f2
