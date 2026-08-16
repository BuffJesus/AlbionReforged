#pragma once

// Own-format game-state save/restore for the native port.
//
// Grounded in ghidra_out/gamestate_save_restore.txt. Faithful to the retail MODEL,
// NOT the retail byte format (the provider vtbl+0x10 iteration schema and each
// component's LoadFromStream byte layout are Ghidra-only gaps G-1/G-2/G-3, so retail
// .sav interop is out of scope). What IS reproduced:
//   * ONE BIDIRECTIONAL VISITOR — a single WorldArchive whose direction is a runtime
//     flag (retail: one provider, save dir=0 / load dir=1; §0/§B.1). Each `visit()`
//     reads OR writes the same field, so save and load share one code path.
//   * PER-ENTITY-PER-COMPONENT DELTA over the baseline (§D): on load the world is
//     first rebuilt from the cooked scene (the GDB-default analogue, spawn_from_scene),
//     then the saved per-component state is OVERLAID (retail LoadFromStream +0x24) keyed
//     by entity UID. Length-framed components so an unknown/absent one is skipped.
//   * The header state: a ChapterHeader (chaptersave.bin analogue) + the 150-bit quest
//     completion bitfield (Save_BuildProgressMeta @0x82444DA8, §C.3) + the hero position.
//
// Own-format is little-endian host bytes (retail is BE PPC) — this is the PC port's
// private save, versioned by a magic + version so the format can evolve.

#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace f2 {

enum class ArchiveMode : std::uint8_t { Write, Read };

// The single bidirectional visitor. Construct in Write mode to serialize (then take()
// the bytes), or from a byte buffer to deserialize. Every visit() dispatches on mode_.
class WorldArchive {
public:
    explicit WorldArchive(ArchiveMode mode) noexcept : mode_(mode) {}
    explicit WorldArchive(std::vector<std::uint8_t> data) noexcept
        : mode_(ArchiveMode::Read), buffer_(std::move(data)) {}

    [[nodiscard]] ArchiveMode mode() const noexcept { return mode_; }
    [[nodiscard]] bool reading() const noexcept { return mode_ == ArchiveMode::Read; }
    [[nodiscard]] bool ok() const noexcept { return ok_; }  // false once a read overruns
    [[nodiscard]] std::size_t position() const noexcept { return pos_; }

    void visit(std::uint8_t& v) { raw(&v, 1); }
    void visit(std::uint16_t& v) { raw(&v, 2); }
    void visit(std::uint32_t& v) { raw(&v, 4); }
    void visit(std::uint64_t& v) { raw(&v, 8); }
    void visit(float& v) { raw(&v, 4); }
    void visit(bool& v) {
        std::uint8_t b = v ? 1u : 0u;
        raw(&b, 1);
        v = b != 0;
    }
    void visit_i32(int& v) {
        std::int32_t t = static_cast<std::int32_t>(v);
        raw(&t, 4);
        v = static_cast<int>(t);
    }
    void visit(std::array<float, 3>& v) {
        for (auto& c : v) visit(c);
    }
    void visit(std::string& v);  // u32 length + bytes

    template <std::size_t N>
    void visit(std::bitset<N>& bs) {
        constexpr std::size_t bytes = (N + 7) / 8;
        for (std::size_t i = 0; i < bytes; ++i) {
            std::uint8_t byte = 0;
            if (!reading()) {
                for (std::size_t b = 0; b < 8; ++b) {
                    const std::size_t bit = i * 8 + b;
                    if (bit < N && bs.test(bit)) byte |= static_cast<std::uint8_t>(1u << b);
                }
            }
            raw(&byte, 1);
            if (reading()) {
                for (std::size_t b = 0; b < 8; ++b) {
                    const std::size_t bit = i * 8 + b;
                    if (bit < N) bs.set(bit, (byte & (1u << b)) != 0);
                }
            }
        }
    }

    // Raw byte block (used for length-framed component blobs).
    void write_bytes(const std::vector<std::uint8_t>& b);
    [[nodiscard]] std::vector<std::uint8_t> read_bytes(std::size_t n);

    [[nodiscard]] std::vector<std::uint8_t> take() noexcept { return std::move(buffer_); }
    [[nodiscard]] const std::vector<std::uint8_t>& bytes() const noexcept { return buffer_; }

private:
    void raw(void* p, std::size_t n);

    ArchiveMode mode_;
    std::vector<std::uint8_t> buffer_;
    std::size_t pos_ = 0;
    bool ok_ = true;
};

// Gameflow header (chaptersave.bin analogue, §A.4/§B.3) + quest-completion bitfield.
struct ChapterHeader {
    std::uint32_t version = 1;
    std::uint32_t episode = 0;
    std::uint32_t chapter = 0;
};

// The top-level persisted game state that is NOT on the entity graph: the chapter
// header + the 150-bit quest completion summary (§C.3) + the hero position.
struct NativeGameState {
    ChapterHeader header;
    std::bitset<150> quest_completion;  // Save_BuildProgressMeta 0x82444DA8, i=0..149
    std::array<float, 3> hero_position{0.0f, 0.0f, 0.0f};

    void serialize(WorldArchive& ar);
};

}  // namespace f2
