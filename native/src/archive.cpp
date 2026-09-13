#include "albion/archive.h"
#include "miniz.h"

#include <algorithm>
#include <fstream>
#include <limits>
#include <mutex>
#include <new>
#include <unordered_map>
#include <utility>

namespace albion::data {
namespace {
using Bytes = std::vector<std::uint8_t>;
constexpr std::size_t block_size = 32768;
Error fail(ErrorCode code, const char* message) { return {code, message}; }
std::uint32_t be32(const std::uint8_t* p) {
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) |
           (std::uint32_t(p[2]) << 8) | p[3];
}
struct Cursor {
    const Bytes& data;
    std::size_t pos = 0;
    bool word(std::uint32_t& value) {
        if (data.size() - pos < 4) return false;
        value = be32(data.data() + pos); pos += 4; return true;
    }
};

// Shipped BNKs use independent compressed-input blocks, not necessarily complete
// zlib streams. Nonfinal full blocks may stop mid-deflate. The final block in the
// audited corpus either completes zlib or ends in Z_SYNC_FLUSH (00 00 FF FF).
// Framed chunks have no checksum: exact size is a contract check, not integrity.
Result<Bytes> inflate(const Bytes& source, std::size_t offset, std::size_t length,
                      std::uint32_t expected, bool final_block) {
    Result<Bytes> result;
    result.value.resize(std::size_t(expected) + 1); // detects extra decoded bytes
    mz_stream stream{};
    if (mz_inflateInit(&stream) != MZ_OK)
        return {{}, fail(ErrorCode::decode, "inflate initialization failed")};
    stream.next_in = source.data() + offset;
    stream.avail_in = static_cast<unsigned int>(length);
    stream.next_out = result.value.data();
    stream.avail_out = static_cast<unsigned int>(result.value.size());
    const int status = mz_inflate(&stream, MZ_SYNC_FLUSH);
    const auto produced = stream.total_out;
    const auto remaining = stream.avail_in;
    mz_inflateEnd(&stream);
    if (status != MZ_OK && status != MZ_BUF_ERROR && status != MZ_STREAM_END)
        return {{}, fail(ErrorCode::decode, "invalid compressed data or checksum")};
    if (produced != expected)
        return {{}, fail(ErrorCode::size_mismatch, "decoded size differs from table")};
    if (remaining != 0)
        return {{}, fail(ErrorCode::malformed, "unconsumed bytes in compressed block")};
    const bool sync_tail = length >= 4 && source[offset + length - 4] == 0 &&
        source[offset + length - 3] == 0 && source[offset + length - 2] == 255 &&
        source[offset + length - 1] == 255;
    if (final_block && status != MZ_STREAM_END && !sync_tail)
        return {{}, fail(ErrorCode::decode, "final block lacks supported completion framing")};
    result.value.resize(expected);
    return result;
}
} // namespace

struct Archive::Impl {
    struct Entry {
        std::uint64_t offset;
        std::uint32_t stored;
        std::vector<std::uint32_t> chunks;
    };
    Source source;
    std::uint64_t file_size = 0;
    bool compressed = false;
    std::vector<EntryInfo> info;
    std::vector<Entry> records;
    std::unordered_map<std::string, std::size_t> lookup;

    Result<Bytes> bytes(std::uint64_t offset, std::size_t count) const {
        if (offset > file_size || count > file_size - offset)
            return {{}, fail(ErrorCode::malformed, "file range out of bounds")};
        auto result = source->ReadRange(offset, count);
        if (result && result.value.size() != count)
            return {{}, fail(ErrorCode::size_mismatch, "archive source returned wrong byte count")};
        return result;
    }

