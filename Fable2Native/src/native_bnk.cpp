#include "f2/native_bnk.h"

#include "miniz.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>

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

// Inflate exactly ONE zlib stream starting at src (up to src_len bytes), appending its full
// output to `out` and reporting how many COMPRESSED bytes it consumed (so the caller can find
// the next back-to-back stream). `hint` sizes the scratch buffer. Grounded in the reference
// extractor (tools/lua_mod/script_index.py entry_bytes: decompressobj per chunk, advancing by
// consumed bytes). Returns false on a zlib error before the stream ends.
bool inflate_one_stream(const std::uint8_t* src, std::size_t src_len, std::size_t hint,
                        std::vector<std::uint8_t>& out, std::size_t& consumed) {
    consumed = 0;
    mz_stream s;
    std::memset(&s, 0, sizeof(s));
    if (mz_inflateInit(&s) != MZ_OK) return false;
    s.next_in = src;
    s.avail_in = static_cast<unsigned int>(src_len);
    // Fixed oversized scratch (NOT sized to `hint`): if the output buffer is exactly the
    // decompressed size, miniz can return MZ_BUF_ERROR instead of MZ_STREAM_END because it
    // has no room to emit the end-of-stream on the same pass. Keeping spare avail_out lets it
    // flag MZ_STREAM_END; larger payloads just loop. (`hint` is retained for the API.)
    (void)hint;
    std::vector<std::uint8_t> buf(1u << 16);
    int rc = MZ_OK;
    for (;;) {
        s.next_out = buf.data();
        s.avail_out = static_cast<unsigned int>(buf.size());
        rc = mz_inflate(&s, MZ_NO_FLUSH);
        out.insert(out.end(), buf.data(), buf.data() + (buf.size() - s.avail_out));
        if (rc == MZ_STREAM_END) break;
        if (s.avail_out == 0) continue;   // output buffer full -> more payload pending, loop
        // avail_out > 0 means miniz produced everything it could this pass. MZ_OK/MZ_BUF_ERROR
        // here with no input left = the stream is fully decoded; miniz just doesn't always flag
        // MZ_STREAM_END (mz_uncompress is more lenient, which is why it worked before). Any
        // other code, or leftover input it can't use, is a genuine error.
        if ((rc == MZ_OK || rc == MZ_BUF_ERROR) && s.avail_in == 0) break;
        mz_inflateEnd(&s);
        return false;
    }
    consumed = static_cast<std::size_t>(s.total_in);
    mz_inflateEnd(&s);
    // Success = a clean end, or a fully-consumed stream that produced output (the miniz quirk).
    return rc == MZ_STREAM_END || (s.avail_in == 0 && !out.empty());
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
    cooked_ = false;          // leave any prior open_cooked mode (e.g. a failed cooked probe)
    cooked_paths_.clear();
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

bool BnkReader::open_cooked(const std::string& dir) {
    error_.clear();
    entries_.clear();
    file_.clear();
    cooked_ = true;
    cooked_paths_.clear();
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
        error_ = "cooked dir not found: " + dir;
        return false;
    }
    // Index every regular file by its normalized path relative to the package root. The cooker
    // writes files as "<dir>/scripts/...", so the relative path already carries the "scripts\"
    // prefix and .lua extension that normalize() expects (idempotent for those keys).
    const std::filesystem::path root(dir);
    for (auto it = std::filesystem::recursive_directory_iterator(root, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const std::filesystem::path rel = std::filesystem::relative(it->path(), root, ec);
        if (ec) continue;
        std::string key = rel.generic_string();  // forward-slashes
        if (key.size() < 4 || key.compare(key.size() - 4, 4, ".lua") != 0) continue;  // scripts only
        cooked_paths_.emplace(normalize(key), it->path().string());
    }
    if (cooked_paths_.empty()) {
        error_ = "no cooked scripts under: " + dir;
        return false;
    }
    return true;
}

std::vector<std::string> BnkReader::names() const {
    std::vector<std::string> out;
    if (cooked_) {
        out.reserve(cooked_paths_.size());
        for (const auto& kv : cooked_paths_) out.push_back(kv.first);
    } else {
        out.reserve(entries_.size());
        for (const auto& kv : entries_) out.push_back(kv.first);
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool BnkReader::has(const std::string& name) const {
    const std::string key = normalize(name);
    return cooked_ ? cooked_paths_.find(key) != cooked_paths_.end()
                   : entries_.find(key) != entries_.end();
}

std::vector<std::uint8_t> BnkReader::extract(const std::string& name) {
    const std::string key = normalize(name);
    if (cooked_) {
        auto it = cooked_paths_.find(key);
        if (it == cooked_paths_.end()) {
            error_ = "no cooked script '" + key + "'";
            return {};
        }
        std::ifstream f(it->second, std::ios::binary);
        if (!f) {
            error_ = "cannot read cooked script '" + it->second + "'";
            return {};
        }
        return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(f),
                                         std::istreambuf_iterator<char>());
    }
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
    const std::size_t comp = e.comp_size;
    constexpr std::size_t kBlock = 32768;  // 0x8000
    std::vector<std::uint8_t> out;
    out.reserve(e.decomp_size);

    if (comp > kBlock) {
        // Large entry: fixed 32768-COMPRESSED-byte blocks, each an independent zlib stream
        // (script_index.entry_bytes: `for pos in range(0, len(raw), 32768)`).
        for (std::size_t pos = 0; pos < comp; pos += kBlock) {
            const std::size_t blk = std::min(kBlock, comp - pos);
            std::size_t consumed = 0;
            if (!inflate_one_stream(raw + pos, blk, 0, out, consumed)) {
                error_ = "entry block inflate failed: " + key;
                return {};
            }
        }
    } else {
        // Small entry: back-to-back zlib streams whose boundaries are found by consumption.
        // The TOC's per-chunk decompressed sizes are the targets; fall back to one stream.
        std::vector<std::uint32_t> targets = e.chunk_decomp;
        if (targets.empty()) targets.push_back(e.decomp_size);
        std::size_t pos = 0;
        for (const std::uint32_t target : targets) {
            if (pos >= comp) break;
            std::size_t consumed = 0;
            if (!inflate_one_stream(raw + pos, comp - pos, target, out, consumed)) {
                error_ = "entry chunk inflate failed: " + key;
                return {};
            }
            if (consumed == 0) break;
            pos += consumed;
        }
    }
    if (out.size() > e.decomp_size) out.resize(e.decomp_size);  // truncate to recorded size
    return out;
}

}  // namespace f2
