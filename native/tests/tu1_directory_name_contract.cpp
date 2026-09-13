#include "albion/tu1_directory_name.h"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <vector>

int main() {
    using namespace albion::compat;
    Tu1DirectoryName out;
    std::vector<std::uint8_t> memory(4096);
    auto put = [&](std::size_t p, std::uint32_t v) {
        for (int i = 0; i < 4; ++i) memory[p+i] = std::uint8_t(v >> (24-8*i));
    };
    auto reset = [&](std::size_t units, bool heap) {
        std::fill(memory.begin(), memory.end(), 0);
        put(0x100, 0x8200dea0); put(0x11c, static_cast<std::uint32_t>(units));
        put(0x120, heap ? static_cast<std::uint32_t>(std::max(units, std::size_t(8))) : 7);
        if (heap) put(0x10c, 0x400);
        for (std::size_t i = 0; i < units; ++i) memory[(heap ? 0x400 : 0x10c)+2*i+1] = 'a';
    };
    int calls = 0, checks = 0;
    auto read = [&](std::uint32_t p, void* d, std::size_t n) {
        ++calls;
        if (p > memory.size() || n > memory.size()-p) return false;
        std::copy_n(memory.data()+p, n, static_cast<std::uint8_t*>(d)); return true;
    };
    auto check = [&](bool condition) {
        if (++checks && !condition) { std::cerr << "failed " << checks << '\n'; std::exit(1); }
    };
    auto capture = [&] { calls = 0; CaptureTu1DirectoryName(out, 0x100, read); };
    for (const auto units : {0u, 1u, 7u, 8u, 512u}) {
        reset(units, units >= 8); auto original = memory; capture();
        check(out.status == DirectoryNameStatus::copied && out.units == units);
        check(memory == original);
    }
    reset(513, true); capture(); check(out.status == DirectoryNameStatus::invalid && !out.units);
    reset(7, false); put(0x120, 6); capture(); check(out.status == DirectoryNameStatus::invalid);
    reset(8, true); put(0x10c, 0xfffffff0); capture(); check(out.status == DirectoryNameStatus::invalid);
    reset(8, true); put(0x10c, 0); capture(); check(out.status == DirectoryNameStatus::invalid);
    reset(1, false); memory[0x10f] = 'x'; capture(); check(out.status == DirectoryNameStatus::invalid);
    reset(1, false); memory[0x10d] = 0; capture(); check(out.status == DirectoryNameStatus::invalid);
    reset(1, false); memory[0x10c] = 0xdc; memory[0x10d] = 0; capture();
    check(out.status == DirectoryNameStatus::invalid);
    reset(1, false); memory[0x10c] = 0xd8; memory[0x10d] = 0; capture();
    check(out.status == DirectoryNameStatus::invalid);
    reset(2, false); memory[0x10c] = 0xd8; memory[0x10d] = 0; capture();
    check(out.status == DirectoryNameStatus::invalid);
    memory[0x10e] = 0xdc; memory[0x10f] = 0; capture();
    check(out.status == DirectoryNameStatus::copied && out.units == 2);
    reset(1, false); put(0x100, 0x82000000); capture();
    check(out.status == DirectoryNameStatus::unsupported_object && calls == 1 && !out.units);
    CaptureTu1DirectoryName(out, 0xfffffff0, read); check(out.status == DirectoryNameStatus::unreadable);
    for (int fail = 1; fail <= 4; ++fail) {
        reset(1, false); calls = 0;
        auto failure = [&](std::uint32_t p, void* d, std::size_t n) {
            if (calls + 1 == fail) { ++calls; return false; } return read(p,d,n);
        };
        CaptureTu1DirectoryName(out, 0x100, failure);
        check(out.status == DirectoryNameStatus::unreadable && !out.units);
    }
    reset(1, false); calls = 0;
    auto changed = [&](std::uint32_t p, void* d, std::size_t n) {
        if (calls == 3) put(0x120, 8);
        return read(p,d,n);
    };
    CaptureTu1DirectoryName(out, 0x100, changed);
    check(out.status == DirectoryNameStatus::unreadable && !out.units);
    std::cout << checks << " directory-name checks passed\n";
}
