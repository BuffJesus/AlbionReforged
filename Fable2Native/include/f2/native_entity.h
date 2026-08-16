#pragma once

// Entity-component substrate — host-native transcription of the retail
// GDB-record -> live-entity instantiation backbone (ghidra_out/gdb_instantiation_re.txt).
//
// Retail chain (decomp-verified):
//   entity_create_from_record @0x8238B860
//     -> entity_build_components_from_record @0x8238DCC8
//          -> gdb_record_collect_component_hashes @0x8238DB28  (parent-first union
//             up the kHashParent chain, minus RemoveComponent)
//          -> per hash: entity_create_component_by_hash @0x8238CDC8 (the FACTORY)
//               -> component_registry_lookup @0x82630218 (bsearch, 0x18 stride,
//                  {nameHash, createFn, ..., typeId})
//               -> createFn(entity) -> GetTypeId (vtbl+0x10) -> sorted-insert
//                  {typeId, component} into entity+0x48 (dup-gate entity+0x24)
//               -> InitFromGdbRecord (vtbl+0x20) -> OnPostCreate (vtbl+0x2C)
//
// We model host-native components (not guest byte layouts). The registry is keyed
// by FNV-1 of the component field name (see native_gdb_hash.h). Only a few types
// have real behaviour today; the rest can register as inert placeholders so real
// cooked records still instantiate faithfully.

#include "native_gdb_hash.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace f2 {

class NativeEntity;

// Component typeIds (from gdb_component_registry.txt, decomp-verified).
enum ComponentTypeId : std::uint8_t {
    kTypeIdPhysics                    = 2,   // all CECPhysics* share slot 2
    kTypeIdGraphicAppearance          = 3,   // CECGraphicAppearance
    kTypeIdGraphicAppearanceStaticMesh= 4,   // CECGraphicAppearanceStaticMesh
    kTypeIdNavigation                 = 60,  // CECNavigation
    kTypeIdPerception                 = 61,  // CECPerception
    kTypeIdAIBrain                    = 71,  // CECAIBrain
    kTypeIdCreatureGenerator          = 51,  // CECCreatureGenerator
    kTypeIdVillager                   = 26,  // CECVillager
    // Transform is engine-special (NOT in the 261-class CEC registry). Retail
    // commits it specially (entity+0x90 |= 0xA0); its exact typeId is ambiguous in
    // the specs (STEP3.6 says 3, but registry 3 = GraphicAppearance). We give it a
    // host-reserved id above the 0..0xFD registry range so it never collides.
    kTypeIdTransform                  = 0xFE, // HOST-assigned ordering id (not a retail claim)
};

// Live component base. Mirrors the retail component vtable contract:
//   GetTypeId (vtbl+0x10), InitFromGdbRecord (vtbl+0x20), OnPostCreate (vtbl+0x2C).
class NativeComponent {
public:
    virtual ~NativeComponent() = default;
    [[nodiscard]] virtual std::uint8_t type_id() const noexcept = 0;
    // Read GDB fields into the live object (P2+ once records are cooked). Inert now.
    virtual void init_from_gdb() {}
    virtual void on_post_create(NativeEntity& /*entity*/) {}
};

// TransformComponent — the entity's world transform (position sourced from the
// SimpleTransformComponent subrecord, native_gdb_hash kCompSimpleTransform).
class TransformComponent final : public NativeComponent {
public:
    std::array<float, 3> position{0.0f, 0.0f, 0.0f};
    std::array<float, 3> rotation{0.0f, 0.0f, 0.0f};  // euler (matches NativeInstance.rotation)
    float scale = 1.0f;
    [[nodiscard]] std::uint8_t type_id() const noexcept override { return kTypeIdTransform; }
};

// GraphicAppearanceStaticMeshComponent — binds the entity to a drawn NativeScene
// instance. Moving the entity's transform updates scene.instances[scene_instance].
class GraphicAppearanceStaticMeshComponent final : public NativeComponent {
public:
    int scene_instance = -1;  // index into NativeScene.instances (-1 = not bound)
    [[nodiscard]] std::uint8_t type_id() const noexcept override {
        return kTypeIdGraphicAppearanceStaticMesh;
    }
};

// VillagerComponent — a townsperson's identity (age/gender/job/home). Fields mirror
// the real GDB VillagerComponent schema (gdb_component_schemas.txt, hashes verified in
// native_gdb_hash.h). Values are read from the GDB villager record via init_from_gdb
// once the marker->generator->archetype binding is cooked (npc_spawn_re.txt gap P1);
// until then they carry their neutral defaults.
class VillagerComponent final : public NativeComponent {
public:
    int age = 0;             // Age enum (0x484C8542)
    int gender = 0;          // Gender enum (0x2297CE0A)
    int job = 0;             // Job enum (0x20367F82)
    bool rich = false;       // Rich bool (0x026A39B3)
    std::string job_tag;     // JobTag string (0x7FA702D2), e.g. TEXT_CHARACTER_OCCUPATION_*
    [[nodiscard]] std::uint8_t type_id() const noexcept override { return kTypeIdVillager; }
};

