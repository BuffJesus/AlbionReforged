#pragma once

// Reader for Fable II script BNK containers (gamescripts_r.bnk / guiscripts.bnk).
//
// Format v3 (byte-verified against the real gamescripts_r.bnk, 555 entries; see the P6
// native-scripting research). Big-endian u32 throughout:
//   [0] base_offset (=0x8000)  [4] version (=3)  [8] u8 compress_file_data (0|1)
//   then from offset 9 a zlib-compressed FILE TABLE, framed as
//     (u32 comp_size, u32 decomp_size, u8[comp_size])* terminated by comp_size==0,
//     the comp bytes concatenated = ONE zlib stream -> the TOC blob.
//   TOC blob: u32 file_count; per entry { u32 name_len; u8[name_len] name (trailing NUL);
//     u32 rel_off; u32 decomp_size; u32 comp_size; u32 chunk_count;
//     u32 chunk_decomp_size[chunk_count] }.
//   Names are backslash + lowercase, e.g. "scripts\miscellaneous\generalsetupscript.lua".
//   An entry's data lives at file[base_offset+rel_off .. +comp_size] and is split into
//   INDEPENDENT zlib streams every 0x8000 COMPRESSED bytes; inflate each and concatenate
//   to get the payload (552/555 are LuaQ bytecode starting 1B 4C 75 61 51 00).

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace f2 {

class BnkReader {
public:
    // Read + parse the BNK at `path`. Returns false + sets error() on any failure.
    bool open(const std::string& path);

    // Open a COOKED script package produced by tools/cook_scripts.py: a directory of already-
    // decompressed entry files (named by their normalized "scripts\...\*.lua" path). extract()
    // then just reads a file — no zlib. This is the port-plan "consume the native package"
    // path; open() (raw BNK) remains the fallback. Returns false + sets error() if the dir has
    // no script files. Grounded in cook_scripts' layout (docs/NATIVE_PORT_PLAN.md).
    bool open_cooked(const std::string& dir);

    [[nodiscard]] bool is_open() const noexcept {
        return cooked_ ? !cooked_paths_.empty() : (!file_.empty() && !entries_.empty());
    }
    [[nodiscard]] const std::string& error() const noexcept { return error_; }
    [[nodiscard]] std::size_t entry_count() const noexcept {
        return cooked_ ? cooked_paths_.size() : entries_.size();
    }

    // The normalized (backslash-lowercase, "scripts\"-prefixed) names of all entries.
    [[nodiscard]] std::vector<std::string> names() const;
    [[nodiscard]] bool has(const std::string& name) const;

    // Decompress an entry to its raw payload (LuaQ bytecode or Lua source). `name` is
    // normalized the same way as the TOC keys, so a RunScript-style "miscellaneous/foo"
    // or "foo.lua" resolves. Empty vector + error() set if missing/corrupt.
    [[nodiscard]] std::vector<std::uint8_t> extract(const std::string& name);

    // Normalize a lookup name to the TOC key form (tolower, '/'->'\\', prepend "scripts\\"
    // when absent, append ".lua" when there is no extension).
    [[nodiscard]] static std::string normalize(const std::string& name);

private:
    struct Entry {
        std::uint32_t rel_off = 0;
        std::uint32_t decomp_size = 0;
        std::uint32_t comp_size = 0;
        std::vector<std::uint32_t> chunk_decomp;  // per-0x8000-chunk output sizes
    };

    std::vector<std::uint8_t> file_;             // whole BNK bytes
    std::uint32_t base_offset_ = 0;
    std::unordered_map<std::string, Entry> entries_;  // normalized name -> entry
    std::string error_;

    bool cooked_ = false;                             // true after open_cooked()
    std::unordered_map<std::string, std::string> cooked_paths_;  // normalized name -> file path
};

}  // namespace f2
