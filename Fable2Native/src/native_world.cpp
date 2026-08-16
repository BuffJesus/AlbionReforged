#include "f2/native_world.h"

#include "f2/native_save.h"

#include <cstddef>

namespace f2 {

void NativeWorld::spawn_from_scene(const NativeScene& scene) {
    entities.clear();
    hero = nullptr;

    for (std::size_t i = 0; i < scene.instances.size(); ++i) {
        const NativeInstance& inst = scene.instances[i];
        NativeEntity& e = entities.create_entity();

        // Transform is engine-special (not a CEC registry type) — attach directly,
        // seeded from the baked instance transform.
        auto transform = std::make_unique<TransformComponent>();
        transform->position = inst.position;
        transform->rotation = inst.rotation;
        transform->scale = inst.scale;
        e.add_component(std::move(transform));

        // GraphicAppearanceStaticMesh via the registry factory (the real path), then
        // bind it to this drawn instance so moving the entity moves the mesh.
        auto* gfx = static_cast<GraphicAppearanceStaticMeshComponent*>(
            entities.create_component_by_hash(e, gdb::kCompGraphicAppearanceStaticMesh));
        if (gfx) {
            gfx->scene_instance = static_cast<int>(i);
        }

        // Tag by cooked mesh name: the cook names hero geoms "heroN" and villager
        // geoms "npcP_G" (cook_levels.py; npc_spawn_re.txt). The hero geom marks the
        // player entity; villager geoms carry a VillagerComponent (placement-derived —
        // per-NPC age/gender/job await the marker->archetype cook, gap P1).
        const std::string& mesh_name =
            inst.mesh < scene.meshes.size() ? scene.meshes[inst.mesh].name : std::string{};
        if (mesh_name.rfind("hero", 0) == 0) {
            if (!hero) hero = &e;
        } else if (mesh_name.rfind("npc", 0) == 0) {
            entities.create_component_by_hash(e, gdb::kCompVillager);
        }
    }
}

void NativeWorld::sync_to_scene(NativeScene& scene) const {
    // NativeWorld does not expose its entity vector directly; iterate via UID space.
    // Entities are dense from uid 1..N, so walk that range (cheap, no allocation).
    const std::size_t count = entities.entity_count();
    for (std::uint64_t uid = 1; uid <= count; ++uid) {
        const NativeEntity* e = entities.find(uid);
        if (!e) continue;
        const auto* gfx = e->get<GraphicAppearanceStaticMeshComponent>(kTypeIdGraphicAppearanceStaticMesh);
        if (!gfx || gfx->scene_instance < 0) continue;
        const auto si = static_cast<std::size_t>(gfx->scene_instance);
        if (si >= scene.instances.size()) continue;
        const auto* transform = e->get<TransformComponent>(kTypeIdTransform);
        if (!transform) continue;
        scene.instances[si].position = transform->position;
        scene.instances[si].rotation = transform->rotation;
        scene.instances[si].scale = transform->scale;
    }
}

void NativeWorld::serialize(WorldArchive& ar) {
    // Per-entity records keyed by UID, each carrying length-framed per-component blobs.
    // The direction lives in the archive; the graph iteration itself branches on it (as
    // the retail provider vtbl+0x10 does internally).
    if (!ar.reading()) {
        // WRITE: walk the live entities (dense UID 1..count).
        const std::size_t count = entities.entity_count();
        std::uint32_t record_count = static_cast<std::uint32_t>(count);
        ar.visit(record_count);
        for (std::uint64_t uid = 1; uid <= count; ++uid) {
            const NativeEntity* e = entities.find(uid);
            if (!e) continue;
            std::uint64_t key = e->uid;
            ar.visit(key);
            std::uint32_t comp_count = static_cast<std::uint32_t>(e->component_count());
            ar.visit(comp_count);
            e->for_each_component([&](std::uint8_t type_id, NativeComponent* comp) {
                std::uint8_t tid = type_id;
                ar.visit(tid);
                // Serialize the component into its own blob, then length-frame it so a
                // reader with a different baseline can skip an unknown component.
                WorldArchive sub(ArchiveMode::Write);
                comp->serialize(sub);
                std::vector<std::uint8_t> blob = sub.take();
                std::uint32_t len = static_cast<std::uint32_t>(blob.size());
                ar.visit(len);
                ar.write_bytes(blob);
            });
        }
    } else {
        // READ: overlay each record onto the already-rebuilt baseline entity (by UID).
        std::uint32_t record_count = 0;
        ar.visit(record_count);
        for (std::uint32_t r = 0; r < record_count && ar.ok(); ++r) {
            std::uint64_t key = 0;
            ar.visit(key);
            std::uint32_t comp_count = 0;
            ar.visit(comp_count);
            NativeEntity* e = entities.find(key);
            for (std::uint32_t c = 0; c < comp_count && ar.ok(); ++c) {
                std::uint8_t tid = 0;
                ar.visit(tid);
                std::uint32_t len = 0;
                ar.visit(len);
                std::vector<std::uint8_t> blob = ar.read_bytes(len);
                NativeComponent* comp = e ? e->component_by_typeid(tid) : nullptr;
                if (comp) {
                    WorldArchive sub(std::move(blob));
                    comp->serialize(sub);  // overlay onto the baseline component
                }
                // else: unknown entity/component -> blob already consumed, skip.
            }
        }
    }
}

void NativeWorld::clear() {
    entities.clear();
    hero = nullptr;
}

}  // namespace f2
