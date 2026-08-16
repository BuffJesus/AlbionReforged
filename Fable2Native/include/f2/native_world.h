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

namespace f2 {

struct NativeWorld {
    EntityManager entities;
    NativeEntity* hero = nullptr;

    // Build the entity graph from the cooked scene: one entity per NativeInstance,
    // carrying a TransformComponent (seeded from the instance transform) and a
    // GraphicAppearanceStaticMeshComponent bound to that instance index.
    void spawn_from_scene(const NativeScene& scene);

    // Write every bound entity's transform back into scene.instances[] (the master
    // tick's final "sync entity transforms -> renderer" step). Cheap; only touches
    // instances an entity is bound to.
    void sync_to_scene(NativeScene& scene) const;

    void clear();
};

}  // namespace f2
