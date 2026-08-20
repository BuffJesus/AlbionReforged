#pragma once

// The game's localised text: TextTag -> the real in-game string.
//
// The scripts and the authored data address every player-visible string by TAG, never by literal:
// a cutscene beat carries TextTag='TEXT_QUEST_QC010_JEEVES_GREET_02', a level carries
// 'TEXT_LEVEL_FAIRFAX_CASTLE', a counter carries 'TEXT_QUEST_QC010_GOLD_10'. So anything the port
// shows — dialogue, subtitles, quest names, menu labels — should resolve through here, and then it
// reads in the game's own words (and in the player's own language, since the tables are per-locale).
//
// Source: data/language/<locale>/text/book.babel, cooked by tools/babel_text.py into a compact
// runtime package:
//     "F2TEXT\0\0" u32 count, u32 blobBytes
//     count * { u32 fnv1(tag), u32 utf8Offset }      -- sorted ascending by hash
//     blobBytes of NUL-terminated UTF-8
// (book.babel itself is an index of {fnv1(tag), blockKey, offset} over 333 **zlib** blocks of
// UTF-16BE strings — the "unknown codec" this project carried for a long time was plain deflate.)
//
// Lookup is FNV-1 of the tag + one binary search, matching how the game keys it.

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace f2 {

class TextTable {
public:
    // Load a cooked .f2text package. Returns false if absent/malformed (the port then simply has
    // no text and callers fall back to the tag, which is what the game does for a missing tag).
    bool load(const std::filesystem::path& path);

    [[nodiscard]] bool valid() const noexcept { return !rows_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return rows_.size(); }

    // The string for a tag, or nullptr when the tag is unknown.
    [[nodiscard]] const char* find(std::string_view tag) const;
    [[nodiscard]] const char* find_hash(std::uint32_t tag_hash) const;

    // The string for a tag, or the tag itself when unknown — what a UI wants.
    [[nodiscard]] std::string get(std::string_view tag) const;

private:
    std::vector<std::pair<std::uint32_t, std::uint32_t>> rows_;  // {fnv1(tag), offset}, sorted
    std::vector<char> blob_;
};

}  // namespace f2
