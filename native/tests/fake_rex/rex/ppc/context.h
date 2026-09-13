#pragma once
#include <cstdint>
struct PPCRegister { union { std::uint64_t u64{}; std::uint32_t u32; }; };
struct PPCContext { PPCRegister r3, r4, r5, r6; std::uint64_t lr{}; };
