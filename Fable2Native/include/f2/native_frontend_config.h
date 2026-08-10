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

// The persisted Options-page settings (everything except the render backend, which has its own file).
// Defaults here match FrontendController::reset(); read_options() returns these when no file exists.
struct FrontendOptions {
    bool subtitles = true;
    bool tutorials = true;
    bool multiplayer_orbs = true;
    bool auto_joinable = true;
    bool invert_aim = false;
    int breadcrumb_size = 1;
    int gamma_percent = 50;
    int resolution_index = 1;
    int anti_aliasing_index = 2;
    bool fps_display = false;
    int sounds_volume = 80;
    int music_volume = 80;
    int voice_volume = 80;
    int speaker_mode = 0;
};

// Persisted at %LOCALAPPDATA%\Fable2Native\options.ini so all Options settings stick across launches.
[[nodiscard]] FrontendOptions read_options();
void save_options(const FrontendOptions& options);

// Resolve the backend to launch: a "--backend d3d12|vulkan" token on the command line overrides the
// persisted preference; otherwise the persisted preference; otherwise D3D12. `command_line` is the
// raw wide command line (GetCommandLineW()); may be null.
[[nodiscard]] RenderBackend resolve_render_backend(const wchar_t* command_line);

}  // namespace f2