    Error parse(ArchiveLimits limits) {
        // The inflater API uses unsigned-int lengths, including one overflow byte.
        constexpr auto maximum = std::numeric_limits<unsigned int>::max() - 1ull;
        if (limits.table_bytes > maximum || limits.entry_bytes > maximum)
            return fail(ErrorCode::limit, "configured limit exceeds codec range");
        auto header = bytes(0, 9);
        if (!header) return header.error;
        const auto base = be32(header.value.data());
        if (be32(header.value.data() + 4) != 3)
            return fail(ErrorCode::unsupported, "only BNK version 3 is supported");
        if (header.value[8] > 1)
            return fail(ErrorCode::unsupported, "unsupported compression flag");
        compressed = header.value[8] != 0;
        if (base < 17 || base > file_size)
            return fail(ErrorCode::malformed, "invalid data-section offset");
        std::uint64_t pos = 9, decoded_size = 0;
        Bytes packed;
        for (;;) {
            if (pos > base || base - pos < 8)
                return fail(ErrorCode::malformed, "missing table terminator");
            auto pair = bytes(pos, 8);
            if (!pair) return pair.error;
            pos += 8;
            const auto stored = be32(pair.value.data());
            const auto decoded = be32(pair.value.data() + 4);
            if (!stored) {
                if (decoded) return fail(ErrorCode::malformed, "invalid table terminator");
                break;
            }
            if (stored > base - pos)
                return fail(ErrorCode::malformed, "table overlaps data section");
            if (stored > limits.table_bytes - packed.size() ||
                decoded > limits.table_bytes - decoded_size)
                return fail(ErrorCode::limit, "table exceeds configured budget");
            auto part = bytes(pos, stored);
            if (!part) return part.error;
            packed.insert(packed.end(), part.value.begin(), part.value.end());
            pos += stored; decoded_size += decoded;
        }
        if (packed.empty()) return fail(ErrorCode::malformed, "missing compressed table");
        auto table = inflate(packed, 0, packed.size(), static_cast<std::uint32_t>(decoded_size), true);
        if (!table) return table.error;
        Cursor cursor{table.value};
        std::uint32_t count;
        if (!cursor.word(count)) return fail(ErrorCode::malformed, "missing entry count");
        if (count > limits.entries) return fail(ErrorCode::limit, "too many entries");
        for (std::uint32_t i = 0; i < count; ++i) {
            std::uint32_t name_size, relative, decoded, stored, chunks = 0;
            if (!cursor.word(name_size)) return fail(ErrorCode::malformed, "missing name length");
            if (name_size > limits.name_bytes) return fail(ErrorCode::limit, "entry name too long");
            if (!name_size || name_size > table.value.size() - cursor.pos)
                return fail(ErrorCode::malformed, "invalid entry name length");
            std::string name(reinterpret_cast<const char*>(table.value.data() + cursor.pos), name_size);
            cursor.pos += name_size;
            if (name.back() == '\0') name.pop_back();
            if (name.empty() || name.find('\0') != std::string::npos)
                return fail(ErrorCode::malformed, "empty or embedded-NUL entry name");
            if (!cursor.word(relative) || !cursor.word(decoded))
                return fail(ErrorCode::malformed, "truncated entry metadata");
            stored = decoded;
            if (compressed && (!cursor.word(stored) || !cursor.word(chunks)))
                return fail(ErrorCode::malformed, "truncated compressed entry metadata");
            if (stored > limits.entry_bytes || decoded > limits.entry_bytes)
                return fail(ErrorCode::limit, "entry exceeds configured budget");
            Entry entry{std::uint64_t(base) + relative, stored, {}};
            if (entry.offset > file_size || stored > file_size - entry.offset)
                return fail(ErrorCode::malformed, "entry range outside archive");
            if (compressed) {
                const auto expected_chunks = (std::uint64_t(stored) + block_size - 1) / block_size;
                if (chunks != expected_chunks)
                    return fail(ErrorCode::unsupported, "unsupported compressed block layout");
                std::uint64_t sum = 0;
                for (std::uint32_t j = 0; j < chunks; ++j) {
                    std::uint32_t size;
                    if (!cursor.word(size)) return fail(ErrorCode::malformed, "truncated chunk table");
                    sum += size;
                    entry.chunks.push_back(size);
                }
                if (sum != decoded)
                    return fail(ErrorCode::size_mismatch, "chunk sizes differ from entry size");
            }
            const auto inserted = lookup.emplace(name, records.size());
            if (!inserted.second) inserted.first->second = std::numeric_limits<std::size_t>::max();
            info.push_back({std::move(name), decoded});
            records.push_back(std::move(entry));
        }
        if (cursor.pos != table.value.size()) return fail(ErrorCode::malformed, "trailing table bytes");
        return {};
    }
};

