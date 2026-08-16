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

// --- Byte-validated ground truth (source string unknown; do NOT recompute) ---
// SimpleTransformComponent field hash — byte-validated 109/109 in chapter2slums
// (npc_spawn_re.txt:35); this is where an entity's Position subrecord lives.
// NB: FNV1("SimpleTransformComponent") != this, so the real field name differs
// from the label — the hash itself is authoritative.
inline constexpr std::uint32_t kCompSimpleTransform = 0x619F96CFu;

}  // namespace f2::gdb
