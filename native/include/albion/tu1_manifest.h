#pragma once
#include "albion/byte_source.h"
#include <string>

namespace albion::compat {
struct ManifestRecord {
    std::string raw_name;
    // One-based nonempty record order. Original manifest-provider token lookup
    // selects this record then opens its stored name through the parent provider;
    // this is not an archive index or proof of a physical file mapping.
    std::uint32_t ordinal{};
    bool terminated{};      // EOF tail takes a different guest conversion path
};
struct ManifestLimits {
    std::uint64_t bytes = 16ull << 20;
    std::uint32_t records = 1000000;
    std::uint32_t line_bytes = 2048;
    std::uint32_t read_bytes = 128u << 10;
};
// ASCII subset of original framing. CR/LF terminate; empty lines are ignored.
// Preserves order, duplicates, case, slashes and whitespace. Rejects NUL/high
// bytes rather than guessing guest conversion. No normalization, hashing, path
// resolution or missing-manifest fallback is performed. Results are all-or-none.
albion::data::Result<std::vector<ManifestRecord>> ReadTu1Manifest(
    albion::data::Source source, ManifestLimits limits = {});
} // namespace albion::compat
