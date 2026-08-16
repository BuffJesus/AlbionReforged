#include "f2/native_world.h"

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

void NativeWorld::clear() {
    entities.clear();
    hero = nullptr;
}

}  // namespace f2
