#pragma once
#include "albion/archive.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace albion::data {

struct ArchiveHandle {
    std::uint32_t value = 0;
};

// Host-side adapter storage. Tokens are local to THIS table, not an existing
// guest ABI or a replacement for guest reference counting. Never cast a token
// to an address. The low/high 16 bits encode slot+1/generation; zero is invalid.
// Acquire retains an immutable archive before unlocking. Release prevents new
// acquisitions, while outstanding leases/read jobs keep the archive alive.
// Generation exhaustion permanently retires a slot rather than reviving tokens.
class ArchiveHandles final {
public:
    explicit ArchiveHandles(std::uint16_t capacity = 4096) : capacity_(capacity) {}
    Result<ArchiveHandle> Insert(std::shared_ptr<const Archive> archive);
    Result<std::shared_ptr<const Archive>> Acquire(ArchiveHandle handle) const;
    Error Release(ArchiveHandle handle);

private:
    struct Slot {
        std::uint16_t generation = 1; // zero means permanently retired
        std::shared_ptr<const Archive> archive;
    };
    bool Valid(ArchiveHandle handle, std::size_t& index) const;
    const std::uint16_t capacity_;
    mutable std::mutex mutex_;
    std::vector<Slot> slots_;
};

} // namespace albion::data
