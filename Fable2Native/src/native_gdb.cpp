#include "f2/native_gdb.h"

#include "f2/native_gdb_hash.h"

#include <algorithm>
#include <cstring>
#include <fstream>

namespace f2::gdb {
namespace {
constexpr std::size_t kHeader = 0x18;
}  // namespace

std::uint32_t GdbFile::be32(std::size_t off) const {
    if (off + 4 > b_.size()) return 0;
    return (static_cast<std::uint32_t>(b_[off]) << 24) |
           (static_cast<std::uint32_t>(b_[off + 1]) << 16) |
           (static_cast<std::uint32_t>(b_[off + 2]) << 8) |
           static_cast<std::uint32_t>(b_[off + 3]);
}

bool GdbFile::open(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(f)),
                                    std::istreambuf_iterator<char>());
    return parse(std::move(bytes));
}

bool GdbFile::parse(std::vector<std::uint8_t> bytes) {
    ok_ = false;
    b_ = std::move(bytes);
    offsets_.clear();
    names_.clear();
    if (b_.size() < kHeader || std::memcmp(b_.data(), "GDB\0", 4) != 0) return false;

    count_ = be32(0x04);
    const std::uint32_t size_a = be32(0x08);
    const std::uint32_t size_b = be32(0x0C);
    const std::uint32_t name_count = be32(0x10);
    if (count_ == 0) return false;

    schema_base_ = kHeader + size_a;
    hash_base_ = schema_base_ + size_b;
    body_end_ = schema_base_;
    if (hash_base_ + static_cast<std::size_t>(count_) * 4 > b_.size()) return false;

    // Walk the record bodies: each record is a 4-byte schema pointer + one u32 per field.
    offsets_.reserve(count_);
    std::size_t cur = kHeader;
    for (std::uint32_t i = 0; i < count_; ++i) {
        if (cur + 4 > body_end_) return false;
        offsets_.push_back(cur);
        std::size_t schema_off = 0;
        std::uint32_t fc = 0;
        if (!schema_at(cur, schema_off, fc)) return false;
        cur += 4 + static_cast<std::size_t>(fc) * 4;
        if (cur > body_end_) return false;
    }

    // Name table: name_count * {fnv1(name), guid}, already sorted ascending on disk.
    const std::size_t offset_base = hash_base_ + static_cast<std::size_t>(count_) * 4;
    const std::size_t name_base = (offset_base + static_cast<std::size_t>(count_) * 2 + 3) & ~std::size_t{3};
    if (name_count > 0 && name_base + static_cast<std::size_t>(name_count) * 8 <= b_.size()) {
        names_.reserve(name_count);
        for (std::uint32_t i = 0; i < name_count; ++i) {
            const std::size_t at = name_base + static_cast<std::size_t>(i) * 8;
            names_.emplace_back(be32(at), be32(at + 4));
        }
        // Defensive: the shipped files are sorted, but a lower_bound on unsorted data is silent
        // nonsense, so only trust it when it really is ordered.
        if (!std::is_sorted(names_.begin(), names_.end(),
                            [](const auto& a, const auto& b) { return a.first < b.first; })) {
            std::sort(names_.begin(), names_.end(),
                      [](const auto& a, const auto& b) { return a.first < b.first; });
        }
    }

    // STRING TABLE, immediately after the name table: { 0x00010000, byteSize, stringCount } then
    // stringCount * { u32 fnv1(s), NUL-terminated bytes }. It is the value pool for type-4 fields
    // AND the source of field names, so a record can describe itself.
    {
        const std::size_t at = name_base + static_cast<std::size_t>(name_count) * 8;
        if (at + 12 <= b_.size() && be32(at) == 0x00010000u) {
            const std::uint32_t n = be32(at + 8);
            std::size_t off = at + 12;
            strings_.reserve(n);
            for (std::uint32_t i = 0; i < n && off + 4 < b_.size(); ++i) {
                const std::uint32_t h = be32(off);
                off += 4;
                const std::size_t start = off;
                while (off < b_.size() && b_[off] != 0) ++off;
                if (off >= b_.size()) break;
                strings_.emplace_back(h, static_cast<std::uint32_t>(string_blob_.size()));
                string_blob_.insert(string_blob_.end(), b_.begin() + static_cast<std::ptrdiff_t>(start),
                                    b_.begin() + static_cast<std::ptrdiff_t>(off));
                string_blob_.push_back(char{0});
                ++off;  // skip the terminator
            }
            std::sort(strings_.begin(), strings_.end(),
                      [](const auto& a, const auto& c) { return a.first < c.first; });
        }
    }

    ok_ = true;
    return true;
}

const char* GdbFile::intern(std::uint32_t hash) const {
    const auto it = std::lower_bound(strings_.begin(), strings_.end(), hash,
                                     [](const auto& e, std::uint32_t v) { return e.first < v; });
    if (it == strings_.end() || it->first != hash) return nullptr;
    return string_blob_.data() + it->second;
}

const char* GdbFile::field_string(std::uint32_t guid, std::uint32_t field) const {
    const auto raw = field_raw(guid, field, kTypeString);
    return raw ? intern(*raw) : nullptr;
}

