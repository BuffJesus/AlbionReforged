#include "albion/byte_source.h"
#include "albion/archive.h"
#include <fstream>
#include <limits>
#include <mutex>
#include <new>

namespace albion::data {
namespace {
using Bytes = std::vector<std::uint8_t>;
Error fail(ErrorCode code, const char* message) { return {code, message}; }
class File final : public ByteSource {
public:
    File(std::ifstream stream, std::uint64_t length, std::uint64_t budget)
        : stream_(std::move(stream)), length_(length), budget_(budget) {}
    std::uint64_t size() const noexcept override { return length_; }
    Result<Bytes> ReadRange(std::uint64_t offset, std::uint64_t count) const override {
        if (offset > length_ || count > length_ - offset)
            return {{}, fail(ErrorCode::out_of_range, "file range outside source")};
        if (count > budget_ || count > std::numeric_limits<std::size_t>::max() ||
            count > std::uint64_t(std::numeric_limits<std::streamsize>::max()))
            return {{}, fail(ErrorCode::limit, "file read exceeds budget")};
        try {
            Bytes bytes(static_cast<std::size_t>(count));
            if (count) {
                std::lock_guard<std::mutex> guard(mutex_);
                stream_.clear();
                stream_.seekg(static_cast<std::streamoff>(offset));
                stream_.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(count));
                if (!stream_ || stream_.gcount() != static_cast<std::streamsize>(count))
                    return {{}, fail(ErrorCode::io, "short or failed source read")};
            }
            return {std::move(bytes), {}};
        } catch (const std::bad_alloc&) {
            return {{}, fail(ErrorCode::limit, "source read allocation failed")};
        }
    }
private:
    mutable std::ifstream stream_;
    mutable std::mutex mutex_;
    std::uint64_t length_, budget_;
};
class Slice final : public ByteSource {
public:
    Slice(Source parent, std::uint64_t begin, std::uint64_t end)
        : parent_(std::move(parent)), begin_(begin), length_(end-begin) {}
    std::uint64_t size() const noexcept override { return length_; }
    Result<Bytes> ReadRange(std::uint64_t offset, std::uint64_t count) const override {
        if (offset > length_ || count > length_ - offset)
            return {{}, fail(ErrorCode::out_of_range, "slice read outside source")};
        auto result = parent_->ReadRange(begin_ + offset, count);
        if (result && result.value.size() != count)
            return {{}, fail(ErrorCode::size_mismatch, "source returned wrong byte count")};
        return result;
    }
private:
    Source parent_;
    std::uint64_t begin_, length_;
};
class Entry final : public ByteSource {
public:
    Entry(std::shared_ptr<const Archive> archive, std::size_t index)
        : archive_(std::move(archive)), index_(index) {}
    std::uint64_t size() const noexcept override { return archive_->entries()[index_].size; }
    Result<Bytes> ReadRange(std::uint64_t offset, std::uint64_t count) const override {
        return archive_->ReadRange(index_, offset, count);
    }
private:
    std::shared_ptr<const Archive> archive_;
    std::size_t index_;
};
}
Result<Source> OpenFileSource(const std::filesystem::path& path, std::uint64_t max_read_bytes) {
    try {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file) return {{}, fail(ErrorCode::io, "cannot open source file")};
        const auto end = file.tellg();
        if (end < 0) return {{}, fail(ErrorCode::io, "cannot determine source size")};
        return {std::make_shared<File>(std::move(file), static_cast<std::uint64_t>(end), max_read_bytes), {}};
    } catch (const std::bad_alloc&) {
        return {{}, fail(ErrorCode::limit, "source allocation failed")};
    }
}
Result<Source> SliceSource(Source parent, std::uint64_t begin, std::uint64_t end) {
    if (!parent) return {{}, fail(ErrorCode::malformed, "null slice parent")};
    if (begin > end || end > parent->size())
        return {{}, fail(ErrorCode::out_of_range, "slice outside parent")};
    try { return {std::make_shared<Slice>(std::move(parent), begin, end), {}}; }
    catch (const std::bad_alloc&) { return {{}, fail(ErrorCode::limit, "slice allocation failed")}; }
}
Result<Source> ArchiveEntrySource(std::shared_ptr<const Archive> archive, std::size_t index) {
    if (!archive) return {{}, fail(ErrorCode::malformed, "null entry archive")};
    if (index >= archive->entries().size())
        return {{}, fail(ErrorCode::not_found, "entry index outside archive")};
    try { return {std::make_shared<Entry>(std::move(archive), index), {}}; }
    catch (const std::bad_alloc&) { return {{}, fail(ErrorCode::limit, "entry source allocation failed")}; }
}
} // namespace albion::data
