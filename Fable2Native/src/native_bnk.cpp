#include "f2/native_bnk.h"

#include "miniz.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>

namespace f2 {

namespace {
std::uint32_t be32(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
}

// Inflate a full zlib stream (`src`,`src_len`) into a buffer of exactly `out_size` bytes.
// Returns false on any zlib error / size mismatch.
bool zinflate(const std::uint8_t* src, std::size_t src_len, std::size_t out_size,
              std::vector<std::uint8_t>& out) {
    out.assign(out_size, 0);
    if (out_size == 0) return true;
    mz_ulong dst_len = static_cast<mz_ulong>(out_size);
    const int rc = mz_uncompress(out.data(), &dst_len, src, static_cast<mz_ulong>(src_len));
    return rc == MZ_OK && dst_len == out_size;
}
}  // namespace

std::string BnkReader::normalize(const std::string& name) {
    std::string s = name;
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::replace(s.begin(), s.end(), '/', '\\');
    if (s.find(".") == std::string::npos) s += ".lua";
    if (s.rfind("scripts\\", 0) != 0) s = "scripts\\" + s;
    return s;
}

bool BnkReader::open(const std::string& path) {
    error_.clear();
    entries_.clear();
    file_.clear();
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        error_ = "cannot open '" + path + "'";
        return false;
    }
    file_.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    if (file_.size() < 12) {
        error_ = "file too small";
        return false;
    }
    base_offset_ = be32(&file_[0]);
    const std::uint32_t version = be32(&file_[4]);
    if (version != 3) {
        error_ = "unsupported BNK version " + std::to_string(version);
        return false;
    }
    const std::uint8_t flag = file_[8];
    if (flag != 0 && flag != 1) {
        error_ = "unsupported compress flag";
        return false;
    }

    // Concatenate the file-table's compressed pieces (one zlib stream) + sum decomp size.
    std::size_t pos = 9;
    std::vector<std::uint8_t> tbl_comp;
    std::size_t tbl_decomp = 0;
    while (pos + 8 <= file_.size()) {
        const std::uint32_t comp_size = be32(&file_[pos]); pos += 4;
        const std::uint32_t decomp_size = be32(&file_[pos]); pos += 4;
        if (comp_size == 0) break;
        if (pos + comp_size > file_.size()) { error_ = "truncated file table"; return false; }
        tbl_comp.insert(tbl_comp.end(), file_.begin() + static_cast<std::ptrdiff_t>(pos),
                        file_.begin() + static_cast<std::ptrdiff_t>(pos + comp_size));
        pos += comp_size;
        tbl_decomp += decomp_size;
    }
    std::vector<std::uint8_t> tbl;
    if (!zinflate(tbl_comp.data(), tbl_comp.size(), tbl_decomp, tbl)) {
        error_ = "file-table inflate failed";
        return false;
    }

    // Parse the TOC blob.
    std::size_t tp = 0;
    auto ru32 = [&](std::uint32_t& v) -> bool {
        if (tp + 4 > tbl.size()) return false;
        v = be32(&tbl[tp]); tp += 4; return true;
    };
    std::uint32_t count = 0;
    if (!ru32(count) || count > 100000) { error_ = "bad TOC count"; return false; }
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint32_t name_len = 0;
        if (!ru32(name_len) || tp + name_len > tbl.size()) { error_ = "bad TOC name"; return false; }
        std::string name(reinterpret_cast<const char*>(&tbl[tp]), name_len);
        tp += name_len;
        if (!name.empty() && name.back() == '\0') name.pop_back();
        std::transform(name.begin(), name.end(), name.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        Entry e;
        std::uint32_t chunk_count = 0;
        if (!ru32(e.rel_off) || !ru32(e.decomp_size) || !ru32(e.comp_size) || !ru32(chunk_count)) {
            error_ = "bad TOC entry"; return false;
        }
        if (chunk_count > 100000) { error_ = "bad chunk count"; return false; }
        e.chunk_decomp.resize(chunk_count);
        for (std::uint32_t c = 0; c < chunk_count; ++c) {
            if (!ru32(e.chunk_decomp[c])) { error_ = "bad chunk table"; return false; }
        }
        entries_.emplace(std::move(name), std::move(e));
    }
    return !entries_.empty();
}

std::vector<std::string> BnkReader::names() const {
    std::vector<std::string> out;
    out.reserve(entries_.size());
    for (const auto& kv : entries_) out.push_back(kv.first);
    std::sort(out.begin(), out.end());
    return out;
}

bool BnkReader::has(const std::string& name) const {
    return entries_.find(normalize(name)) != entries_.end();
}

std::vector<std::uint8_t> BnkReader::extract(const std::string& name) {
    const std::string key = normalize(name);
    auto it = entries_.find(key);
    if (it == entries_.end()) {
        error_ = "no BNK entry '" + key + "'";
        return {};
    }
    const Entry& e = it->second;
    const std::size_t base = base_offset_ + e.rel_off;
    if (base + e.comp_size > file_.size()) {
        error_ = "entry data out of range";
        return {};
    }
    const std::uint8_t* raw = file_.data() + base;
    constexpr std::uint32_t kChunkStride = 0x8000;  // 32KB COMPRESSED stride, independent streams
    std::vector<std::uint8_t> out;
    out.reserve(e.decomp_size);
    for (std::size_t c = 0; c < e.chunk_decomp.size(); ++c) {
        const std::uint32_t coff = static_cast<std::uint32_t>(c) * kChunkStride;
        if (coff >= e.comp_size) break;
        const std::uint32_t cin = std::min(kChunkStride, e.comp_size - coff);
        std::vector<std::uint8_t> part;
        if (!zinflate(raw + coff, cin, e.chunk_decomp[c], part)) {
            error_ = "entry chunk inflate failed: " + key;
            return {};
        }
        out.insert(out.end(), part.begin(), part.end());
    }
    return out;
}

}  // namespace f2