Archive::Archive(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Archive::~Archive() = default;
const std::vector<EntryInfo>& Archive::entries() const { return impl_->info; }

Result<std::unique_ptr<Archive>> Archive::Open(const std::filesystem::path& path, ArchiveLimits limits) {
    auto source = OpenFileSource(path, std::max({limits.table_bytes, limits.entry_bytes, std::uint64_t{32768}}));
    if (!source) return {{}, source.error};
    return Open(std::move(source.value), limits);
}

Result<std::unique_ptr<Archive>> Archive::Open(Source source, ArchiveLimits limits) {
    if (!source) return {{}, fail(ErrorCode::malformed, "null archive source")};
    try {
        auto impl = std::make_unique<Impl>();
        impl->file_size = source->size();
        impl->source = std::move(source);
        auto error = impl->parse(limits);
        if (error.code != ErrorCode::none) return {{}, std::move(error)};
        return {std::unique_ptr<Archive>(new Archive(std::move(impl))), {}};
    } catch (const std::bad_alloc&) {
        return {{}, fail(ErrorCode::limit, "archive metadata allocation failed")};
    }
}

Result<Bytes> Archive::Read(const std::string& name) const {
    try {
        const auto found = impl_->lookup.find(name);
        if (found == impl_->lookup.end()) return {{}, fail(ErrorCode::not_found, "entry not found")};
        if (found->second == std::numeric_limits<std::size_t>::max())
            return {{}, fail(ErrorCode::ambiguous, "multiple entries have this exact name")};
        return Read(found->second);
    } catch (const std::bad_alloc&) {
        return {{}, fail(ErrorCode::limit, "archive lookup allocation failed")};
    }
}

Result<Bytes> Archive::Read(std::size_t index) const {
    if (index >= impl_->records.size()) return {{}, fail(ErrorCode::not_found, "entry index out of range")};
    return ReadRange(index, 0, impl_->info[index].size);
}

Result<Bytes> Archive::ReadRange(std::size_t index, std::uint64_t offset, std::uint64_t count) const {
    try {
        if (index >= impl_->records.size()) return {{}, fail(ErrorCode::not_found, "entry index out of range")};
        const auto& entry = impl_->records[index];
        const auto size = impl_->info[index].size;
        if (offset > size || count > size - offset)
            return {{}, fail(ErrorCode::out_of_range, "read range outside decoded entry")};
        if (!count) return {};
        if (!impl_->compressed)
            return impl_->bytes(entry.offset + offset, static_cast<std::size_t>(count));
        Bytes output;
        output.reserve(static_cast<std::size_t>(count));
        const auto end = offset + count; // subtraction-based bounds check above
        std::uint64_t decoded_start = 0;
        for (std::size_t i = 0; i < entry.chunks.size(); ++i) {
            const auto decoded_end = decoded_start + entry.chunks[i];
            if (decoded_start >= end) break;
            if (decoded_end <= offset) { decoded_start = decoded_end; continue; }
            const auto packed_offset = i * block_size;
            const auto packed_size = std::min(block_size, std::size_t(entry.stored) - packed_offset);
            auto packed = impl_->bytes(entry.offset + packed_offset, packed_size);
            if (!packed) return packed;
            auto part = inflate(packed.value, 0, packed_size,
                                entry.chunks[i], i + 1 == entry.chunks.size());
            if (!part) return part;
            const auto first = static_cast<std::size_t>(std::max(offset, decoded_start) - decoded_start);
            const auto last = static_cast<std::size_t>(std::min(end, decoded_end) - decoded_start);
            output.insert(output.end(), part.value.begin() + first, part.value.begin() + last);
            decoded_start = decoded_end;
        }
        return {std::move(output), {}};
    } catch (const std::bad_alloc&) {
        return {{}, fail(ErrorCode::limit, "archive read allocation failed")};
    }
}
} // namespace albion::data
