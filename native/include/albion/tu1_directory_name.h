#pragma once
#include "albion/tu1_table_capture.h"

namespace albion::compat {
enum class DirectoryNameStatus : std::uint32_t {
    unreadable = 0, unsupported_object = 1, invalid = 2, copied = 3
};
struct Tu1DirectoryName {
    DirectoryNameStatus status{DirectoryNameStatus::unreadable};
    std::uint32_t vtable{}, units{};
    std::array<std::uint8_t, 1026> utf16be{}; // at most 512 units plus terminator
};

// Only the constructor-verified 0x8200DEA0 directory layout is recognized.
// This copies the supplied open name, not a resolved physical file identity.
// The caller holds the manager lock and keeps its mount objects alive.
template<class Read>
void CaptureTu1DirectoryName(Tu1DirectoryName& out, std::uint32_t object, Read&& read) {
    out.status = DirectoryNameStatus::unreadable;
    out.units = out.vtable = 0;
    std::array<std::uint8_t, 36> fields{}, again{};
    if (!object || object > UINT32_MAX - 35 || !read(object, fields.data(), 4)) return;
    out.vtable = CaptureBe32(fields.data());
    if (out.vtable != 0x8200DEA0) {
        out.status = DirectoryNameStatus::unsupported_object;
        return;
    }
    if (!read(object, fields.data(), fields.size())) return;
    if (CaptureBe32(fields.data()) != out.vtable) return;
    const auto units = CaptureBe32(fields.data() + 0x1c);
    const auto capacity = CaptureBe32(fields.data() + 0x20);
    out.status = DirectoryNameStatus::invalid;
    if (units > 512 || units > capacity) return;
    const auto source = capacity < 8 ? object + 0xc : CaptureBe32(fields.data() + 0xc);
    const auto bytes = (units + 1) * 2;
    if (!source || bytes > (std::uint64_t{1} << 32) - source) return;
    out.status = DirectoryNameStatus::unreadable;
    if (!read(source, out.utf16be.data(), bytes) ||
        !read(object, again.data(), again.size()) || fields != again) return;
    out.status = DirectoryNameStatus::invalid;
    if (out.utf16be[bytes - 2] || out.utf16be[bytes - 1]) return;
    // Reject embedded NULs and unpaired surrogates; never silently alter a name.
    for (std::size_t i = 0; i < units; ++i) {
        const auto unit = (std::uint32_t(out.utf16be[2*i]) << 8) | out.utf16be[2*i+1];
        if (!unit || (unit >= 0xdc00 && unit <= 0xdfff)) return;
        if (unit >= 0xd800 && unit <= 0xdbff) {
            if (++i == units) return;
            const auto low = (std::uint32_t(out.utf16be[2*i]) << 8) | out.utf16be[2*i+1];
            if (low < 0xdc00 || low > 0xdfff) return;
        }
    }
    out.units = units;
    out.status = DirectoryNameStatus::copied;
}
} // namespace albion::compat
