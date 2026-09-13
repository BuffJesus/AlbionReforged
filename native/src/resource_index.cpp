#include "albion/resource_index.h"

#include <algorithm>
#include <limits>
#include <map>
#include <new>
#include <utility>

namespace albion::data {

Result<ResourceIndex> ResourceIndex::Build(const std::vector<ResourceMount>& mounts,
                                          std::size_t max_entries) try {
    if (mounts.size() > (std::numeric_limits<std::uint32_t>::max)())
        return {{}, {ErrorCode::limit, "mount count exceeds index range"}};
    std::size_t count = 0;
    for (const auto& mount : mounts) {
        if (mount.entries.size() > max_entries - count)
            return {{}, {ErrorCode::limit, "resource index input budget exceeded"}};
        count += mount.entries.size();
    }
    struct Winner {
        ResourceLocation location;
        std::int32_t priority;
    };
    std::map<std::uint32_t, Winner> winners;
    for (std::size_t m = 0; m < mounts.size(); ++m) {
        const auto& mount = mounts[m];
        auto entries = mount.entries;
        std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
            return a.hash < b.hash;
        });
        for (std::size_t i = 1; i < entries.size(); ++i) {
            if (entries[i - 1].hash == entries[i].hash)
                return {{}, {ErrorCode::ambiguous, "duplicate hash within a mount is not supported"}};
        }
        for (const auto& entry : entries) {
            Winner candidate{{static_cast<std::uint32_t>(m), entry.entry_index}, mount.priority};
            auto [it, inserted] = winners.emplace(entry.hash, candidate);
            if (!inserted && mount.priority >= it->second.priority)
                it->second = candidate;
        }
    }
    ResourceIndex result;
    result.records_.reserve(winners.size());
    for (const auto& [hash, winner] : winners)
        result.records_.push_back({hash, winner.location});
    return {std::move(result), {}};
} catch (const std::bad_alloc&) {
    return {{}, {ErrorCode::limit, "resource index allocation failed"}};
}

std::optional<ResourceLocation> ResourceIndex::Find(std::uint32_t hash) const noexcept {
    auto it = std::lower_bound(records_.begin(), records_.end(), hash,
        [](const Record& record, std::uint32_t key) { return record.hash < key; });
    if (it == records_.end() || it->hash != hash) return std::nullopt;
    return it->location;
}

Result<ResourceIndex> ResourceIndex::FromOrderedRecords(std::vector<ResourceRecord> records,
                                                       std::size_t max_entries) {
    if (records.size() > max_entries)
        return {{}, {ErrorCode::limit, "resource snapshot budget exceeded"}};
    if (!std::is_sorted(records.begin(), records.end(), [](const Record& a, const Record& b) {
        return a.hash < b.hash;
    })) return {{}, {ErrorCode::malformed, "resource snapshot is not ordered by unsigned hash"}};
    ResourceIndex index;
    index.records_ = std::move(records);
    return {std::move(index), {}};
}

} // namespace albion::data
