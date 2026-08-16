#pragma once

// GDB field-name hashing + verified component/field hash constants.
//
// Fable II hashes GDB field names with FNV-1 (basis 0x811C9DC5, prime 0x01000193,
// case-sensitive) — the runtime hasher FUN_821f3d58 (gdb_entity_spec.txt:129,
// gdb_component_schemas.txt:12). This is FNV-1 (multiply THEN xor), not FNV-1a.
//
// The constants below are either verified by recomputing FNV-1 of the field name
// (the static_asserts prove it) or, where the exact source string is unknown, taken
// as byte-validated ground truth from the decomp specs and marked as such.

#include <cstdint>
#include <string_view>

namespace f2::gdb {

inline constexpr std::uint32_t kFnvBasis = 0x811C9DC5u;
inline constexpr std::uint32_t kFnvPrime = 0x01000193u;

// FNV-1 over ASCII bytes. constexpr so component identities resolve at compile time.
constexpr std::uint32_t fnv1(std::string_view s) noexcept {
    std::uint32_t h = kFnvBasis;
    for (char c : s) {
        h *= kFnvPrime;
        h ^= static_cast<std::uint8_t>(c);
    }
    return h;
}

// --- Reserved sentinels (NOT string hashes) ---
inline constexpr std::uint32_t kHashParent = 0x5F6317D5u;  // inheritance ref field (gdb_entity_spec.txt:173)

// --- Verified FNV-1 field/component hashes (static_assert-checked below) ---
inline constexpr std::uint32_t kRemoveComponent = 0x9B41D00Au;  // FNV1("RemoveComponent")
inline constexpr std::uint32_t kFieldPosition   = 0xBD7C27D4u;  // FNV1("Position")
inline constexpr std::uint32_t kCompGraphicAppearanceStaticMesh = 0x29CF50D1u;  // FNV1("GraphicAppearanceStaticMeshComponent")

static_assert(fnv1("") == kFnvBasis, "empty string must equal the FNV basis");
static_assert(fnv1("RemoveComponent") == kRemoveComponent, "FNV-1 variant mismatch");
static_assert(fnv1("Position") == kFieldPosition, "FNV-1 variant mismatch");
static_assert(fnv1("GraphicAppearanceStaticMeshComponent") == kCompGraphicAppearanceStaticMesh,
              "component field name form mismatch");

// --- NPC / AI component identities (<Name>Component convention, verified) ---
// The GDB component field name is the CEC class name minus "CEC" plus "Component"
// (schema convention, confirmed on all 33 enumerated components). typeIds are from
// gdb_component_registry.txt (the 261-class table).
inline constexpr std::uint32_t kCompVillager           = 0x5BA014D4u;  // CECVillager typeId 26
inline constexpr std::uint32_t kCompAIBrain            = 0xCD405756u;  // CECAIBrain typeId 71
inline constexpr std::uint32_t kCompPerception         = 0xF0FB7769u;  // CECPerception typeId 61
inline constexpr std::uint32_t kCompNavigation         = 0x08E9F814u;  // CECNavigation typeId 60
inline constexpr std::uint32_t kCompCreatureGenerator  = 0xA2371C5Au;  // CECCreatureGenerator typeId 51
static_assert(fnv1("VillagerComponent") == kCompVillager, "FNV mismatch");
static_assert(fnv1("AIBrainComponent") == kCompAIBrain, "FNV mismatch");
static_assert(fnv1("PerceptionComponent") == kCompPerception, "FNV mismatch");
static_assert(fnv1("NavigationComponent") == kCompNavigation, "FNV mismatch");
// Cross-checked: gdb_instantiation_re.txt names CreatureGeneratorComponent as GDB
// field 0xA2371C5A — independent confirmation of the convention + this hash.
static_assert(fnv1("CreatureGeneratorComponent") == kCompCreatureGenerator, "FNV mismatch");

// --- HealthComponent (CECHealth typeId 36) — schema-verified ---
inline constexpr std::uint32_t kCompHealth        = 0x26546FBCu;  // FNV1("HealthComponent")
inline constexpr std::uint32_t kFieldHealth       = 0x83632C03u;  // float
inline constexpr std::uint32_t kFieldMaxHealth    = 0x5B42D9DBu;  // float
inline constexpr std::uint32_t kFieldInvulnerable = 0x3215DFC8u;  // bool
static_assert(fnv1("HealthComponent") == kCompHealth, "FNV mismatch");
static_assert(fnv1("Health") == kFieldHealth, "FNV mismatch");
static_assert(fnv1("MaxHealth") == kFieldMaxHealth, "FNV mismatch");
static_assert(fnv1("Invulnerable") == kFieldInvulnerable, "FNV mismatch");

// --- VillagerComponent field hashes (schema-verified, gdb_component_schemas.txt) ---
inline constexpr std::uint32_t kFieldAge    = 0x484C8542u;  // enum
inline constexpr std::uint32_t kFieldGender = 0x2297CE0Au;  // enum
inline constexpr std::uint32_t kFieldJob    = 0x20367F82u;  // enum
inline constexpr std::uint32_t kFieldRich   = 0x026A39B3u;  // bool
inline constexpr std::uint32_t kFieldJobTag = 0x7FA702D2u;  // string
static_assert(fnv1("Age") == kFieldAge, "FNV mismatch");
static_assert(fnv1("Gender") == kFieldGender, "FNV mismatch");
static_assert(fnv1("Job") == kFieldJob, "FNV mismatch");
static_assert(fnv1("Rich") == kFieldRich, "FNV mismatch");
static_assert(fnv1("JobTag") == kFieldJobTag, "FNV mismatch");

// --- Byte-validated ground truth (source string unknown; do NOT recompute) ---
// SimpleTransformComponent field hash — byte-validated 109/109 in chapter2slums
// (npc_spawn_re.txt:35); this is where an entity's Position subrecord lives.
// NB: FNV1("SimpleTransformComponent") != this, so the real field name differs
// from the label — the hash itself is authoritative.
inline constexpr std::uint32_t kCompSimpleTransform = 0x619F96CFu;

}  // namespace f2::gdb
