// Isolated TU1 diagnostic only. Original lookup and lower_bound always execute.
#include "albion/tu1_table_capture.h"
#include "albion/tu1_directory_name.h"
#include "albion/tu1_manifest_name.h"
#include <rex/hook.h>
#include <rex/ppc/context.h>
#include <rex/logging.h>
#include <Windows.h>
#include <atomic>
#include <cstdlib>
#include <memory>
#include <string>

extern "C" {
void __imp__sub_82B453F8(PPCContext&, std::uint8_t*);
void __imp__sub_82B47240(PPCContext&, std::uint8_t*);
void __imp__sub_8237F958(PPCContext&, std::uint8_t*);
void __imp__sub_82C6A400(PPCContext&, std::uint8_t*);
}
namespace {
using albion::compat::Tu1TableCapture;
struct Capture : Tu1TableCapture {
    std::array<albion::compat::Tu1DirectoryName, max_mounts> names{};
    struct Manifest {
        std::uint32_t mount{}, hash{}, token{};
        albion::compat::Tu1ManifestName name;
    };
    std::array<Manifest, 32> manifests{};
    std::uint32_t manifest_total{}, manifest_count{};
};
thread_local Capture* pending = nullptr;
thread_local bool indexed_seen = false;
std::atomic_flag attempted = ATOMIC_FLAG_INIT;
std::atomic<unsigned> attempts{0};
std::atomic<std::uint32_t> installed_manager{0};
bool read_guest(std::uint8_t* base, std::uint32_t address, void* dest, std::size_t size) {
    if (!base || !address || size > (std::uint64_t{1} << 32) - address) return false;
    SIZE_T copied = 0;
    return ReadProcessMemory(GetCurrentProcess(), base + address, dest, size, &copied) && copied == size;
}
const std::string& output_path() {
    static const std::string path = [] {
        const DWORD size = GetEnvironmentVariableA("FABLE2_TU1_TABLE_CAPTURE", nullptr, 0);
        if (!size || size > 32768) return std::string();
        std::string path(size, '\0');
        const DWORD copied = GetEnvironmentVariableA("FABLE2_TU1_TABLE_CAPTURE", path.data(), size);
        if (!copied || copied >= size) return std::string();
        path.resize(copied);
        return path;
    }();
    return path;
}
struct PendingScope {
    explicit PendingScope(Capture* p) { pending = p; }
    ~PendingScope() { pending = nullptr; }
};
void write_manifests(const Capture& c) {
    if (!c.manifest_count) return;
    const auto path = output_path() + ".manifest";
    HANDLE file = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        REXLOG_INFO("[Tu1ManifestCapture] Cannot create sidecar: {}", GetLastError());
        return;
    }
    const auto write = [&](const void* data, DWORD bytes) {
        DWORD done = 0;
        return WriteFile(file, data, bytes, &done, nullptr) && done == bytes;
    };
    const auto word = [&](std::uint32_t value) {
        const std::uint8_t bytes[]{std::uint8_t(value >> 24), std::uint8_t(value >> 16),
                                  std::uint8_t(value >> 8), std::uint8_t(value)};
        return write(bytes, 4);
    };
    bool ok = write("TU1MAN01", 8) && word(c.manager) && word(c.query) &&
              word(c.manifest_total) && word(c.manifest_count);
    for (std::size_t i = 0; ok && i < c.manifest_count; ++i) {
        const auto& m = c.manifests[i]; const auto& n = m.name;
        ok = word(m.mount) && word(m.hash) && word(m.token) && word(std::uint32_t(n.status)) &&
             word(n.parent) && word(n.parent_vtable) && word(n.list) && word(n.record) &&
             word(n.ordinal) && word(n.units) && write(n.utf16be.data(), n.units * 2);
    }
    CloseHandle(file);
    REXLOG_INFO("[Tu1ManifestCapture] file_complete={} captured={} eligible={}",
                ok, c.manifest_count, c.manifest_total);
}
void write_capture(const Capture& c) {
    if (!c.complete) {
        REXLOG_INFO("[Tu1TableCapture] No eligible bounded indexed table captured; no file written");
        return;
    }
    // Single exclusive-create file; never overwrite previous evidence.
    HANDLE file = CreateFileA(output_path().c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        REXLOG_INFO("[Tu1TableCapture] Cannot create capture file: {}", GetLastError());
        return;
    }
    std::array<std::uint8_t, 24> header{'T','U','1','T','A','B','0','2'};
    const std::uint32_t values[]{c.manager, c.query, c.mount_bytes, c.record_bytes};
    for (std::size_t i = 0; i < 4; ++i)
        for (std::size_t b = 0; b < 4; ++b)
            header[8 + i * 4 + b] = std::uint8_t(values[i] >> (24 - b * 8));
    const auto write = [&](const void* p, DWORD n) {
        DWORD done = 0;
        return WriteFile(file, p, n, &done, nullptr) && done == n;
    };
    bool ok = write(header.data(), DWORD(header.size())) &&
        write(c.mounts.data(), c.mount_bytes) && write(c.records.data(), c.record_bytes);
    for (std::size_t i = 0; ok && i < c.mount_bytes / 36; ++i) {
        const auto& name = c.names[i];
        std::array<std::uint8_t, 12> info{};
        const std::uint32_t words[]{std::uint32_t(name.status), name.vtable, name.units};
        for (std::size_t w = 0; w < 3; ++w)
            for (std::size_t b = 0; b < 4; ++b)
                info[4*w+b] = std::uint8_t(words[w] >> (24-8*b));
        ok = write(info.data(), DWORD(info.size())) && write(name.utf16be.data(), name.units * 2);
    }
    CloseHandle(file);
    if (ok) write_manifests(c);
    REXLOG_INFO("[Tu1TableCapture] file_complete={} manager={:08X} query={:08X} mounts={} records={}",
                ok, c.manager, c.query, c.mount_bytes / 36, c.record_bytes / 12);
}
void observe_lookup(PPCContext& ctx, std::uint8_t* base,
                    void (*original)(PPCContext&, std::uint8_t*)) {
    const auto target = installed_manager.load();
    if (output_path().empty() || !target || ctx.r4.u32 != target ||
        ctx.r6.u32 == 0 || pending || attempted.test_and_set()) {
        original(ctx, base);
        return;
    }
    // Wrapping the derived lookup allocates before either of its manager locks.
    auto capture = std::unique_ptr<Capture>(new (std::nothrow) Capture);
    if (!capture) {
        original(ctx, base);
        return;
    }
    // r3 is the output smart pointer; r4 is the manager (saved as r29).
    capture->manager = ctx.r4.u32;
    indexed_seen = false;
    {
        PendingScope scope(capture.get());
        original(ctx, base);
    }
    // The wrapped lookup has returned and released its manager lock.
    if (indexed_seen || ++attempts >= 64) write_capture(*capture);
    else attempted.clear(); // qualified-name delegation or derived-cache hit
}
}

