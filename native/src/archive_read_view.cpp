#include "albion/archive_read_view.h"

#include <new>
#include <utility>

namespace albion::data {

Result<std::vector<ResourceRecord>> DecodeResourceRecords(
    const std::uint8_t* bytes, std::size_t size, std::size_t max_records) try {
    if (size % 12 != 0 || (size && !bytes))
        return {{}, {ErrorCode::malformed, "invalid resource snapshot byte span"}};
    if (size / 12 > max_records)
        return {{}, {ErrorCode::limit, "resource snapshot budget exceeded"}};
    std::vector<ResourceRecord> records;
    records.reserve(size / 12);
    auto word = [](const std::uint8_t* p) {
        return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) |
               (std::uint32_t(p[2]) << 8) | p[3];
    };
    for (std::size_t offset = 0; offset < size; offset += 12) {
        const auto* p = bytes + offset;
        records.push_back({word(p), {word(p + 4), word(p + 8)}});
    }
    return {std::move(records), {}};
} catch (const std::bad_alloc&) {
    return {{}, {ErrorCode::limit, "resource snapshot allocation failed"}};
}

Result<ArchiveReadView> ArchiveReadView::Create(
    std::vector<std::shared_ptr<const Archive>> mounts,
    std::vector<ResourceRecord> records, std::size_t max_records) {
    if (records.size() > max_records)
        return {{}, {ErrorCode::limit, "resource snapshot budget exceeded"}};
    for (const auto& mount : mounts)
        if (!mount) return {{}, {ErrorCode::malformed, "null archive in resource snapshot"}};
    for (const auto& record : records) {
        const auto location = record.location;
        if (location.mount_index >= mounts.size() ||
            location.entry_index >= mounts[location.mount_index]->entries().size())
            return {{}, {ErrorCode::out_of_range, "invalid resource snapshot location"}};
    }
    auto index = ResourceIndex::FromOrderedRecords(std::move(records), max_records);
    if (!index) return {{}, index.error};
    ArchiveReadView view;
    view.index_ = std::move(index.value);
    view.mounts_ = std::move(mounts);
    return {std::move(view), {}};
}

Result<std::vector<std::uint8_t>> ArchiveReadView::ReadRange(
    std::uint32_t hash, std::uint64_t offset, std::uint64_t count) const {
    const auto location = index_.Find(hash);
    if (!location) return {{}, {ErrorCode::not_found, "resource hash not present in snapshot"}};
    return mounts_[location->mount_index]->ReadRange(location->entry_index, offset, count);
}

} // namespace albion::data