// InertComponent — a registered-but-unimplemented component that carries its retail
// typeId so real GDB records still instantiate faithfully (the AI family: Brain/
// Perception/Navigation/Generator). Behaviour is a documented gap (P4).
class InertComponent final : public NativeComponent {
public:
    explicit InertComponent(std::uint8_t type_id) noexcept : type_id_(type_id) {}
    [[nodiscard]] std::uint8_t type_id() const noexcept override { return type_id_; }

private:
    std::uint8_t type_id_;
};

// Registry descriptor (mirrors the retail 0x18-stride entry {nameHash, createFn, .., typeId}).
using ComponentCreateFn = std::unique_ptr<NativeComponent> (*)(NativeEntity&);
struct ComponentDesc {
    std::uint32_t name_hash = 0;
    std::uint8_t type_id = 0;
    ComponentCreateFn create = nullptr;
};

// Component registry — FNV-1(name) -> {typeId, createFn}, kept sorted by name_hash
// so lookup is a binary search (mirrors component_registry_lookup @0x82630218).
class ComponentRegistry {
public:
    // Register by GDB field name (e.g. "GraphicAppearanceStaticMeshComponent").
    void register_component(std::string_view name, std::uint8_t type_id, ComponentCreateFn create);
    void register_hash(std::uint32_t name_hash, std::uint8_t type_id, ComponentCreateFn create);
    [[nodiscard]] const ComponentDesc* lookup(std::uint32_t name_hash) const noexcept;

    // Seed the real component types this build implements (Transform is engine-
    // special and added directly by spawn_from_scene, not via the registry).
    void seed_defaults();

    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

private:
    void insert_sorted(const ComponentDesc& d);
    std::vector<ComponentDesc> entries_;  // sorted by name_hash
    bool sorted_ = true;
};

// One live entity. Components are held sorted by typeId (mirrors entity+0x48).
class NativeEntity {
public:
    std::uint64_t uid = 0;
    std::uint32_t record_guid = 0;

    // Sorted-insert with the retail duplicate gate: a typeId already present is not
    // added twice (entity+0x24 bitmask). Returns the live component (existing or new).
    NativeComponent* add_component(std::unique_ptr<NativeComponent> component);

    [[nodiscard]] NativeComponent* component_by_typeid(std::uint8_t type_id) const noexcept;

    template <class T>
    [[nodiscard]] T* get(std::uint8_t type_id) const noexcept {
        return static_cast<T*>(component_by_typeid(type_id));
    }

    [[nodiscard]] std::size_t component_count() const noexcept { return components_.size(); }

private:
    std::vector<std::pair<std::uint8_t, std::unique_ptr<NativeComponent>>> components_;
};

// Owns entities + the registry + the world UID counter, and runs the instantiate
// chain. Entities are heap-stable (unique_ptr) so pointers into by_uid stay valid.
class EntityManager {
public:
    EntityManager() { registry_.seed_defaults(); }

    [[nodiscard]] ComponentRegistry& registry() noexcept { return registry_; }

    // Allocate an entity + UID (retail world counter *(S+4)++, starts at 1).
    NativeEntity& create_entity(std::uint32_t record_guid = 0);

    // Factory: look up name_hash in the registry, create + attach the component
    // (mirrors entity_create_component_by_hash). Returns null if unregistered.
    NativeComponent* create_component_by_hash(NativeEntity& entity, std::uint32_t name_hash);

    [[nodiscard]] NativeEntity* find(std::uint64_t uid) const noexcept;
    [[nodiscard]] std::size_t entity_count() const noexcept { return entities_.size(); }

    void clear();

private:
    ComponentRegistry registry_;
    std::vector<std::unique_ptr<NativeEntity>> entities_;
    std::unordered_map<std::uint64_t, NativeEntity*> by_uid_;
    std::uint64_t next_uid_ = 1;
};

// Abstract read side of a GDB record set — lets collect_component_hashes model the
// retail parent-walk without a concrete GDB parser yet (unit-tested with a fake).
struct GdbRecordSource {
    virtual ~GdbRecordSource() = default;
    // Parent record GUID via the kHashParent field; returns false if none.
    virtual bool parent_of(std::uint32_t guid, std::uint32_t& parent_out) const = 0;
    // Type-6 component field-name hashes declared directly on this record.
    virtual std::vector<std::uint32_t> component_fields(std::uint32_t guid) const = 0;
    // RemoveComponent string values (already hashed) declared on this record.
    virtual std::vector<std::uint32_t> removed_components(std::uint32_t guid) const = 0;
};

// Parent-first union of component field hashes up the kHashParent chain, minus any
// RemoveComponent entries (mirrors gdb_record_collect_component_hashes @0x8238DB28).
// Order: ancestors first, then this record (so a child can prune an inherited one).
std::vector<std::uint32_t> collect_component_hashes(std::uint32_t guid, const GdbRecordSource& src);

}  // namespace f2
