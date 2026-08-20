#include "f2/native_text.h"

#include "f2/native_gdb_hash.h"

#include <algorithm>
#include <cstring>
#include <fstream>

namespace f2 {
namespace {
std::uint32_t be32(const std::vector<std::uint8_t>& b, std::size_t o) {
    if (o + 4 > b.size()) return 0;
    return (static_cast<std::uint32_t>(b[o]) << 24) | (static_cast<std::uint32_t>(b[o + 1]) << 16) |
           (static_cast<std::uint32_t>(b[o + 2]) << 8) | static_cast<std::uint32_t>(b[o + 3]);
}
}  // namespace

bool TextTable::load(const std::filesystem::path& path) {
    rows_.clear();
    blob_.clear();
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::vector<std::uint8_t> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (b.size() < 16 || std::memcmp(b.data(), "F2TEXT\0\0", 8) != 0) return false;
    const std::uint32_t count = be32(b, 8);
    const std::uint32_t blob_bytes = be32(b, 12);
    const std::size_t rows_at = 16;
    const std::size_t blob_at = rows_at + static_cast<std::size_t>(count) * 8;
    if (blob_at + blob_bytes > b.size()) return false;

    rows_.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::size_t at = rows_at + static_cast<std::size_t>(i) * 8;
        rows_.emplace_back(be32(b, at), be32(b, at + 4));
    }
    blob_.assign(reinterpret_cast<const char*>(b.data() + blob_at),
                 reinterpret_cast<const char*>(b.data() + blob_at + blob_bytes));
    // The cook writes them sorted; only pay for a sort if something upstream changed.
    if (!std::is_sorted(rows_.begin(), rows_.end(),
                        [](const auto& a, const auto& c) { return a.first < c.first; })) {
        std::sort(rows_.begin(), rows_.end(),
                  [](const auto& a, const auto& c) { return a.first < c.first; });
    }
    return true;
}

const char* TextTable::find_hash(std::uint32_t tag_hash) const {
    const auto it = std::lower_bound(rows_.begin(), rows_.end(), tag_hash,
                                     [](const auto& e, std::uint32_t v) { return e.first < v; });
    if (it == rows_.end() || it->first != tag_hash) return nullptr;
    if (it->second >= blob_.size()) return nullptr;
    return blob_.data() + it->second;
}

const char* TextTable::find(std::string_view tag) const {
    if (tag.empty() || rows_.empty()) return nullptr;
    return find_hash(gdb::fnv1(tag));
}

std::string TextTable::get(std::string_view tag) const {
    const char* s = find(tag);
    return s ? std::string(s) : std::string(tag);
}

}  // namespace f2
