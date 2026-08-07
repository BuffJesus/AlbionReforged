#pragma once

#include <windows.h>

// Per-backend frontend entry points. Each is implemented in its own translation unit
// (native_frontend_app.cpp = D3D12, native_frontend_vulkan_app.cpp = Vulkan) and selected at
// launch by the unified WinMain (frontend_main.cpp) based on the resolved RenderBackend. The
// two backends are never active in the same process — the choice is restart-applied.
namespace f2 {

int run_d3d12_frontend(HINSTANCE instance);

#ifdef F2NATIVE_HAS_VULKAN
int run_vulkan_frontend(HINSTANCE instance);
#endif

}  // namespace f2
