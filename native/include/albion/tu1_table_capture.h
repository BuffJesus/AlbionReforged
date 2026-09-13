#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace albion::compat {
// Caller supplies storage before entering the guest manager lock. No allocation,
// guest writes, pointer chasing into mount objects, or file I/O occurs here.
struct Tu1TableCapture {
    static constexpr std::size_t max_records = 65536;
    static constexpr std::size_t max_mounts = 256;
    std::uint32_t manager{}, query{}, record_bytes{}, mount_bytes{};
    std::array<std::uint8_t, max_records * 12> records{};
    std::array<std::uint8_t, max_mounts * 36> mounts{};
    bool complete{};
};
inline std::uint32_t CaptureBe32(const std::uint8_t* p) noexcept {
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) |
           (std::uint32_t(p[2]) << 8) | p[3];
}

// Only the indexed lookup callsite is eligible. Read must copy a whole readable
// guest span or return false. Caller must hold the manager lock throughout.
template<class Read>
bool CaptureTu1Table(Tu1TableCapture& out, std::uint64_t lr,
                     std::uint32_t manager, std::uint64_t begin_iterator,
                     std::uint64_t end_iterator, std::uint32_t query_pointer,
                     Read&& read) {
    out.complete = false;
    out.record_bytes = out.mount_bytes = 0;
    if (lr != 0x82B45574 || !manager || manager > UINT32_MAX - 0x40 ||
        (begin_iterator >> 32) != manager + 0x34 ||
        (end_iterator >> 32) != manager + 0x34) return false;
    std::array<std::uint8_t, 24> fields{}, again{};
    std::array<std::uint8_t, 4> query{};
    if (!read(manager + 0x28, fields.data(), fields.size())) return false;
    const auto mb = CaptureBe32(fields.data());
    const auto me = CaptureBe32(fields.data() + 4);
    const auto rb = CaptureBe32(fields.data() + 16);
    const auto re = CaptureBe32(fields.data() + 20);
    if (rb != std::uint32_t(begin_iterator) || re != std::uint32_t(end_iterator) ||
        me < mb || re < rb || (me - mb) % 36 || (re - rb) % 12 ||
        me - mb > out.mounts.size() || re - rb > out.records.size() ||
        ((me != mb) && !mb) || ((re != rb) && !rb) ||
        !query_pointer || query_pointer > UINT32_MAX - 3) return false;
    if (!read(query_pointer, query.data(), query.size()) ||
        (me != mb && !read(mb, out.mounts.data(), me - mb)) ||
        (re != rb && !read(rb, out.records.data(), re - rb)) ||
        !read(manager + 0x28, again.data(), again.size()) || fields != again) return false;
    for (std::size_t i = 0; i < re - rb; i += 12) {
        if (CaptureBe32(out.records.data() + i + 4) >= (me - mb) / 36 ||
            (i && CaptureBe32(out.records.data() + i - 12) >
                  CaptureBe32(out.records.data() + i))) return false;
    }
    out.manager = manager;
    out.query = CaptureBe32(query.data());
    out.record_bytes = re - rb;
    out.mount_bytes = me - mb;
    out.complete = true;
    return true;
}
} // namespace albion::compat
