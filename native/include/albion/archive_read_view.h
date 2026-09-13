#pragma once
#include "albion/archive.h"
#include "albion/resource_index.h"

namespace albion::data {

// Decode the observed TU1 table format: big-endian {hash,mount,entry} words.
// This copies records only, never follows guest pointers or discovers mounts.
// The third guest word is provider-specific. Decoding does NOT convert a guest
// entry token to a native archive index; a provenance adapter must do that.
Result<std::vector<ResourceRecord>> DecodeResourceRecords(
    const std::uint8_t* bytes, std::size_t size, std::size_t max_records = 1000000);

// Immutable native read view for a captured, already-resolved resource table.
// Input mount positions must correspond exactly to the captured manager vector.
// Entry values supplied here MUST already be native zero-based archive indexes.
// Raw TU1 directory ordinals and nested-manager pointers are not accepted mappings.
// All mount and entry references are checked before publication. Sources must
// remain immutable on disk. Retained archives survive external handle closure;
// returned byte buffers survive this view. No guest memory is read or written.
class ArchiveReadView final {
public:
    static Result<ArchiveReadView> Create(
        std::vector<std::shared_ptr<const Archive>> mounts,
        std::vector<ResourceRecord> ordered_records,
        std::size_t max_records = 1000000);
    Result<std::vector<std::uint8_t>> ReadRange(
        std::uint32_t hash, std::uint64_t offset, std::uint64_t count) const;
    std::optional<ResourceLocation> Locate(std::uint32_t hash) const noexcept {
        return index_.Find(hash);
    }

private:
    ResourceIndex index_;
    std::vector<std::shared_ptr<const Archive>> mounts_;
};

} // namespace albion::data
