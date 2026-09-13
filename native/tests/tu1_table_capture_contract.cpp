#include "albion/tu1_table_capture.h"
#include <algorithm>
#include <iostream>
#include <memory>
#include <vector>

int main() {
    using namespace albion::compat;
    auto out = std::make_unique<Tu1TableCapture>();
    std::vector<std::uint8_t> memory(4096);
    auto put = [&](std::size_t p, std::uint32_t v) {
        for (int b = 0; b < 4; ++b) memory[p + b] = std::uint8_t(v >> (24 - 8*b));
    };
    constexpr std::uint32_t manager = 0x100, mounts = 0x200, records = 0x400, query = 0x800;
    const auto begin = (std::uint64_t(manager + 0x34) << 32) | records;
    const auto end = begin + 36;
    auto reset = [&] {
        std::fill(memory.begin(), memory.end(), 0);
        put(manager + 0x28, mounts); put(manager + 0x2c, mounts + 72);
        put(manager + 0x38, records); put(manager + 0x3c, records + 36);
        put(records, 7); put(records + 4, 1); put(records + 8, 9);
        put(records + 12, 7); put(records + 16, 0);
        put(records + 24, 0xffffffff); put(records + 28, 1);
        put(query, 7);
    };
    auto read = [&](std::uint32_t p, void* dst, std::size_t n) {
        if (p > memory.size() || n > memory.size() - p) return false;
        std::copy_n(memory.data() + p, n, static_cast<std::uint8_t*>(dst)); return true;
    };
    int checks = 0;
    auto check = [&](bool condition) {
        ++checks; if (!condition) { std::cerr << "failed check " << checks << '\n'; std::exit(1); }
    };
    auto capture = [&] { return CaptureTu1Table(*out, 0x82B45574, manager, begin, end, query, read); };
    reset(); auto before = memory;
    check(capture()); check(out->complete && out->record_bytes == 36 && out->mount_bytes == 72);
    check(out->query == 7 && CaptureBe32(out->records.data() + 8) == 9); check(before == memory);
    check(!CaptureTu1Table(*out, 0x82B46150, manager, begin, end, query, read)); check(!out->complete);
    reset(); put(records + 28, 2); check(!capture());
    reset(); put(records + 12, 6); check(!capture());
    reset(); put(manager + 0x2c, mounts + 1); check(!capture());
    reset(); put(manager + 0x2c, mounts - 36); check(!capture());
    reset(); put(manager + 0x2c, mounts + 36 * 257); check(!capture());
    reset(); put(manager + 0x3c, records + 12 * 65537);
    check(!CaptureTu1Table(*out, 0x82B45574, manager, begin, begin + 12 * 65537, query, read));
    reset(); check(!CaptureTu1Table(*out, 0x82B45574, manager, begin ^ (1ull << 32), end, query, read));
    check(!CaptureTu1Table(*out, 0x82B45574, 0xfffffff0, begin, end, query, read));
    check(!CaptureTu1Table(*out, 0x82B45574, manager, begin, end, 0xfffffffe, read));
    for (int fail = 1; fail <= 5; ++fail) {
        reset(); int calls = 0;
        auto failed = [&](std::uint32_t p, void* d, std::size_t n) { return ++calls != fail && read(p,d,n); };
        check(!CaptureTu1Table(*out, 0x82B45574, manager, begin, end, query, failed));
    }
    reset(); int calls = 0;
    auto changed = [&](std::uint32_t p, void* d, std::size_t n) {
        if (++calls == 5) put(manager + 0x3c, records + 24);
        return read(p,d,n);
    };
    check(!CaptureTu1Table(*out, 0x82B45574, manager, begin, end, query, changed));
    reset(); check(capture()); std::fill(memory.begin(), memory.end(), 0);
    check(CaptureBe32(out->records.data()) == 7); // owns the copy
    std::cout << checks << " capture checks passed\n";
}
