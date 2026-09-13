#pragma once
#include "albion/tu1_table_capture.h"

namespace albion::compat {
enum class ManifestNameStatus : std::uint32_t {
    unreadable, unsupported_object, invalid, copied, changed
};
struct Tu1ManifestName {
    ManifestNameStatus status{ManifestNameStatus::unreadable};
    std::uint32_t parent{}, parent_vtable{}, list{}, record{}, ordinal{}, units{};
    std::array<std::uint8_t, 4096> utf16be{};
};

// Diagnostic only: original 8200DEC8 manifest-provider layout, 36-byte records.
// Caller retains the provider and holds its owning manager lock. Read copies a
// whole span or fails. No allocation, guest calls/writes or file I/O. Repeated
// reads detect some concurrent changes, not an atomic snapshot or ABA changes.
// Preserve counted UTF-16 units including NUL/surrogates; do not normalize or
// treat the observed parent address as a physical file identity.
template<class Read>
void CaptureTu1ManifestName(Tu1ManifestName& out, std::uint32_t object,
                            std::uint32_t token, Read&& read) {
    out = {};
    std::array<std::uint8_t, 20> provider{}, provider_again{}, list{}, list_again{};
    std::array<std::uint8_t, 36> record{}, record_again{};
    std::array<std::uint8_t, 4> parent_type{}, parent_again{};
    if (!object || object > UINT32_MAX - 19 || !read(object, provider.data(), 4)) return;
    if (CaptureBe32(provider.data()) != 0x8200DEC8) {
        out.status = ManifestNameStatus::unsupported_object;
        return;
    }
    if (!read(object, provider.data(), provider.size())) return;
    if (CaptureBe32(provider.data()) != 0x8200DEC8) {
        out.status = ManifestNameStatus::changed;
        return;
    }
    const auto parent = CaptureBe32(provider.data() + 8);
    const auto names = CaptureBe32(provider.data() + 12);
    out.status = ManifestNameStatus::invalid;
    if (!parent || parent > UINT32_MAX - 3 || !names || names > UINT32_MAX - 19) return;
    out.status = ManifestNameStatus::unreadable;
    if (!read(names, list.data(), list.size()) || !read(parent, parent_type.data(), 4)) return;
    const auto begin = CaptureBe32(list.data() + 12);
    const auto end = CaptureBe32(list.data() + 16);
    out.status = ManifestNameStatus::invalid;
    if (!begin || begin > end || (end - begin) % 36) return;
    const auto count = (end - begin) / 36;
    if (count > 65536 || !token || token > count) return;
    const auto address = begin + (token - 1) * 36;
    out.status = ManifestNameStatus::unreadable;
    if (!read(address, record.data(), record.size())) return;
    const auto units = CaptureBe32(record.data() + 20);
    const auto capacity = CaptureBe32(record.data() + 24);
    const auto source = capacity < 8 ? address + 4 : CaptureBe32(record.data() + 4);
    out.status = ManifestNameStatus::invalid;
    if (units > 2048 || units > capacity || !source ||
        std::uint64_t(units) * 2 > (std::uint64_t{1} << 32) - source) return;
    std::array<std::uint8_t, 4096> payload{}, payload_again{};
    out.status = ManifestNameStatus::unreadable;
    if ((units && (!read(source, payload.data(), units * 2) ||
                   !read(source, payload_again.data(), units * 2))) ||
        !read(address, record_again.data(), record_again.size()) ||
        !read(names, list_again.data(), list_again.size()) ||
        !read(parent, parent_again.data(), 4) ||
        !read(object, provider_again.data(), provider_again.size())) return;
    if (payload != payload_again || record != record_again || list != list_again ||
        provider != provider_again || parent_type != parent_again) {
        out.status = ManifestNameStatus::changed;
        return;
    }
    out.parent = parent;
    out.parent_vtable = CaptureBe32(parent_type.data());
    out.list = names;
    out.record = address;
    out.ordinal = CaptureBe32(record.data() + 32);
    out.units = units;
    out.utf16be = payload;
    out.status = ManifestNameStatus::copied;
}
} // namespace albion::compat