REX_HOOK_RAW(sub_82B453F8) {
    observe_lookup(ctx, base, __imp__sub_82B453F8);
}

REX_HOOK_RAW(sub_82C6A400) {
    observe_lookup(ctx, base, __imp__sub_82C6A400);
}

REX_HOOK_RAW(sub_82B47240) {
    if (pending && !pending->complete && ctx.lr == 0x82B45574 &&
        (ctx.r4.u64 >> 32) == std::uint64_t(pending->manager) + 0x34 &&
        (ctx.r5.u64 >> 32) == std::uint64_t(pending->manager) + 0x34) {
        indexed_seen = true;
        auto read = [base](std::uint32_t address, void* dest, std::size_t size) {
            return read_guest(base, address, dest, size);
        };
        if (albion::compat::CaptureTu1Table(*pending, ctx.lr, pending->manager,
                                           ctx.r4.u64, ctx.r5.u64, ctx.r6.u32, read)) {
            for (std::size_t i = 0; i < pending->mount_bytes / 36; ++i)
                albion::compat::CaptureTu1DirectoryName(pending->names[i],
                    albion::compat::CaptureBe32(pending->mounts.data() + i * 36), read);
            for (std::size_t i = 0; i < pending->record_bytes / 12; ++i) {
                const auto* row = pending->records.data() + i * 12;
                const auto mount = albion::compat::CaptureBe32(row + 4);
                if (pending->names[mount].vtable != 0x8200DEC8) continue;
                ++pending->manifest_total;
                if (pending->manifest_count == pending->manifests.size()) continue;
                auto& m = pending->manifests[pending->manifest_count++];
                m.mount = mount; m.hash = albion::compat::CaptureBe32(row);
                m.token = albion::compat::CaptureBe32(row + 8);
                albion::compat::CaptureTu1ManifestName(m.name,
                    albion::compat::CaptureBe32(pending->mounts.data() + mount * 36), m.token, read);
            }
        }
    }
    __imp__sub_82B47240(ctx, base);
}

REX_HOOK_RAW(sub_8237F958) {
    // r3 is the source-owner object; r4 is required base source, r5 optional update.
    // Original code stores/uses its manager at owner+0x44. Never arm during install.
    const auto owner = ctx.r3.u32;
    const bool eligible = !output_path().empty() && ctx.r4.u32 != 0 &&
        owner != 0 && owner <= UINT32_MAX - 0x47;
    __imp__sub_8237F958(ctx, base);
    if (!eligible) return;
    std::array<std::uint8_t, 4> word{};
    if (!read_guest(base, owner + 0x44, word.data(), word.size())) return;
    const auto manager = albion::compat::CaptureBe32(word.data());
    if (!read_guest(base, manager, word.data(), word.size())) return;
    const auto vtable = albion::compat::CaptureBe32(word.data());
    if (vtable != 0x820F9988 && vtable != 0x8200DE40) return;
    std::uint32_t empty = 0;
    if (installed_manager.compare_exchange_strong(empty, manager)) {
        REXLOG_INFO("[Tu1TableCapture] Armed after source-install return: manager={:08X}", manager);
    }
}
