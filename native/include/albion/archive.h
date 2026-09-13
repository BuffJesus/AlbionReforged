#pragma once
#include "albion/result.h"
#include "albion/byte_source.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace albion::data {

struct ArchiveLimits {
    std::uint64_t table_bytes = 64ull << 20;
    std::uint64_t entry_bytes = 256ull << 20;
    std::uint32_t entries = 1000000;
    std::uint32_t name_bytes = 4096;
};
struct EntryInfo {
    std::string name;
    std::uint64_t size = 0;
};

// A read-only BNK v3 handle. Keep the underlying archive immutable while open.
// Entry names are exact byte strings: no case folding, path or script rewriting.
// Concurrent reads serialize disk access; decoding and returned buffers are owned
// by each caller. Metadata is immutable for the lifetime of the archive.
class Archive final {
public:
    static Result<std::unique_ptr<Archive>> Open(
        const std::filesystem::path& path, ArchiveLimits limits = {});
    // Retains an immutable source, including bounded slices or archive entries.
    static Result<std::unique_ptr<Archive>> Open(Source source, ArchiveLimits limits = {});
    ~Archive();
    Archive(const Archive&) = delete;
    Archive& operator=(const Archive&) = delete;
    const std::vector<EntryInfo>& entries() const;
    // Duplicate names are retained in table order. Named reads return ambiguous
    // rather than silently selecting one; callers can explicitly read an index.
    Result<std::vector<std::uint8_t>> Read(const std::string& name) const;
    Result<std::vector<std::uint8_t>> Read(std::size_t index) const;
    // Offset/count are in decoded bytes. Only overlapping compressed blocks are
    // read and validated. Empty reads at EOF succeed; out-of-bounds reads fail.
    Result<std::vector<std::uint8_t>> ReadRange(
        std::size_t index, std::uint64_t offset, std::uint64_t count) const;

private:
    struct Impl;
    explicit Archive(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace albion::data
