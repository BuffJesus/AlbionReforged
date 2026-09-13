#pragma once
#include "albion/result.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace albion::data {

struct HashedResource {
    std::uint32_t hash = 0;
    std::uint32_t entry_index = 0;
};
struct ResourceMount {
    std::int32_t priority = 0;
    std::vector<HashedResource> entries;
};
struct ResourceLocation {
    std::uint32_t mount_index = 0;
    std::uint32_t entry_index = 0;
};
struct ResourceRecord {
    std::uint32_t hash = 0;
    ResourceLocation location;
};

// Native subset of TU1's indexed resource manager, not its registry/path layer.
// The caller supplies already resolved guest-compatible hashes and mount order.
// Larger signed priorities win; equal priority selects the later input mount.
// Build rejects a repeated hash WITHIN a mount as ambiguous: the guest's batch
// sort is not proved stable, so neither file order nor last-wins is assumed.
// FromOrderedRecords instead preserves an already-resolved table's exact order.
// No filenames, archive handles, guest addresses or asynchronous jobs are owned.
class ResourceIndex final {
public:
    static Result<ResourceIndex> Build(const std::vector<ResourceMount>& mounts,
                                       std::size_t max_entries = 1000000);
    // Adopt an already-resolved manager snapshot. Do not sort or deduplicate:
    // equal hashes retain their observed order and Find selects the first one.
    // Reject decreasing hashes. Mount/entry validity belongs to the read view.
    static Result<ResourceIndex> FromOrderedRecords(std::vector<ResourceRecord> records,
                                                   std::size_t max_entries = 1000000);
    std::optional<ResourceLocation> Find(std::uint32_t hash) const noexcept;
    std::size_t size() const noexcept { return records_.size(); }

private:
    using Record = ResourceRecord;
    std::vector<Record> records_;
};

} // namespace albion::data
