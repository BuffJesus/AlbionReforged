#pragma once
#include "ppc/context.h"
#define REX_HOOK_RAW(name) extern "C" void name(PPCContext& ctx, std::uint8_t* base)
