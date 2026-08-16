#include "f2/native_entity.h"

#include <algorithm>

namespace f2 {

// ---------------- ComponentRegistry ----------------

void ComponentRegistry::insert_sorted(const ComponentDesc& d) {
    // Keep entries_ sorted by name_hash for binary-search lookup. Duplicate
    // name_hash registration overwrites (last registration wins, matching the
    // register-all pass where later Register_* can replace).
    auto it = std::lower_bound(entries_.begin(), entries_.end(), d.name_hash,
                               [](const ComponentDesc& e, std::uint32_t h) { return e.name_hash < h; });
    if (it != entries_.end() && it->name_hash == d.name_hash) {
        *it = d;
    } else {
        entries_.insert(it, d);
    }
}

void ComponentRegistry::register_hash(std::uint32_t name_hash, std::uint8_t type_id,
                                      ComponentCreateFn create) {
    insert_sorted(ComponentDesc{name_hash, type_id, create});
}

void ComponentRegistry::register_component(std::string_view name, std::uint8_t type_id,
                                           ComponentCreateFn create) {
    register_hash(gdb::fnv1(name), type_id, create);
}

const ComponentDesc* ComponentRegistry::lookup(std::uint32_t name_hash) const noexcept {
    auto it = std::lower_bound(entries_.begin(), entries_.end(), name_hash,
                               [](const ComponentDesc& e, std::uint32_t h) { return e.name_hash < h; });
    if (it != entries_.end() && it->name_hash == name_hash) return &*it;
    return nullptr;
}

void ComponentRegistry::seed_defaults() {
    // GraphicAppearanceStaticMesh (typeId 4, createFn 0x82630E00). The identity is
    // the FNV-1-verified field name "GraphicAppearanceStaticMeshComponent".
    register_hash(gdb::kCompGraphicAppearanceStaticMesh, kTypeIdGraphicAppearanceStaticMesh,
                  [](NativeEntity&) -> std::unique_ptr<NativeComponent> {
                      return std::make_unique<GraphicAppearanceStaticMeshComponent>();
                  });
    // Transform is engine-special and attached directly (see spawn_from_scene), so
    // it is intentionally NOT registered here.
}

// ---------------- NativeEntity ----------------

NativeComponent* NativeEntity::add_component(std::unique_ptr<NativeComponent> component) {
    if (!component) return nullptr;
    const std::uint8_t tid = component->type_id();

    // Retail duplicate gate (entity+0x24 bitmask): a typeId already present is not
    // added again — return the existing one.
    auto it = std::lower_bound(components_.begin(), components_.end(), tid,
                               [](const auto& e, std::uint8_t t) { return e.first < t; });
    if (it != components_.end() && it->first == tid) {
        return it->second.get();
    }
    NativeComponent* raw = component.get();
    components_.insert(it, std::make_pair(tid, std::move(component)));
    return raw;
}

NativeComponent* NativeEntity::component_by_typeid(std::uint8_t type_id) const noexcept {
    auto it = std::lower_bound(components_.begin(), components_.end(), type_id,
                               [](const auto& e, std::uint8_t t) { return e.first < t; });
    if (it != components_.end() && it->first == type_id) return it->second.get();
    return nullptr;
}

// ---------------- EntityManager ----------------

NativeEntity& EntityManager::create_entity(std::uint32_t record_guid) {
    auto entity = std::make_unique<NativeEntity>();
    entity->uid = next_uid_++;  // world UID counter (retail *(S+4)++)
    entity->record_guid = record_guid;
    NativeEntity& ref = *entity;
    by_uid_.emplace(ref.uid, &ref);
    entities_.push_back(std::move(entity));
    return ref;
}

NativeComponent* EntityManager::create_component_by_hash(NativeEntity& entity,
                                                         std::uint32_t name_hash) {
    const ComponentDesc* desc = registry_.lookup(name_hash);
    if (!desc || !desc->create) return nullptr;
    std::unique_ptr<NativeComponent> comp = desc->create(entity);
    if (!comp) return nullptr;
    NativeComponent* added = entity.add_component(std::move(comp));
    // Retail: InitFromGdbRecord (vtbl+0x20) then OnPostCreate (vtbl+0x2C). Inert now.
    if (added) {
        added->init_from_gdb();
        added->on_post_create(entity);
    }
    return added;
}

NativeEntity* EntityManager::find(std::uint64_t uid) const noexcept {
    auto it = by_uid_.find(uid);
    return it == by_uid_.end() ? nullptr : it->second;
}

void EntityManager::clear() {
    entities_.clear();
    by_uid_.clear();
    next_uid_ = 1;
}

// ---------------- collect_component_hashes ----------------

namespace {
void collect_recursive(std::uint32_t guid, const GdbRecordSource& src,
                       std::vector<std::uint32_t>& out) {
    // Parent first (so a child's RemoveComponent can prune an inherited one).
    std::uint32_t parent = 0;
    if (src.parent_of(guid, parent)) {
        collect_recursive(parent, src, out);
    }
    // Add this record's declared component fields (union — skip already-present).
    for (std::uint32_t h : src.component_fields(guid)) {
        if (std::find(out.begin(), out.end(), h) == out.end()) {
            out.push_back(h);
        }
    }
    // Prune RemoveComponent entries declared at this level.
    for (std::uint32_t removed : src.removed_components(guid)) {
        out.erase(std::remove(out.begin(), out.end(), removed), out.end());
    }
}
}  // namespace

std::vector<std::uint32_t> collect_component_hashes(std::uint32_t guid,
                                                    const GdbRecordSource& src) {
    std::vector<std::uint32_t> out;
    collect_recursive(guid, src, out);
    return out;
}

}  // namespace f2