bool GdbFile::schema_at(std::size_t record, std::size_t& schema_off, std::uint32_t& field_count) const {
    if (record + 4 > body_end_) return false;
    schema_off = schema_base_ + be32(record);
    if (schema_off + 4 > hash_base_) return false;
    const std::uint32_t header = be32(schema_off);
    std::uint32_t fc = header >> 8;
    if (fc > 256) {  // extended field count (rare) — low two bytes + a third byte, like the reader
        if (schema_off + 3 > b_.size()) return false;
        fc = static_cast<std::uint32_t>(b_[schema_off] | (b_[schema_off + 1] << 8)) + b_[schema_off + 2];
        if (fc > 1024) return false;
    }
    if (schema_off + 4 + static_cast<std::size_t>(fc) * 8 > hash_base_) return false;
    field_count = fc;
    return true;
}

std::optional<std::size_t> GdbFile::record_offset(std::uint32_t guid) const {
    if (!ok_) return std::nullopt;
    // Binary search the sorted record-GUID table at hash_base_.
    std::uint32_t lo = 0, hi = count_;
    while (lo < hi) {
        const std::uint32_t mid = lo + (hi - lo) / 2;
        if (be32(hash_base_ + static_cast<std::size_t>(mid) * 4) < guid) lo = mid + 1;
        else hi = mid;
    }
    if (lo >= count_ || be32(hash_base_ + static_cast<std::size_t>(lo) * 4) != guid)
        return std::nullopt;
    return offsets_[lo];
}

bool GdbFile::has_record(std::uint32_t guid) const { return record_offset(guid).has_value(); }

std::optional<std::uint32_t> GdbFile::guid_for_name(std::string_view name) const {
    if (!ok_ || names_.empty()) return std::nullopt;
    const std::uint32_t h = fnv1(name);
    const auto it = std::lower_bound(names_.begin(), names_.end(), h,
                                     [](const auto& e, std::uint32_t v) { return e.first < v; });
    if (it == names_.end() || it->first != h) return std::nullopt;
    return it->second;
}

std::optional<std::uint32_t> GdbFile::field_local(std::size_t record, std::uint32_t field,
                                                  std::uint8_t type) const {
    std::size_t schema_off = 0;
    std::uint32_t fc = 0;
    if (!schema_at(record, schema_off, fc)) return std::nullopt;
    const std::size_t hashes = schema_off + 4;
    const std::size_t descs = hashes + static_cast<std::size_t>(fc) * 4;
    for (std::uint32_t i = 0; i < fc; ++i) {
        if (be32(hashes + static_cast<std::size_t>(i) * 4) != field) continue;
        if ((be32(descs + static_cast<std::size_t>(i) * 4) >> 24) != type) continue;
        return be32(record + 4 + static_cast<std::size_t>(i) * 4);
    }
    return std::nullopt;
}

std::optional<std::uint32_t> GdbFile::field_raw(std::uint32_t guid, std::uint32_t field,
                                                std::uint8_t type) const {
    auto rec = record_offset(guid);
    int depth = 0;
    while (rec && depth++ < 64) {
        if (auto v = field_local(*rec, field, type)) return v;
        // Not defined here — follow kHashParent up the archetype chain.
        auto parent = field_local(*rec, kHashParent, kTypeRecord);
        if (!parent) return std::nullopt;
        rec = record_offset(*parent);
    }
    return std::nullopt;
}

std::optional<float> GdbFile::field_float(std::uint32_t guid, std::uint32_t field) const {
    auto raw = field_raw(guid, field, kTypeFloat);
    if (!raw) return std::nullopt;
    float out = 0.0f;
    const std::uint32_t bits = *raw;
    std::memcpy(&out, &bits, sizeof(out));  // stored as raw IEEE-754 bits
    return out;
}

// ---- GdbDatabase ----

bool GdbDatabase::add(const std::filesystem::path& path) {
    GdbFile f;
    if (!f.open(path)) return false;
    files_.push_back(std::move(f));
    return true;
}

std::optional<std::uint32_t> GdbDatabase::guid_for_name(std::string_view name) const {
    for (const auto& f : files_)
        if (auto g = f.guid_for_name(name)) return g;
    return std::nullopt;
}

bool GdbDatabase::record_exists(std::string_view name) const {
    auto guid = guid_for_name(name);
    if (!guid) return false;
    for (const auto& f : files_)
        if (f.has_record(*guid)) return true;
    return false;
}

std::optional<std::uint32_t> GdbDatabase::field_raw(std::uint32_t guid, std::string_view field,
                                                    std::uint8_t type) const {
    const std::uint32_t fh = fnv1(field);
    for (const auto& f : files_)
        if (auto v = f.field_raw(guid, fh, type)) return v;
    return std::nullopt;
}

const char* GdbDatabase::field_string(std::uint32_t guid, std::string_view field) const {
    const std::uint32_t fh = fnv1(field);
    // The value's intern pool belongs to whichever file holds the record, but a cross-file value
    // can resolve elsewhere, so fall back to scanning every file's pool for the key.
    for (const auto& f : files_)
        if (const char* s = f.field_string(guid, fh)) return s;
    for (const auto& f : files_)
        if (const auto raw = f.field_raw(guid, fh, gdb::kTypeString))
            for (const auto& g : files_)
                if (const char* s = g.intern(*raw)) return s;
    return nullptr;
}

std::optional<float> GdbDatabase::field_float(std::uint32_t guid, std::string_view field) const {
    const std::uint32_t fh = fnv1(field);
    for (const auto& f : files_)
        if (auto v = f.field_float(guid, fh)) return v;
    return std::nullopt;
}

}  // namespace f2::gdb
