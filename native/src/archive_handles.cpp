#include "albion/archive_handles.h"

#include <limits>
#include <new>
#include <utility>

namespace albion::data {

bool ArchiveHandles::Valid(ArchiveHandle handle, std::size_t& index) const {
    const auto slot = handle.value & 0xffffu;
    const auto generation = handle.value >> 16;
    if (slot == 0 || generation == 0) return false;
    index = slot - 1;
    return index < slots_.size() && slots_[index].generation == generation && slots_[index].archive;
}

Result<ArchiveHandle> ArchiveHandles::Insert(std::shared_ptr<const Archive> archive) try {
    if (!archive) return {{}, {ErrorCode::malformed, "cannot insert a null archive"}};
    std::lock_guard<std::mutex> lock(mutex_);
    std::size_t index = 0;
    for (; index < slots_.size(); ++index)
        if (!slots_[index].archive && slots_[index].generation != 0) break;
    if (index == slots_.size()) {
        if (index >= capacity_)
            return {{}, {ErrorCode::limit, "archive handle capacity exhausted"}};
        slots_.emplace_back();
    }
    auto& slot = slots_[index];
    slot.archive = std::move(archive);
    const auto value = (std::uint32_t(slot.generation) << 16) | std::uint32_t(index + 1);
    return {{value}, {}};
} catch (const std::bad_alloc&) {
    return {{}, {ErrorCode::limit, "archive handle allocation failed"}};
}

Result<std::shared_ptr<const Archive>> ArchiveHandles::Acquire(ArchiveHandle handle) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::size_t index;
    if (!Valid(handle, index)) return {{}, {ErrorCode::not_found, "invalid or stale archive handle"}};
    return {slots_[index].archive, {}};
}

Error ArchiveHandles::Release(ArchiveHandle handle) {
    std::shared_ptr<const Archive> retired;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::size_t index;
        if (!Valid(handle, index)) return {ErrorCode::not_found, "invalid or stale archive handle"};
        auto& slot = slots_[index];
        retired = std::move(slot.archive);
        slot.generation = slot.generation == (std::numeric_limits<std::uint16_t>::max)()
            ? 0 : static_cast<std::uint16_t>(slot.generation + 1);
    }
    // Final archive destruction, including file close, happens outside the lock.
    return {};
}

} // namespace albion::data
