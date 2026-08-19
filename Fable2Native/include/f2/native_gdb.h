#pragma once

// Read-only GDB (game database) access for the native port.
//
// A .gdb holds the game's authored records — entity archetypes, animation sets, environment
// themes, interactive cutscenes. The game's scripts reach it through the GDB native class:
// GDB.RecordExists(name) / GDB.GetRecord(name) / record:GetFloat(field).
//
// TWO-STEP LOOKUP (this is the part that is easy to get wrong):
// A record's key is an editor-assigned OPAQUE GUID, not a hash of any name — hashing a name and
// searching the record table always fails. Each file carries a separate NAME TABLE mapping
// FNV-1(name) -> record GUID:
//
//   0x00 'GDB\0'
//   0x04 count        record count
//   0x08 size_a       record-data size
//   0x0C size_b       schema blob size
//   0x10 name_count   NAME-TABLE entry count   (NOT equal to `count` in any shipped file)
//   schema_base = 0x18 + size_a
//   hash_base   = schema_base + size_b        count * u32 record GUIDs, sorted
//   offset_base = hash_base + count*4         count * u16
//   name_base   = align4(offset_base + count*2)
//   NAME TABLE  = name_count * { u32 fnv1(name), u32 recordGUID }, sorted ascending by hash
//
// So: guid = name_table[fnv1(name)], then binary-search the record GUID table.
//
// Verified end to end (2026-08-19): fnv1("QC010_SetRoseMode") -> GUID 0x94FA26B1 -> record
// @0x2A318 -> MaxRangeFromPlayer = 10.0, which is exactly the field
// QuestEntityThreadBase.PlayCutscene reads (questmanager.lua:2103). Pairs are strictly ascending
// in every shipped file and every GUID column resolves as a record in the same file.
// Independent corroboration: Fable2AssetBrowser/source/src/Level/GdbEdit.cpp:79-167 parses the
// same layout. Python equivalent: Fable2Native/tools/gdb_anim_slots.py (GdbView).
//
// All integers are BIG-endian (Xbox 360 data, shipped unbyteswapped).

#include "f2/native_gdb_hash.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace f2::gdb {

// Field type codes (ghidra_out/gdb_entity_spec.txt): 0=bool 1=s32 2=u32 3=float 4=string
// 6=record-ref. A type-6 field's value is another record's GUID — including kHashParent, the
// inheritance link a field lookup walks when a record does not define the field itself.
inline constexpr std::uint8_t kTypeBool = 0;
inline constexpr std::uint8_t kTypeS32 = 1;
inline constexpr std::uint8_t kTypeU32 = 2;
inline constexpr std::uint8_t kTypeFloat = 3;
inline constexpr std::uint8_t kTypeString = 4;
inline constexpr std::uint8_t kTypeRecord = 6;

// kHashParent (the link a record inherits fields through) comes from native_gdb_hash.h.

class GdbFile {
public:
    GdbFile() = default;

    // Parse a .gdb. Returns false (and leaves the object unusable) on a bad header/layout.
    bool open(const std::filesystem::path& path);
    bool parse(std::vector<std::uint8_t> bytes);

    [[nodiscard]] bool valid() const noexcept { return ok_; }
    [[nodiscard]] std::uint32_t record_count() const noexcept { return count_; }
    [[nodiscard]] std::size_t name_count() const noexcept { return names_.size(); }

    // NAME -> record GUID, via the name table. std::nullopt if the name is not in this file.
    [[nodiscard]] std::optional<std::uint32_t> guid_for_name(std::string_view name) const;

    // Does this file hold a record with that GUID?
    [[nodiscard]] bool has_record(std::uint32_t guid) const;

    // Field value on a record, following kHashParent inheritance when the record does not define
    // the field itself. `field` is the FNV-1 of the field name.
    [[nodiscard]] std::optional<std::uint32_t> field_raw(std::uint32_t guid, std::uint32_t field,
                                                         std::uint8_t type) const;
    [[nodiscard]] std::optional<float> field_float(std::uint32_t guid, std::uint32_t field) const;

private:
    [[nodiscard]] std::optional<std::size_t> record_offset(std::uint32_t guid) const;
    [[nodiscard]] bool schema_at(std::size_t record, std::size_t& schema_off,
                                 std::uint32_t& field_count) const;
    [[nodiscard]] std::optional<std::uint32_t> field_local(std::size_t record, std::uint32_t field,
                                                           std::uint8_t type) const;
    [[nodiscard]] std::uint32_t be32(std::size_t off) const;

    std::vector<std::uint8_t> b_;
    bool ok_ = false;
    std::uint32_t count_ = 0;
    std::size_t schema_base_ = 0;
    std::size_t hash_base_ = 0;
    std::size_t body_end_ = 0;
    std::vector<std::size_t> offsets_;                              // record offsets, index-aligned
    std::vector<std::pair<std::uint32_t, std::uint32_t>> names_;    // {fnv1(name), guid}, sorted
};

// The set of open .gdb files, searched in order. Retail keeps ONE merged database; searching a
// small ordered list is equivalent for lookup purposes and keeps each file's bytes separate.
// Order matters: a level's own gdb should be searched before globals so a level override wins.
class GdbDatabase {
public:
    // Open a .gdb and append it to the search order. Returns false if it did not parse.
    bool add(const std::filesystem::path& path);
    [[nodiscard]] std::size_t file_count() const noexcept { return files_.size(); }

    // NAME -> GUID, from the first file that knows the name.
    [[nodiscard]] std::optional<std::uint32_t> guid_for_name(std::string_view name) const;
    // Is there a record for this name? (GDB.RecordExists)
    [[nodiscard]] bool record_exists(std::string_view name) const;
    // Field lookup by GUID across all files (the record may live in a different file than the
    // name table that named it — measured: cutscene names resolve into interactivecutscenes.gdb,
    // entity names into globals.gdb).
    [[nodiscard]] std::optional<float> field_float(std::uint32_t guid, std::string_view field) const;
    [[nodiscard]] std::optional<std::uint32_t> field_raw(std::uint32_t guid, std::string_view field,
                                                         std::uint8_t type) const;

private:
    std::vector<GdbFile> files_;
};

}  // namespace f2::gdb
