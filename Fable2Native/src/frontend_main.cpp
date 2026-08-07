// Unified Fable2Native frontend entry point. One executable hosts both the D3D12 and Vulkan
// backends; the backend is selected at launch (restart-applied) from a "--backend d3d12|vulkan"
// command-line token or the persisted preference (%LOCALAPPDATA%\Fable2Native\renderer.txt),
// falling back to D3D12. When the build has no Vulkan support, the selection always resolves to D3D12.
#include "f2/native_frontend_config.h"
#include "f2/native_frontend_entry.h"

#include <windows.h>

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int) {
    const f2::RenderBackend backend = f2::resolve_render_backend(GetCommandLineW());

#ifdef F2NATIVE_HAS_VULKAN
    if (backend == f2::RenderBackend::Vulkan) {
        return f2::run_vulkan_frontend(instance);
    }
#else
    (void)backend;
#endif
    return f2::run_d3d12_frontend(instance);
}
