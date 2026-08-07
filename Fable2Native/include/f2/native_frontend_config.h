#pragma once

namespace f2 {

// The PC presentation backend. Restart-applied: the choice persists to config and takes effect on
// the next launch (device/swapchain lifetimes stay simple; no live API switch).
enum class RenderBackend {
    D3D12,
    Vulkan,
};

[[nodiscard]] const char* render_backend_name(RenderBackend backend) noexcept;

// Persisted preference at %LOCALAPPDATA%\Fable2Native\renderer.txt (default D3D12).
[[nodiscard]] RenderBackend read_render_backend_preference();
void save_render_backend_preference(RenderBackend backend);

// Resolve the backend to launch: a "--backend d3d12|vulkan" token on the command line overrides the
// persisted preference; otherwise the persisted preference; otherwise D3D12. `command_line` is the
// raw wide command line (GetCommandLineW()); may be null.
[[nodiscard]] RenderBackend resolve_render_backend(const wchar_t* command_line);

}  // namespace f2
