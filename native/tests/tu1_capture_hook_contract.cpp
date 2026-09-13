// Runs the real Windows adapter with a deliberately small fake PPC ABI. This
// tests host sequencing and forwarding, not real ReX register layout/dispatch.
#include <rex/hook.h>
#include <Windows.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
extern "C" void sub_82B453F8(PPCContext&, std::uint8_t*);
extern "C" void sub_82B47240(PPCContext&, std::uint8_t*);
extern "C" void sub_8237F958(PPCContext&, std::uint8_t*);
extern "C" void sub_82C6A400(PPCContext&, std::uint8_t*);
namespace {
std::filesystem::path output;
std::string mode;
int lookups{}, leaves{}, installs{}, derived{};
bool preexisting{};
void check(bool ok) { if (!ok) std::exit(2); }
void put(std::uint8_t* b, std::uint32_t p, std::uint32_t v) {
    for (int i=0;i<4;++i) b[p+i] = std::uint8_t(v >> (24-i*8));
}
}
extern "C" void __imp__sub_82B47240(PPCContext& ctx, std::uint8_t*) {
    ++leaves;
    check(!std::filesystem::exists(output.string() + ".manifest"));
    check(std::filesystem::exists(output) == preexisting); // no I/O inside lookup
    ctx.r3.u64 = 0xabcdef0123456789ull;
}
extern "C" void __imp__sub_82B453F8(PPCContext& ctx, std::uint8_t* base) {
    ++lookups;
    ctx.lr = mode == "wrong_caller" ? 0x82B46150 : 0x82B45574;
    ctx.r4.u64 = (0x134ull << 32) | 0x400;
    ctx.r5.u64 = ctx.r4.u64 + 12;
    ctx.r6.u64 = mode == "unreadable" ? 0x1000 : 0x800;
    sub_82B47240(ctx, base);
    check(ctx.r3.u64 == 0xabcdef0123456789ull);
    check(std::filesystem::exists(output) == preexisting);
    ctx.r3.u64 = 0x123456789abcdef0ull;
    ctx.lr = 0x1122334455667788ull;
}
extern "C" void __imp__sub_8237F958(PPCContext& ctx, std::uint8_t* base) {
    ++installs;
    // An indexed lookup during installation must not consume the capture.
    ctx.r3.u64 = 0x900; ctx.r4.u64 = 0x100; ctx.r6.u64 = 1;
    sub_82B453F8(ctx, base);
    check(std::filesystem::exists(output) == preexisting);
    ctx.r3.u64 = 0x8765432101234567ull;
    ctx.lr = 0x1234567887654321ull;
}
extern "C" void __imp__sub_82C6A400(PPCContext& ctx, std::uint8_t* base) {
    ++derived;
    if (mode == "cache_then_miss" && derived == 1) {
        ctx.r3.u64 = 0x123456789abcdef0ull; ctx.lr = 0x1122334455667788ull;
        return;
    }
    sub_82B453F8(ctx, base);
    // Represents the derived manager lock, still held after the base call.
    check(!std::filesystem::exists(output.string() + ".manifest"));
    check(std::filesystem::exists(output) == preexisting);
    check(ctx.r3.u64 == 0x123456789abcdef0ull && ctx.lr == 0x1122334455667788ull);
}
int main(int argc, char** argv) {
    check(argc == 3); mode = argv[1]; output = argv[2];
    preexisting = mode == "existing";
    if (preexisting) { std::ofstream f(output); f << "keep"; }
    check(SetEnvironmentVariableA("FABLE2_TU1_TABLE_CAPTURE",
          mode == "disabled" ? nullptr : output.string().c_str()) != 0);
    std::vector<std::uint8_t> memory(4096);
    put(memory.data(),0x100,mode == "unknown_manager" ? 0x82000000 : 0x8200de40);
    put(memory.data(),0xa44,0x100);
    put(memory.data(),0x128,0x200); put(memory.data(),0x12c,0x224);
    put(memory.data(),0x138,0x400); put(memory.data(),0x13c,0x40c);
    put(memory.data(),0x400,7); put(memory.data(),0x408,9); put(memory.data(),0x800,7);
    put(memory.data(),0x200,0x900); put(memory.data(),0x900,0x8200dea0);
    put(memory.data(),0x91c,5); put(memory.data(),0x920,7);
    const char* name = "a.bnk";
    for (int i = 0; i < 5; ++i) memory[0x90d + i*2] = std::uint8_t(name[i]);
    if (mode == "manifest") {
        put(memory.data(),0x900,0x8200DEC8); put(memory.data(),0x908,0xD00);
        put(memory.data(),0x90C,0xB00); put(memory.data(),0xD00,0x8200DEA0);
        put(memory.data(),0xB0C,0xC00); put(memory.data(),0xB10,0xC24);
        put(memory.data(),0xC14,3); put(memory.data(),0xC18,7); put(memory.data(),0xC20,1);
        memory[0xC05]='X'; memory[0xC07]='/'; memory[0xC09]='N';
        put(memory.data(),0x408,1);
    }
    auto original = memory;
    PPCContext ctx{}; ctx.r3.u64 = 0x900; ctx.r4.u64 = 0x100; ctx.r6.u64 = 1;
    auto* guest = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 8192,
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    check(guest != nullptr);
    std::copy(memory.begin(), memory.end(), guest);
    DWORD previous = 0;
    check(VirtualProtect(guest + 4096, 4096, PAGE_NOACCESS, &previous) != 0);
    // Before installation, even an eligible-looking lookup produces no capture.
    sub_82B453F8(ctx, guest);
    check(std::filesystem::exists(output) == preexisting);
    ctx.r3.u64 = mode == "bad_owner" ? 0x1000 : 0xa00;
    ctx.r4.u64 = mode == "null_source" ? 0 : 0xb00;
    sub_8237F958(ctx, guest);
    check(installs == 1 && ctx.r3.u64 == 0x8765432101234567ull && ctx.lr == 0x1234567887654321ull);
    ctx.r3.u64 = 0x900; ctx.r4.u64 = mode == "other_manager" ? 0x200 : 0x100; ctx.r6.u64 = 1;
    if (mode == "base_direct") sub_82B453F8(ctx, guest);
    else sub_82C6A400(ctx, guest);
    if (mode == "cache_then_miss") {
        check(!std::filesystem::exists(output) && lookups == 2 && leaves == 2);
        check(ctx.r3.u64 == 0x123456789abcdef0ull && ctx.lr == 0x1122334455667788ull);
        ctx.r3.u64 = 0x900; ctx.r4.u64 = 0x100; ctx.r6.u64 = 1;
        sub_82C6A400(ctx, guest);
    }
    check(ctx.r3.u64 == 0x123456789abcdef0ull && ctx.lr == 0x1122334455667788ull);
    check(lookups == 3 && leaves == 3 && std::equal(original.begin(), original.end(), guest));
    check(derived == (mode == "base_direct" ? 0 : mode == "cache_then_miss" ? 2 : 1));
    bool expected = mode == "success" || mode == "manifest" || mode == "base_direct" || mode == "cache_then_miss" || preexisting;
    check(std::filesystem::exists(output) == expected);
    if (expected) check(std::filesystem::file_size(output) == (preexisting ? 4 : mode == "manifest" ? 84 : 94));
    check(std::filesystem::exists(output.string() + ".manifest") == (mode == "manifest"));
    if (mode == "success") {
        preexisting = true;
        // A later lookup forwards again, without replacing the one-shot snapshot.
        ctx.r3.u64 = 0x900; ctx.r4.u64 = 0x100; ctx.r6.u64 = 1;
        sub_82B453F8(ctx, guest);
        check(lookups == 4 && leaves == 4 && std::filesystem::file_size(output) == 94);
        check(ctx.r3.u64 == 0x123456789abcdef0ull && ctx.lr == 0x1122334455667788ull);
    }
    VirtualFree(guest, 0, MEM_RELEASE);
    std::cout << "adapter forwarding and file sequencing passed: " << mode << '\n';
}
