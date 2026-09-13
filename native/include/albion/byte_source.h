#pragma once
#include "albion/result.h"
#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

namespace albion::data {
class Archive;
// Immutable-size, thread-safe byte source. Successful reads must return exactly
// count owned bytes. Underlying files must remain immutable while retained.
class ByteSource {
public:
    virtual ~ByteSource() = default;
    virtual std::uint64_t size() const noexcept = 0;
    virtual Result<std::vector<std::uint8_t>> ReadRange(
        std::uint64_t offset, std::uint64_t count) const = 0;
};
using Source = std::shared_ptr<const ByteSource>;
Result<Source> OpenFileSource(const std::filesystem::path& path,
                            std::uint64_t max_read_bytes = 256ull << 20);
// Half-open [begin,end); retains parent and rejects overflow/outside-parent spans.
Result<Source> SliceSource(Source parent, std::uint64_t begin, std::uint64_t end);
// Explicit entry index preserves duplicate-name identity; retains the archive.
Result<Source> ArchiveEntrySource(std::shared_ptr<const Archive> archive, std::size_t index);
} // namespace albion::data
