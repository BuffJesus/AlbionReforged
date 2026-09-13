#include "albion/tu1_manifest_name.h"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <vector>

int main() {
    using namespace albion::compat;
    std::vector<std::uint8_t> memory(16384);
    Tu1ManifestName out;
    int checks = 0, calls = 0;
    auto check = [&](bool ok) {
        ++checks;
        if (!ok) { std::cerr << "failed check " << checks << '\n'; std::exit(1); }
    };
    auto put = [&](std::size_t p, std::uint32_t value) {
        for (int b = 0; b < 4; ++b) memory[p+b] = std::uint8_t(value >> (24-8*b));
    };
    auto read = [&](std::uint32_t p, void* dest, std::size_t size) {
        ++calls;
        if (p > memory.size() || size > memory.size() - p) return false;
        std::copy_n(memory.data() + p, size, static_cast<std::uint8_t*>(dest));
        return true;
    };
    auto reset = [&](std::uint32_t units) {
        std::fill(memory.begin(), memory.end(), 0);
        put(0x100, 0x8200DEC8); put(0x108, 0x200); put(0x10C, 0x300);
        put(0x200, 0x8200DEA0); put(0x30C, 0x400); put(0x310, 0x448);
        // Two records: selected token 2 has a deliberately distinct stored ordinal.
        put(0x438, units); put(0x43C, units < 8 ? 7 : units); put(0x444, 99);
        if (units >= 8) put(0x428, 0x1000);
        const auto start = units < 8 ? 0x428 : 0x1000;
        for (std::uint32_t i = 0; i < units; ++i) memory[start + 2*i+1] = 'A';
        calls = 0;
    };
    auto capture = [&](std::uint32_t token = 2) { CaptureTu1ManifestName(out, 0x100, token, read); };
    for (auto units : {0u, 1u, 7u, 8u, 2048u}) {
        reset(units); const auto before = memory; capture();
        check(out.status == ManifestNameStatus::copied && out.units == units);
        check(out.parent == 0x200 && out.parent_vtable == 0x8200DEA0 &&
              out.list == 0x300 && out.record == 0x424 && out.ordinal == 99);
        check(memory == before);
    }
    reset(3); memory[0x428] = 0xD8; memory[0x429] = 0;
    memory[0x42A] = memory[0x42B] = 0; capture();
    check(out.status == ManifestNameStatus::copied && out.utf16be[0] == 0xD8 &&
          out.utf16be[3] == 0 && out.units == 3);
    for (auto token : {0u, 3u, 0x80000000u, 0xFFFFFFFFu}) {
        reset(1); capture(token); check(out.status == ManifestNameStatus::invalid && !out.units);
    }
    reset(1); capture(1); check(out.status == ManifestNameStatus::copied && out.record == 0x400);
    for (auto field : {0x108u, 0x10Cu, 0x30Cu}) {
        reset(1); put(field, 0); capture(); check(out.status == ManifestNameStatus::invalid);
    }
    reset(1); put(0x310, 0x3FF); capture(); check(out.status == ManifestNameStatus::invalid);
    reset(1); put(0x310, 0x449); capture(); check(out.status == ManifestNameStatus::invalid);
    reset(1); put(0x310, 0x400 + 65537*36); capture(); check(out.status == ManifestNameStatus::invalid);
    reset(2049); capture(); check(out.status == ManifestNameStatus::invalid);
    reset(7); put(0x43C, 6); capture(); check(out.status == ManifestNameStatus::invalid);
    for (auto pointer : {0u, 0xFFFFFFF8u}) {
        reset(8); put(0x428, pointer); capture(); check(out.status == ManifestNameStatus::invalid);
    }
    reset(1); put(0x100, 0x8200DEA0); capture();
    check(out.status == ManifestNameStatus::unsupported_object && calls == 1);
    reset(1); CaptureTu1ManifestName(out, 0xFFFFFFF0, 1, read);
    check(out.status == ManifestNameStatus::unreadable && calls == 0);
    reset(8); capture(); const int read_count = calls;
    for (int fail = 1; fail <= read_count; ++fail) {
        reset(8);
        auto failing = [&](std::uint32_t p, void* dest, std::size_t size) {
            if (calls + 1 == fail) { ++calls; return false; }
            return read(p, dest, size);
        };
        CaptureTu1ManifestName(out, 0x100, 2, failing);
        check(out.status == ManifestNameStatus::unreadable && !out.units && !out.parent);
    }
    // Mutate payload or metadata only when its second read occurs.
    for (auto field : {0x100u, 0x200u, 0x300u, 0x424u, 0x1000u}) {
        reset(8); int seen = 0;
        auto changing = [&](std::uint32_t p, void* dest, std::size_t size) {
            const int target = field == 0x100 ? 3 : 2; // initial vtable peek
            if (p == field && ++seen == target) memory[p] ^= 1;
            return read(p, dest, size);
        };
        CaptureTu1ManifestName(out, 0x100, 2, changing);
        check(out.status == ManifestNameStatus::changed && !out.units && !out.parent);
    }
    std::cout << checks << " manifest name capture checks passed\n";
}
