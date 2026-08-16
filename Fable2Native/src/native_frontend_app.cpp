#include "f2/native_game.h"
#include "f2/native_audio.h"
#include "f2/native_frontend_entry.h"
#include "f2/native_install.h"
#include "f2/native_input.h"
#include "f2/native_font.h"
#include "f2/frontend_scene_builder.h"
#include "f2/native_ui.h"
#include "f2/native_ui_renderer.h"
#include "f2/native_video_decoder.h"
#include "f2/render/texture_registry.h"
#include "f2/render/ui_draw_list.h"
#include "f2/native_cloud_renderer.h"
#include "f2/native_sky_billboard_renderer.h"
#include "f2/native_scene_color.h"
#include "f2/native_sky_stars_renderer.h"
#include "f2/native_tonemap_renderer.h"
#include "f2/native_world_renderer.h"


#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <cstdlib>
#include <dxgi1_6.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

constexpr UINT kFrameCount = 2;
constexpr DXGI_FORMAT kBackBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr UINT kShadowSize = 2048;  // sun shadow-map resolution (square)
constexpr DXGI_FORMAT kDepthFormat = DXGI_FORMAT_D32_FLOAT;
constexpr UINT kUiTextureCount = 36;

// Recovered from the retail ExpandableMenuFormatter LuaQ bytecode.  The
// original formatter assigns a menu item to:
//
//     DisplayIndex + HIGHLIGHT_INDEX - CurrentHighlightIdx
//
// where HIGHLIGHT_INDEX is 4.  The middle slots are intentionally straight;
// the outer slots are the curved portion of the rail and are also faded and
// scaled by the opacity/scale table below.
struct RetailMenuSlot {
    float x;
    float y;
    float opacity;
    float scale;
};

constexpr std::array<RetailMenuSlot, 12> kRetailMenuSlots = {{
    {0.0f, 85.0f, 0.0f, 0.65f},
    {22.0f, 69.0f, 50.0f, 0.75f},
    {42.0f, 15.0f, 75.0f, 0.85f},
    {56.0f, -45.0f, 100.0f, 1.0f},
    {56.0f, -106.0f, 100.0f, 1.0f},
    {56.0f, -164.0f, 100.0f, 1.0f},
    {56.0f, -222.0f, 100.0f, 1.0f},
    {56.0f, -280.0f, 100.0f, 1.0f},
    {50.0f, -338.0f, 100.0f, 1.0f},
    {42.0f, -396.0f, 75.0f, 0.85f},
    {22.0f, -454.0f, 50.0f, 0.75f},
    {0.0f, -475.0f, 0.0f, 0.65f},
}};

constexpr int kRetailHighlightSlot = 4;
constexpr float kRetailCenterSlotX = 56.0f;
constexpr float kRetailCenterSlotY = -45.0f;

const RetailMenuSlot* retail_menu_slot(std::size_t display_index,
                                       std::size_t selected_index) {
    const int slot = kRetailHighlightSlot + static_cast<int>(display_index) -
                     static_cast<int>(selected_index);
    if (slot < 1 || slot > static_cast<int>(kRetailMenuSlots.size())) return nullptr;
    return &kRetailMenuSlots[static_cast<std::size_t>(slot - 1)];
}

struct FrameContext {
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12Resource> render_target;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
};

struct UiGpuTexture {
    ComPtr<ID3D12Resource> texture;
    ComPtr<ID3D12Resource> upload;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT row_count = 0;
    UINT64 row_size = 0;
    UINT64 upload_size = 0;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu{};
    UINT descriptor_index = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    bool shader_read = false;
};

std::optional<std::filesystem::path> show_source_picker(HWND owner, bool pick_iso) {
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dialog)))) {
        return std::nullopt;
    }

    DWORD options = 0;
    dialog->GetOptions(&options);
    options |= FOS_FORCEFILESYSTEM;
    if (pick_iso) {
        options |= FOS_FILEMUSTEXIST;
        const COMDLG_FILTERSPEC filter{L"Fable II ISO", L"*.iso"};
        dialog->SetFileTypes(1, &filter);
        dialog->SetTitle(L"Select your legally obtained Fable II ISO");
    } else {
        options |= FOS_PICKFOLDERS;
        dialog->SetTitle(L"Select the extracted Fable II game directory");
    }
    dialog->SetOptions(options);
    if (FAILED(dialog->Show(owner))) return std::nullopt;

    ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(&item))) return std::nullopt;
    PWSTR raw_path = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw_path))) return std::nullopt;
    std::filesystem::path path(raw_path);
    CoTaskMemFree(raw_path);
    return path;
}

std::optional<std::filesystem::path> command_line_path(std::wstring_view option) {
    int argument_count = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argument_count);
    if (!arguments) return std::nullopt;

    std::optional<std::filesystem::path> result;
    for (int index = 0; index + 1 < argument_count; ++index) {
        if (option == arguments[index]) {
            result = std::filesystem::path(arguments[index + 1]);
            break;
        }
    }
    LocalFree(arguments);
    return result;
}

bool command_line_flag(std::wstring_view option) {
    int argument_count = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argument_count);
    if (!arguments) return false;
    bool found = false;
    for (int index = 0; index < argument_count; ++index) {
        if (option == arguments[index]) {
            found = true;
            break;
        }
    }
    LocalFree(arguments);
    return found;
}

std::filesystem::path source_config_path() {
    wchar_t local_app_data[MAX_PATH]{};
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", local_app_data,
                                                  static_cast<DWORD>(std::size(local_app_data)));
    if (length == 0 || length >= std::size(local_app_data)) return {};
    return std::filesystem::path(local_app_data) / "Fable2Native" / "source.txt";
}

std::optional<std::filesystem::path> read_configured_source() {
    const auto config_path = source_config_path();
    if (config_path.empty()) return std::nullopt;
    std::wifstream input(config_path);
    std::wstring line;
    if (!input || !std::getline(input, line) || line.empty()) return std::nullopt;
    return std::filesystem::path(line);
}

void save_configured_source(const std::filesystem::path& path) {
    const auto config_path = source_config_path();
    if (config_path.empty()) return;
    std::error_code error;
    std::filesystem::create_directories(config_path.parent_path(), error);
    if (error) return;
    std::wofstream output(config_path);
    if (output) output << path.wstring() << L"\n";
}

class FrontendApp {
public:
    bool initialise(HINSTANCE instance) {
        const HRESULT com_result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        const bool com_initialized = SUCCEEDED(com_result);
        if (FAILED(com_result) && com_result != RPC_E_CHANGED_MODE) return false;
        std::string video_error;
        if (!f2::start_native_video_runtime(video_error)) {
            MessageBoxA(nullptr, video_error.c_str(), "Fable II Native - video runtime failed",
                        MB_OK | MB_ICONERROR);
            return false;
        }
        video_runtime_started_ = true;

        WNDCLASSA window_class{};
        window_class.hInstance = instance;
        window_class.lpfnWndProc = &FrontendApp::window_proc;
        window_class.lpszClassName = "Fable2NativeFrontend";
        window_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
        if (!RegisterClassA(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            return false;
        }

        window_ = CreateWindowA(window_class.lpszClassName, "Fable II Native",
                                WS_POPUP, CW_USEDEFAULT, CW_USEDEFAULT,
                                width_, height_, nullptr, nullptr, instance, this);
        if (!window_ || !select_game_source()) {
            if (window_) DestroyWindow(window_);
            if (com_initialized) CoUninitialize();
            return false;
        }
        if (const auto scene = command_line_path(L"--scene")) {
            std::string error;
            if (!game_.load_scene(*scene, error)) {
                MessageBoxA(window_, error.c_str(), "Fable II Native - scene load failed",
                            MB_OK | MB_ICONERROR);
                if (window_) DestroyWindow(window_);
                if (com_initialized) CoUninitialize();
                return false;
            }
        }
        if (const auto texture = command_line_path(L"--texture")) {
            if (game_.scene.materials.empty()) game_.scene.materials.push_back({"cli_texture"});
            game_.scene.materials[0].albedo = texture->string();
            for (auto& mesh : game_.scene.meshes) mesh.material = 0;
        }
        video_root_ = command_line_path(L"--video-root").value_or(std::filesystem::path{});
        if (const auto requested_ui_root = command_line_path(L"--ui-root")) {
            ui_root_ = *requested_ui_root;
        } else if (source_) {
            ui_root_ = source_->data_root / "art" / "gui" / "native_ui";
        }
        std::string ui_error;
        if (!ui_assets_.load(ui_root_, ui_error) && !ui_error.empty()) {
            MessageBoxA(window_, ui_error.c_str(), "Fable II Native - UI asset warning",
                        MB_OK | MB_ICONWARNING);
        }
        std::string audio_error;
        // Cooked GUI audio lives in the native_audio subdir of the cooked UI package
        // (produced by tools/cook_gui_audio.py from the user's data/audio/gui.bnk).
        audio_.initialise(command_line_path(L"--audio-root").value_or(ui_root_ / "native_audio"),
                          audio_error);
        input_.load_bindings(source_config_path().parent_path() / "bindings.ini");
        if (command_line_flag(L"--controller-prompts")) input_.force_controller_prompts(true);
        if (command_line_flag(L"--skip-intro")) {
            game_.frontend.dispatch(f2::FrontendAction::Skip);
            if (game_.frontend.state() == f2::FrontendState::IntroVideo) {
                game_.frontend.dispatch(f2::FrontendAction::Skip);
            }
            if (command_line_flag(L"--start-menu")) {
                game_.frontend.dispatch(f2::FrontendAction::Accept);
            }
        }
        if (command_line_flag(L"--show-fps")) game_.frontend.set_fps_display_enabled(true);
        if (command_line_flag(L"--gameplay")) gameplay_mode_ = true;
        if (command_line_flag(L"--start-world")) {
            game_.frontend.debug_jump_to(f2::FrontendState::World);
        }
        if (com_initialized) CoUninitialize();
        if (!create_device()) return false;
        std::string renderer_error;
        auto texture_cpu_handle = descriptor_heap_->GetCPUDescriptorHandleForHeapStart();
        auto texture_gpu_handle = descriptor_heap_->GetGPUDescriptorHandleForHeapStart();
        const auto descriptor_stride = device_->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        texture_cpu_handle.ptr += descriptor_stride;
        texture_gpu_handle.ptr += descriptor_stride;
        if (!world_renderer_.initialise(
                device_.Get(), queue_.Get(), game_.scene,
                source_ ? source_->data_root : std::filesystem::path{},
                texture_cpu_handle, texture_gpu_handle, renderer_error)) {
            MessageBoxA(window_, renderer_error.c_str(), "Fable II Native - renderer failed",
                        MB_OK | MB_ICONERROR);
            return false;
        }
        world_renderer_.set_scene_depth_copy(depth_target_.Get(), depth_copy_.Get(),
                                             depth_gpu_handle_);
        if (shadow_map_) {
            world_renderer_.set_shadow_map(shadow_dsv_, shadow_srv_gpu_, kShadowSize);
        }
        // Procedural sky pass (self-contained: own root sig/PSO/LUT/descriptor heap). A
        // failure here is non-fatal — the World branch falls back to the flat sky_color clear.
        std::string sky_error;
        if (!sky_renderer_.initialise(device_.Get(), queue_.Get(), sky_error)) {
            OutputDebugStringA(("Fable2Native: sky renderer disabled: " + sky_error + "\n").c_str());
        }
        // Cloud-layer pass (self-contained like the sky). Non-fatal on failure: the sky just
        // renders without its scrolling cloud layers.
        std::string cloud_error;
        if (!cloud_renderer_.initialise(device_.Get(), queue_.Get(), cloud_error)) {
            OutputDebugStringA(
                ("Fable2Native: cloud renderer disabled: " + cloud_error + "\n").c_str());
        }
        std::string billboard_error;
        if (!billboard_renderer_.initialise(device_.Get(), queue_.Get(), billboard_error)) {
            OutputDebugStringA(
                ("Fable2Native: billboard renderer disabled: " + billboard_error + "\n").c_str());
        }
        std::string stars_error;
        if (!stars_renderer_.initialise(device_.Get(), stars_error)) {
            OutputDebugStringA(
                ("Fable2Native: stars renderer disabled: " + stars_error + "\n").c_str());
        }
        // HDR -> LDR compositor (tonemap/exposure). Non-fatal: if it fails, the World path falls
        // back to rendering directly to the LDR back buffer (see render()).
        std::string tonemap_error;
        if (!tonemap_renderer_.initialise(device_.Get(), kBackBufferFormat, tonemap_error)) {
            OutputDebugStringA(
                ("Fable2Native: tonemap compositor disabled: " + tonemap_error + "\n").c_str());
        } else if (hdr_scene_) {
            tonemap_renderer_.ensure_targets(device_.Get(), hdr_scene_.Get(), width_, height_);
        }
        read_hdr_options();
        std::string ui_renderer_error;
        if (!native_ui_renderer_.initialise(device_.Get(), 1, ui_renderer_error)) {
            MessageBoxA(window_, ui_renderer_error.c_str(),
                        "Fable II Native - UI renderer failed", MB_OK | MB_ICONERROR);
            return false;
        }
        apply_aa_setting();  // apply the default Anti-Aliasing option (rebuilds the UI PSOs + MSAA target)

        ShowWindow(window_, SW_SHOWDEFAULT);
        UpdateWindow(window_);
        return true;
    }

    int run() {
        MSG message{};
        auto previous = std::chrono::steady_clock::now();
        // Don't resize on the first frame — only when the user actually changes the setting.
        last_resolution_index_ = game_.frontend.resolution_index();
        while (message.message != WM_QUIT) {
            while (PeekMessageA(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message);
                DispatchMessageA(&message);
            }
            const auto now = std::chrono::steady_clock::now();
            const double delta = std::chrono::duration<double>(now - previous).count();
            previous = now;
            if (delta > 0.0) current_fps_ = current_fps_ * 0.9 + (1.0 / delta) * 0.1;
            // 'G' toggles gameplay control on/off in the World (edge-detected).
            {
                const bool g = (GetForegroundWindow() == window_) &&
                               (GetAsyncKeyState('G') & 0x8000) != 0;
                if (g && !gameplay_toggle_held_) gameplay_mode_ = !gameplay_mode_;
                gameplay_toggle_held_ = g;
            }
            // The sim runs the InWorld gameplay path only when gameplay control is on and
            // the front end is showing the World; otherwise it stays a Frontend-only tick.
            game_.mode = (gameplay_mode_ && game_.frontend.state() == f2::FrontendState::World)
                             ? f2::GameMode::InWorld
                             : f2::GameMode::Frontend;
            game_.tick(delta);
            update_video();
            input_.poll();
            handle_input();
            update_free_camera(delta);
            update_character_controller(delta);
            apply_resolution_setting();
            apply_aa_setting();
            audio_.tick();
            audio_.set_volumes(game_.frontend.sounds_volume(), game_.frontend.music_volume(),
                               game_.frontend.voice_volume());
            const auto state = game_.frontend.state();
            audio_.set_music_enabled(game_.frontend.frontend_music_active() &&
                                      game_.frontend.sound_enabled());
            draw();
            if (game_.frontend.quit_requested()) PostMessageA(window_, WM_CLOSE, 0, 0);
        }
        shutdown();
        return static_cast<int>(message.wParam);
    }

private:
    bool select_game_source() {
        // Visual front-end iteration can run without a full extracted game tree.
        // Normal game launches still require detect_game_source() below.
        if (command_line_flag(L"--ui-only")) return true;
        std::optional<std::filesystem::path> requested = command_line_path(L"--game-dir");
        if (!requested) requested = read_configured_source();

        std::string error;
        if (requested) {
            if (const auto source = f2::detect_game_source(*requested, error)) {
                source_ = *source;
                save_configured_source(source_->root);
                return true;
            }
            MessageBoxA(window_, error.c_str(), "Fable II Native - source not usable",
                        MB_OK | MB_ICONERROR);
        }

        requested = show_source_picker(window_, false);
        if (requested) {
            if (const auto source = f2::detect_game_source(*requested, error)) {
                source_ = *source;
                save_configured_source(source_->root);
                return true;
            }
            MessageBoxA(window_, error.c_str(), "Fable II Native - source not usable",
                        MB_OK | MB_ICONERROR);
        }

        const auto iso = command_line_path(L"--iso").value_or(
            show_source_picker(window_, true).value_or(std::filesystem::path{}));
        if (!iso.empty()) {
            MessageBoxA(window_,
                        "The ISO is an acquisition source. Run f2native_installer.exe to extract it into a user-selected directory, then relaunch Fable2Native.",
                        "Fable II Native - extraction required", MB_OK | MB_ICONINFORMATION);
        }
        return false;
    }

    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        FrontendApp* app = reinterpret_cast<FrontendApp*>(
            GetWindowLongPtrA(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTA*>(lparam);
            app = static_cast<FrontendApp*>(create->lpCreateParams);
            SetWindowLongPtrA(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        }
        if (app && message == WM_SIZE && wparam != SIZE_MINIMIZED) {
            app->resize(LOWORD(lparam), HIWORD(lparam));
            return 0;
        }
        if (message == WM_DESTROY) {
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcA(window, message, wparam, lparam);
    }

    bool create_device() {
        // Opt-in D3D12 debug layer (FABLE2NATIVE_D3D_DEBUG=1): validates every call and
        // lets us drain the real GPU errors instead of guessing why a draw produced nothing.
        if (std::getenv("FABLE2NATIVE_D3D_DEBUG")) {
            ComPtr<ID3D12Debug> debug;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
            d3d_debug_enabled_ = true;
        }
        ComPtr<IDXGIFactory7> factory;
        if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) return false;
        ComPtr<IDXGIAdapter4> adapter;
        for (UINT index = 0; factory->EnumAdapterByGpuPreference(
                 index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                 IID_PPV_ARGS(&adapter)) != DXGI_ERROR_NOT_FOUND; ++index) {
            DXGI_ADAPTER_DESC3 description{};
            adapter->GetDesc3(&description);
            if (description.Flags & DXGI_ADAPTER_FLAG3_SOFTWARE) {
                adapter.Reset();
                continue;
            }
            if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                                            IID_PPV_ARGS(&device_)))) break;
            adapter.Reset();
        }
        if (!device_) {
            ComPtr<IDXGIAdapter> warp;
            if (FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))) ||
                FAILED(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0,
                                         IID_PPV_ARGS(&device_)))) return false;
        }

        if (d3d_debug_enabled_) device_.As(&info_queue_);

        D3D12_COMMAND_QUEUE_DESC queue_desc{};
        queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(device_->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue_)))) return false;

        DXGI_SWAP_CHAIN_DESC1 swap_desc{};
        swap_desc.BufferCount = kFrameCount;
        swap_desc.Width = width_;
        swap_desc.Height = height_;
        swap_desc.Format = kBackBufferFormat;
        swap_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swap_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        swap_desc.SampleDesc.Count = 1;
        ComPtr<IDXGISwapChain1> swap_chain;
        if (FAILED(factory->CreateSwapChainForHwnd(queue_.Get(), window_, &swap_desc,
                                                   nullptr, nullptr, &swap_chain)) ||
            FAILED(swap_chain.As(&swap_chain_))) return false;
        factory->MakeWindowAssociation(window_, DXGI_MWA_NO_ALT_ENTER);

        D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{};
        rtv_desc.NumDescriptors = kFrameCount + 2;  // +1 MSAA resolve target, +1 HDR scene target
        rtv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        if (FAILED(device_->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtv_heap_)))) return false;
        D3D12_DESCRIPTOR_HEAP_DESC dsv_desc{};
        dsv_desc.NumDescriptors = 2;  // [0] world depth, [1] sun shadow map depth
        dsv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        if (FAILED(device_->CreateDescriptorHeap(&dsv_desc, IID_PPV_ARGS(&dsv_heap_)))) return false;
        dsv_handle_ = dsv_heap_->GetCPUDescriptorHandleForHeapStart();
        {
            const UINT dsv_stride =
                device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
            shadow_dsv_ = dsv_handle_;
            shadow_dsv_.ptr += dsv_stride;  // second DSV slot
        }
        D3D12_DESCRIPTOR_HEAP_DESC srv_desc{};
        srv_desc.NumDescriptors =
            f2::NativeWorldRenderer::kMaxMaterialTextures + kUiTextureCount + 5;
        srv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        srv_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(device_->CreateDescriptorHeap(&srv_desc, IID_PPV_ARGS(&descriptor_heap_)))) return false;
        descriptor_stride_ = device_->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        video_descriptor_index_ = f2::NativeWorldRenderer::kMaxMaterialTextures + kUiTextureCount;
        font_descriptor_index_ = video_descriptor_index_ + 1;
        depth_descriptor_index_ = font_descriptor_index_ + 1;
        hdr_descriptor_index_ = depth_descriptor_index_ + 1;
        shadow_descriptor_index_ = hdr_descriptor_index_ + 1;

        rtv_stride_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        auto handle = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
        for (UINT i = 0; i < kFrameCount; ++i) {
            if (FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                        IID_PPV_ARGS(&frames_[i].allocator)))) return false;
            frames_[i].rtv = handle;
            handle.ptr += rtv_stride_;
            if (FAILED(swap_chain_->GetBuffer(i, IID_PPV_ARGS(&frames_[i].render_target)))) return false;
            device_->CreateRenderTargetView(frames_[i].render_target.Get(), nullptr, frames_[i].rtv);
        }
        msaa_rtv_ = handle;  // the extra RTV slot after the back buffers
        handle.ptr += rtv_stride_;
        hdr_rtv_ = handle;  // the RTV slot after MSAA, for the HDR scene target
        create_depth_target();
        create_hdr_target();
        create_shadow_map();
        if (FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                               frames_[0].allocator.Get(), nullptr,
                                               IID_PPV_ARGS(&command_list_))) ||
            FAILED(command_list_->Close()) ||
            FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)))) return false;
        fence_event_ = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        return fence_event_ != nullptr;
    }

    void resize(UINT width, UINT height) {
        if (!device_ || width == 0 || height == 0 || (width == width_ && height == height_)) return;
        wait_for_gpu();
        for (auto& frame : frames_) frame.render_target.Reset();
        swap_chain_->ResizeBuffers(kFrameCount, width, height, kBackBufferFormat, 0);
        width_ = width;
        height_ = height;
        auto handle = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
        for (UINT i = 0; i < kFrameCount; ++i) {
            frames_[i].rtv = handle;
            handle.ptr += rtv_stride_;
            swap_chain_->GetBuffer(i, IID_PPV_ARGS(&frames_[i].render_target));
            device_->CreateRenderTargetView(frames_[i].render_target.Get(), nullptr, frames_[i].rtv);
        }
        create_msaa_target();  // match the MSAA target to the new size
        create_depth_target();  // match the depth buffer to the new size
        create_hdr_target();    // match the HDR scene target to the new size
        if (tonemap_renderer_.ready() && hdr_scene_) {
            tonemap_renderer_.ensure_targets(device_.Get(), hdr_scene_.Get(), width_, height_);
        }
        world_renderer_.set_scene_depth_copy(depth_target_.Get(), depth_copy_.Get(),
                                             depth_gpu_handle_);
    }

    // Real backend owner for the Options "Resolution" setting: when the user changes it, resize the
    // window to the selected client size (which drives WM_SIZE -> resize()) and ensure the swapchain
    // matches. The value lives in the FrontendController, so it survives leaving/reopening Options.
    void apply_resolution_setting() {
        const int index = game_.frontend.resolution_index();
        if (index == last_resolution_index_) return;
        last_resolution_index_ = index;
        const UINT target_w = static_cast<UINT>(game_.frontend.resolution_width());
        const UINT target_h = static_cast<UINT>(game_.frontend.resolution_height());
        if (!window_ || target_w == 0 || target_h == 0 ||
            (target_w == width_ && target_h == height_)) {
            return;
        }
        RECT rect{0, 0, static_cast<LONG>(target_w), static_cast<LONG>(target_h)};
        const DWORD style = static_cast<DWORD>(GetWindowLongPtrA(window_, GWL_STYLE));
        const DWORD ex_style = static_cast<DWORD>(GetWindowLongPtrA(window_, GWL_EXSTYLE));
        AdjustWindowRectEx(&rect, style, FALSE, ex_style);
        SetWindowPos(window_, nullptr, 0, 0, rect.right - rect.left, rect.bottom - rect.top,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        resize(target_w, target_h);  // idempotent if WM_SIZE already handled it
    }

    // Anti-Aliasing option index -> MSAA sample count (0 Off, 1 2x, 2 4x, 3 8x).
    static UINT samples_for_aa(int index) {
        switch (index) {
        case 1: return 2;
        case 2: return 4;
        case 3: return 8;
        default: return 1;
        }
    }

    // Clamp a desired sample count down to what the device supports for the back-buffer format.
    UINT supported_sample_count(UINT desired) const {
        for (UINT s = desired; s > 1; s /= 2) {
            D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS levels{};
            levels.Format = kBackBufferFormat;
            levels.SampleCount = s;
            if (SUCCEEDED(device_->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS,
                                                       &levels, sizeof(levels))) &&
                levels.NumQualityLevels > 0) {
                return s;
            }
        }
        return 1;
    }

    // (Re)create the World depth buffer at the current size. A D32 depth-stencil view gives the
    // world renderer real occlusion (without it the level draws as a flat merged silhouette).
    void create_depth_target() {
        depth_target_.Reset();
        depth_copy_.Reset();
        if (!device_ || width_ == 0 || height_ == 0) return;
        D3D12_RESOURCE_DESC description{};
        description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        description.Width = width_;
        description.Height = height_;
        description.DepthOrArraySize = 1;
        description.MipLevels = 1;
        // Typeless storage permits both the D32 DSV and an R32_FLOAT copy/SRV view.
        description.Format = DXGI_FORMAT_R32_TYPELESS;
        description.SampleDesc.Count = 1;
        description.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_CLEAR_VALUE clear{};
        clear.Format = kDepthFormat;
        // Reversed-Z world depth clears to the far value 0.0 (matching the per-frame clear).
        clear.DepthStencil.Depth = 0.0f;
        if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
                                                    D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear,
                                                    IID_PPV_ARGS(&depth_target_)))) {
            depth_target_.Reset();
            return;
        }
        D3D12_DEPTH_STENCIL_VIEW_DESC view{};
        view.Format = kDepthFormat;
        view.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        device_->CreateDepthStencilView(depth_target_.Get(), &view, dsv_handle_);

        D3D12_RESOURCE_DESC copy_description = description;
        copy_description.Flags = D3D12_RESOURCE_FLAG_NONE;
        D3D12_HEAP_PROPERTIES copy_heap{};
        copy_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        if (FAILED(device_->CreateCommittedResource(
                &copy_heap, D3D12_HEAP_FLAG_NONE, &copy_description,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                IID_PPV_ARGS(&depth_copy_)))) {
            depth_target_.Reset();
            return;
        }
        D3D12_SHADER_RESOURCE_VIEW_DESC depth_srv{};
        depth_srv.Format = DXGI_FORMAT_R32_FLOAT;
        depth_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        depth_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        depth_srv.Texture2D.MipLevels = 1;
        auto depth_cpu = descriptor_heap_->GetCPUDescriptorHandleForHeapStart();
        depth_cpu.ptr += static_cast<std::size_t>(depth_descriptor_index_) * descriptor_stride_;
        device_->CreateShaderResourceView(depth_copy_.Get(), &depth_srv, depth_cpu);
        depth_gpu_handle_ = descriptor_heap_->GetGPUDescriptorHandleForHeapStart();
        depth_gpu_handle_.ptr += static_cast<std::size_t>(depth_descriptor_index_) * descriptor_stride_;
    }

    // Sun shadow map (retail "Render ShadowBuffers"): a fixed-size square depth target the world
    // renderer replays opaque geometry into from the sun POV, then samples in the world PS. Created
    // once (size is window-independent); starts in DEPTH_WRITE.
    void create_shadow_map() {
        shadow_map_.Reset();
        if (!device_) return;
        D3D12_RESOURCE_DESC description{};
        description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        description.Width = kShadowSize;
        description.Height = kShadowSize;
        description.DepthOrArraySize = 1;
        description.MipLevels = 1;
        description.Format = DXGI_FORMAT_R32_TYPELESS;  // D32 DSV + R32_FLOAT SRV
        description.SampleDesc.Count = 1;
        description.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_CLEAR_VALUE clear{};
        clear.Format = DXGI_FORMAT_D32_FLOAT;
        clear.DepthStencil.Depth = 1.0f;  // standard-Z far
        if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
                                                    D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear,
                                                    IID_PPV_ARGS(&shadow_map_)))) {
            shadow_map_.Reset();
            return;
        }
        D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
        dsv.Format = DXGI_FORMAT_D32_FLOAT;
        dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        device_->CreateDepthStencilView(shadow_map_.Get(), &dsv, shadow_dsv_);
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R32_FLOAT;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        auto cpu = descriptor_heap_->GetCPUDescriptorHandleForHeapStart();
        cpu.ptr += static_cast<std::size_t>(shadow_descriptor_index_) * descriptor_stride_;
        device_->CreateShaderResourceView(shadow_map_.Get(), &srv, cpu);
        shadow_srv_gpu_ = descriptor_heap_->GetGPUDescriptorHandleForHeapStart();
        shadow_srv_gpu_.ptr += static_cast<std::size_t>(shadow_descriptor_index_) * descriptor_stride_;
    }

    // Optional HDR-compositor tuning overrides (defaults give the retail glow):
    //   FABLE2NATIVE_HDR_EXPOSURE, FABLE2NATIVE_BLOOM_THRESHOLD, FABLE2NATIVE_BLOOM_INTENSITY.
    void read_hdr_options() {
        const auto env_float = [](const char* name, float& out) {
            char buf[64];
            if (GetEnvironmentVariableA(name, buf, sizeof(buf)) > 0) {
                try {
                    out = std::stof(buf);
                } catch (...) {
                }
            }
        };
        env_float("FABLE2NATIVE_HDR_EXPOSURE", hdr_exposure_);
        env_float("FABLE2NATIVE_BLOOM_THRESHOLD", hdr_bloom_threshold_);
        env_float("FABLE2NATIVE_BLOOM_INTENSITY", hdr_bloom_intensity_);
    }

    // (Re)create the HDR scene target the World passes render into (RGBA16F). The retail engine
    // renders the world HDR then runs a compositor (tonemap/exposure + bloom) to the LDR back buffer
    // (rendering_pipeline.txt §D.3); native mirrors that with this target + NativeTonemapRenderer.
    void create_hdr_target() {
        hdr_scene_.Reset();
        if (!device_ || width_ == 0 || height_ == 0) return;
        D3D12_RESOURCE_DESC description{};
        description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        description.Width = width_;
        description.Height = height_;
        description.DepthOrArraySize = 1;
        description.MipLevels = 1;
        description.Format = f2::kSceneColorFormat;
        description.SampleDesc.Count = 1;
        description.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        // Clear to the scene sky_color, same as the old direct-to-back-buffer World clear.
        D3D12_CLEAR_VALUE clear{};
        clear.Format = f2::kSceneColorFormat;
        for (int i = 0; i < 4; ++i) clear.Color[i] = game_.scene.sky_color[i];
        if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
                                                    D3D12_RESOURCE_STATE_RENDER_TARGET, &clear,
                                                    IID_PPV_ARGS(&hdr_scene_)))) {
            hdr_scene_.Reset();
            return;
        }
        D3D12_RENDER_TARGET_VIEW_DESC rtv{};
        rtv.Format = f2::kSceneColorFormat;
        rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        device_->CreateRenderTargetView(hdr_scene_.Get(), &rtv, hdr_rtv_);

        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = f2::kSceneColorFormat;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        auto cpu = descriptor_heap_->GetCPUDescriptorHandleForHeapStart();
        cpu.ptr += static_cast<std::size_t>(hdr_descriptor_index_) * descriptor_stride_;
        device_->CreateShaderResourceView(hdr_scene_.Get(), &srv, cpu);
        hdr_gpu_handle_ = descriptor_heap_->GetGPUDescriptorHandleForHeapStart();
        hdr_gpu_handle_.ptr += static_cast<std::size_t>(hdr_descriptor_index_) * descriptor_stride_;
    }

    // (Re)create the multisampled resolve target at the current size + sample count. Released at 1x.
    void create_msaa_target() {
        msaa_target_.Reset();
        if (msaa_samples_ <= 1 || !device_ || width_ == 0 || height_ == 0) return;
        D3D12_RESOURCE_DESC description{};
        description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        description.Width = width_;
        description.Height = height_;
        description.DepthOrArraySize = 1;
        description.MipLevels = 1;
        description.Format = kBackBufferFormat;
        description.SampleDesc.Count = msaa_samples_;
        description.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
                                                    D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                                    IID_PPV_ARGS(&msaa_target_)))) {
            msaa_target_.Reset();
            msaa_samples_ = 1;  // fall back to no MSAA on allocation failure
            return;
        }
        D3D12_RENDER_TARGET_VIEW_DESC view{};
        view.Format = kBackBufferFormat;
        view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMS;
        device_->CreateRenderTargetView(msaa_target_.Get(), &view, msaa_rtv_);
    }

    // Apply the Options "Anti-Aliasing" setting: rebuild the UI pipelines for the new sample count and
    // (re)create the MSAA resolve target. No-op when the setting hasn't changed.
    void apply_aa_setting() {
        const int index = game_.frontend.anti_aliasing_index();
        if (index == last_aa_index_ && msaa_target_) return;
        if (index == last_aa_index_ && samples_for_aa(index) <= 1) return;
        last_aa_index_ = index;
        const UINT samples = supported_sample_count(samples_for_aa(index));
        wait_for_gpu();
        if (samples != native_ui_renderer_.sample_count()) {
            std::string error;
            native_ui_renderer_.initialise(device_.Get(), samples, error);
        }
        msaa_samples_ = samples;
        create_msaa_target();
    }

    void update_video() {
        const auto state = game_.frontend.state();
        const bool video_state = state == f2::FrontendState::IntroVideo ||
                                  state == f2::FrontendState::AttractVideo;
        if (!video_state || video_root_.empty()) {
            audio_.stop_video_audio();
            video_decoder_.close();
            active_video_path_.clear();
            video_frame_ = {};
            video_next_frame_time_ = 0.0;
            if (video_texture_) {
                wait_for_gpu();
                video_texture_.Reset();
                video_upload_.Reset();
                video_width_ = 0;
                video_height_ = 0;
                uploaded_video_serial_ = 0;
                video_texture_shader_state_ = false;
            }
            return;
        }
        const auto* clip = state == f2::FrontendState::IntroVideo
                               ? game_.frontend.intro_videos().current_clip()
                               : game_.frontend.attract_videos().current_clip();
        if (!clip) return;
        const auto path = video_root_ / clip->asset;
        if (path != active_video_path_) {
            active_video_path_ = path;
            video_frame_ = {};
            video_next_frame_time_ = 0.0;
            audio_.stop_video_audio();
            if (!video_decoder_.open(path, video_error_)) return;
            // Start the movie soundtrack in lock-step with its first frame. The video timeline is
            // wall-clock driven, so real-time XAudio2 playback stays in sync for these short clips.
            if (video_decoder_.has_audio() && game_.frontend.sound_enabled()) {
                audio_.play_video_audio(video_decoder_.audio_pcm(),
                                        video_decoder_.audio_channels(),
                                        video_decoder_.audio_sample_rate());
            }
        }
        const double target_time = state == f2::FrontendState::IntroVideo
                                       ? game_.frontend.intro_videos().current_time()
                                       : game_.frontend.attract_videos().current_time();
        const double frame_duration = video_decoder_.frame_duration_seconds();
        if (video_frame_.serial == 0) {
            if (video_decoder_.read_next_frame(video_frame_, video_error_)) {
                video_next_frame_time_ = frame_duration;
            }
        }
        for (int frame_count = 0;
             video_frame_.serial != 0 && target_time + 0.000001 >= video_next_frame_time_ &&
             frame_count < 8;
             ++frame_count) {
            f2::NativeVideoFrame next_frame;
            if (!video_decoder_.read_next_frame(next_frame, video_error_)) break;
            video_frame_ = std::move(next_frame);
            video_next_frame_time_ += frame_duration;
        }
    }

    bool ensure_video_texture(const f2::NativeVideoFrame& frame) {
        if (frame.width == 0 || frame.height == 0) return false;
        if (video_texture_ && video_width_ == frame.width && video_height_ == frame.height) return true;
        wait_for_gpu();
        video_texture_.Reset();
        video_upload_.Reset();
        video_texture_shader_state_ = false;
        video_width_ = frame.width;
        video_height_ = frame.height;
        D3D12_RESOURCE_DESC texture_description{};
        texture_description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texture_description.Width = frame.width;
        texture_description.Height = frame.height;
        texture_description.DepthOrArraySize = 1;
        texture_description.MipLevels = 1;
        texture_description.Format = kBackBufferFormat;
        texture_description.SampleDesc.Count = 1;
        texture_description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        D3D12_HEAP_PROPERTIES default_heap{};
        default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        if (FAILED(device_->CreateCommittedResource(
                &default_heap, D3D12_HEAP_FLAG_NONE, &texture_description,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&video_texture_)))) return false;
        device_->GetCopyableFootprints(&texture_description, 0, 1, 0, &video_footprint_,
                                       &video_row_count_, &video_row_size_, &video_upload_size_);
        D3D12_RESOURCE_DESC upload_description{};
        upload_description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        upload_description.Width = video_upload_size_;
        upload_description.Height = 1;
        upload_description.DepthOrArraySize = 1;
        upload_description.MipLevels = 1;
        upload_description.SampleDesc.Count = 1;
        upload_description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES upload_heap{};
        upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        if (FAILED(device_->CreateCommittedResource(
                &upload_heap, D3D12_HEAP_FLAG_NONE, &upload_description,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&video_upload_)))) return false;
        auto cpu = descriptor_heap_->GetCPUDescriptorHandleForHeapStart();
        cpu.ptr += static_cast<std::size_t>(video_descriptor_index_) * descriptor_stride_;
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = kBackBufferFormat;
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Texture2D.MipLevels = 1;
        device_->CreateShaderResourceView(video_texture_.Get(), &view, cpu);
        video_gpu_handle_ = descriptor_heap_->GetGPUDescriptorHandleForHeapStart();
        video_gpu_handle_.ptr += static_cast<std::size_t>(video_descriptor_index_) * descriptor_stride_;
        return true;
    }

    static std::size_t ui_slot(f2::NativeUiAsset asset) {
        switch (asset) {
        case f2::NativeUiAsset::TitleBackground: return 0;
        case f2::NativeUiAsset::Logo: return 1;
        case f2::NativeUiAsset::Accept: return 2;
        case f2::NativeUiAsset::Back: return 3;
        case f2::NativeUiAsset::MainBackground: return 4;
        case f2::NativeUiAsset::MenuSurface: return 5;
        case f2::NativeUiAsset::Sparkle1: return 6;
        case f2::NativeUiAsset::Sparkle2: return 7;
        case f2::NativeUiAsset::Sparkle3: return 8;
        case f2::NativeUiAsset::Sparkle4: return 9;
        case f2::NativeUiAsset::Sparkle5: return 10;
        case f2::NativeUiAsset::Sparkle6: return 11;
        case f2::NativeUiAsset::Sparkle7: return 12;
        case f2::NativeUiAsset::Sparkle8: return 13;
        case f2::NativeUiAsset::AmbientAtlas: return 14;
        case f2::NativeUiAsset::AmbientBaseline: return 15;
        case f2::NativeUiAsset::AmbientDetail: return 16;
        case f2::NativeUiAsset::FrameElements: return 17;
        case f2::NativeUiAsset::AbilityElements: return 18;
        case f2::NativeUiAsset::MenuFrameOverlay: return 19;
        case f2::NativeUiAsset::Frames04: return 20;
        case f2::NativeUiAsset::CardBoy: return 21;
        case f2::NativeUiAsset::CardGirl: return 22;
        case f2::NativeUiAsset::CardBack: return 23;
        case f2::NativeUiAsset::SideRailAtlas: return 24;
        case f2::NativeUiAsset::MenuFrameLeftUpper: return 25;
        case f2::NativeUiAsset::MenuFrameLeftLower: return 26;
        case f2::NativeUiAsset::MenuFrameRightUpper: return 27;
        case f2::NativeUiAsset::MenuFrameRightLower: return 28;
        case f2::NativeUiAsset::FramesPage: return 29;
        case f2::NativeUiAsset::FramesPageTexture: return 30;
        case f2::NativeUiAsset::SliderFrame: return 31;
        case f2::NativeUiAsset::Motifs: return 32;
        case f2::NativeUiAsset::CalibrationImage: return 33;
        case f2::NativeUiAsset::GoldCoin: return 34;
        case f2::NativeUiAsset::LogoFlare: return 35;
        default: return 0;
        }
    }

    // Stable TextureId for an arbitrary D3D12 descriptor, synced into the registry under `key`.
    // (Slots 0..kUiTextureCount are reserved for UI assets; use high keys for non-slot textures
    // like the ImGui glyph atlas or the video frame — a temporary bridge until native text lands.)
    static constexpr std::uint32_t kFontTextureKey = 0x1000;
    static constexpr std::uint32_t kVideoTextureKey = 0x1001;
    f2::render::TextureId texture_id_for(std::uint32_t key, D3D12_GPU_DESCRIPTOR_HANDLE handle) {
        const auto id = texture_registry_.id_for_key(key);
        texture_registry_.set_handle(id, handle.ptr);
        return id;
    }

    // Stable TextureId for a UI asset, with its current D3D12 descriptor synced into the registry.
    // This is the resolver bridge for the backend-neutral scene path (f2::render).
    f2::render::TextureId ui_texture_id(f2::NativeUiAsset asset) {
        const std::size_t slot = ui_slot(asset);
        return texture_id_for(static_cast<std::uint32_t>(slot), ui_textures_[slot].gpu);
    }

    D3D12_GPU_DESCRIPTOR_HANDLE resolve_ui_texture(f2::render::TextureId id) const {
        D3D12_GPU_DESCRIPTOR_HANDLE handle{};
        handle.ptr = texture_registry_.handle(id);
        return handle;
    }

    // The shared, backend-neutral scene builder (docs/FRONTEND_ARCHITECTURE.md). Lazily constructed so
    // its refs to native_font_/ui_assets_ are valid; the texture-access adapter bridges logical assets
    // to this app's D3D12-backed TextureIds (id 0 when uncreated, shader_ready gating the strict draws).
    f2::FrontendSceneBuilder& scene_builder() {
        if (!scene_builder_) {
            f2::FrontendSceneBuilder::TextureAccess access;
            access.id = [this](f2::NativeUiAsset asset) -> f2::render::TextureId {
                return ui_textures_[ui_slot(asset)].texture.Get() != nullptr
                           ? ui_texture_id(asset)
                           : f2::render::kInvalidTexture;
            };
            access.shader_ready = [this](f2::NativeUiAsset asset) {
                const auto& texture = ui_textures_[ui_slot(asset)];
                return texture.texture.Get() != nullptr && texture.shader_read;
            };
            access.font_id = [this]() { return texture_id_for(kFontTextureKey, font_texture_.gpu); };
            access.font_ready = [this]() {
                return font_texture_.texture.Get() != nullptr && font_texture_.shader_read;
            };
            scene_builder_.emplace(native_font_, ui_assets_, std::move(access));
        }
        return *scene_builder_;
    }

    bool ensure_ui_texture(f2::NativeUiAsset asset) {
        const auto* source = ui_assets_.texture(asset);
        if (!source) return false;
        auto& target = ui_textures_[ui_slot(asset)];
        if (target.texture && target.width == source->width && target.height == source->height) {
            return true;
        }
        wait_for_gpu();
        target = {};
        target.width = source->width;
        target.height = source->height;
        target.descriptor_index = f2::NativeWorldRenderer::kMaxMaterialTextures +
                                  static_cast<UINT>(ui_slot(asset));
        D3D12_RESOURCE_DESC description{};
        description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        description.Width = source->width;
        description.Height = source->height;
        description.DepthOrArraySize = 1;
        description.MipLevels = 1;
        description.Format = kBackBufferFormat;
        description.SampleDesc.Count = 1;
        description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        D3D12_HEAP_PROPERTIES default_heap{};
        default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        if (FAILED(device_->CreateCommittedResource(
                &default_heap, D3D12_HEAP_FLAG_NONE, &description,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                IID_PPV_ARGS(&target.texture)))) return false;
        device_->GetCopyableFootprints(&description, 0, 1, 0, &target.footprint,
                                       &target.row_count, &target.row_size, &target.upload_size);
        D3D12_RESOURCE_DESC upload_description{};
        upload_description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        upload_description.Width = target.upload_size;
        upload_description.Height = 1;
        upload_description.DepthOrArraySize = 1;
        upload_description.MipLevels = 1;
        upload_description.SampleDesc.Count = 1;
        upload_description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES upload_heap{};
        upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        if (FAILED(device_->CreateCommittedResource(
                &upload_heap, D3D12_HEAP_FLAG_NONE, &upload_description,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                IID_PPV_ARGS(&target.upload)))) return false;
        auto cpu = descriptor_heap_->GetCPUDescriptorHandleForHeapStart();
        cpu.ptr += static_cast<std::size_t>(target.descriptor_index) * descriptor_stride_;
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = kBackBufferFormat;
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Texture2D.MipLevels = 1;
        device_->CreateShaderResourceView(target.texture.Get(), &view, cpu);
        target.gpu = descriptor_heap_->GetGPUDescriptorHandleForHeapStart();
        target.gpu.ptr += static_cast<std::size_t>(target.descriptor_index) * descriptor_stride_;
        return true;
    }

    // Rasterize the native font (once) and create its GPU atlas texture in the dedicated SRV slot.
    // The retail title font is preferred; a Windows system font is the fallback so text always draws.
    bool ensure_font_texture() {
        if (!native_font_.ready()) {
            std::string error;
            bool loaded = false;
            if (!ui_assets_.title_font_path().empty()) {
                loaded = native_font_.load(ui_assets_.title_font_path(), 48.0f, error);
            }
            if (!loaded) {
                for (const char* fallback :
                     {"C:/Windows/Fonts/segoeui.ttf", "C:/Windows/Fonts/arial.ttf"}) {
                    if (native_font_.load(fallback, 48.0f, error)) {
                        loaded = true;
                        break;
                    }
                }
            }
            if (!loaded) return false;
        }
        if (font_texture_.texture) return true;
        wait_for_gpu();
        font_texture_ = {};
        font_texture_.width = native_font_.atlas_width();
        font_texture_.height = native_font_.atlas_height();
        font_texture_.descriptor_index = font_descriptor_index_;
        D3D12_RESOURCE_DESC description{};
        description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        description.Width = font_texture_.width;
        description.Height = font_texture_.height;
        description.DepthOrArraySize = 1;
        description.MipLevels = 1;
        description.Format = kBackBufferFormat;
        description.SampleDesc.Count = 1;
        description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        D3D12_HEAP_PROPERTIES default_heap{};
        default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        if (FAILED(device_->CreateCommittedResource(
                &default_heap, D3D12_HEAP_FLAG_NONE, &description,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                IID_PPV_ARGS(&font_texture_.texture)))) return false;
        device_->GetCopyableFootprints(&description, 0, 1, 0, &font_texture_.footprint,
                                       &font_texture_.row_count, &font_texture_.row_size,
                                       &font_texture_.upload_size);
        D3D12_RESOURCE_DESC upload_description{};
        upload_description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        upload_description.Width = font_texture_.upload_size;
        upload_description.Height = 1;
        upload_description.DepthOrArraySize = 1;
        upload_description.MipLevels = 1;
        upload_description.SampleDesc.Count = 1;
        upload_description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES upload_heap{};
        upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        if (FAILED(device_->CreateCommittedResource(
                &upload_heap, D3D12_HEAP_FLAG_NONE, &upload_description,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                IID_PPV_ARGS(&font_texture_.upload)))) return false;
        auto cpu = descriptor_heap_->GetCPUDescriptorHandleForHeapStart();
        cpu.ptr += static_cast<std::size_t>(font_texture_.descriptor_index) * descriptor_stride_;
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = kBackBufferFormat;
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Texture2D.MipLevels = 1;
        device_->CreateShaderResourceView(font_texture_.texture.Get(), &view, cpu);
        font_texture_.gpu = descriptor_heap_->GetGPUDescriptorHandleForHeapStart();
        font_texture_.gpu.ptr +=
            static_cast<std::size_t>(font_texture_.descriptor_index) * descriptor_stride_;
        return true;
    }

    void ensure_ui_textures() {
        ensure_ui_texture(f2::NativeUiAsset::TitleBackground);
        ensure_ui_texture(f2::NativeUiAsset::MainBackground);
        ensure_ui_texture(f2::NativeUiAsset::Logo);
        ensure_ui_texture(f2::NativeUiAsset::Accept);
        ensure_ui_texture(f2::NativeUiAsset::Back);
        ensure_ui_texture(f2::NativeUiAsset::MenuSurface);
        ensure_ui_texture(f2::NativeUiAsset::Sparkle1);
        ensure_ui_texture(f2::NativeUiAsset::Sparkle2);
        ensure_ui_texture(f2::NativeUiAsset::Sparkle3);
        ensure_ui_texture(f2::NativeUiAsset::Sparkle4);
        ensure_ui_texture(f2::NativeUiAsset::Sparkle5);
        ensure_ui_texture(f2::NativeUiAsset::Sparkle6);
        ensure_ui_texture(f2::NativeUiAsset::Sparkle7);
        ensure_ui_texture(f2::NativeUiAsset::Sparkle8);
        ensure_ui_texture(f2::NativeUiAsset::AmbientAtlas);
        ensure_ui_texture(f2::NativeUiAsset::AmbientBaseline);
        ensure_ui_texture(f2::NativeUiAsset::FrameElements);
        ensure_ui_texture(f2::NativeUiAsset::AbilityElements);
        ensure_ui_texture(f2::NativeUiAsset::MenuFrameOverlay);
        ensure_ui_texture(f2::NativeUiAsset::Frames04);
        ensure_ui_texture(f2::NativeUiAsset::CardBoy);
        ensure_ui_texture(f2::NativeUiAsset::CardGirl);
        ensure_ui_texture(f2::NativeUiAsset::CardBack);
        ensure_ui_texture(f2::NativeUiAsset::SideRailAtlas);
        ensure_ui_texture(f2::NativeUiAsset::MenuFrameLeftUpper);
        ensure_ui_texture(f2::NativeUiAsset::MenuFrameLeftLower);
        ensure_ui_texture(f2::NativeUiAsset::MenuFrameRightUpper);
        ensure_ui_texture(f2::NativeUiAsset::MenuFrameRightLower);
        ensure_ui_texture(f2::NativeUiAsset::FramesPage);
        ensure_ui_texture(f2::NativeUiAsset::FramesPageTexture);
        ensure_ui_texture(f2::NativeUiAsset::SliderFrame);
        ensure_ui_texture(f2::NativeUiAsset::Motifs);
        ensure_ui_texture(f2::NativeUiAsset::CalibrationImage);
        ensure_ui_texture(f2::NativeUiAsset::GoldCoin);
        ensure_ui_texture(f2::NativeUiAsset::LogoFlare);
        ensure_font_texture();
    }

    void upload_ui_textures(ID3D12GraphicsCommandList* command_list) {
        // Upload the native font atlas on the first frame after creation (same COPY_DEST -> SRV path
        // as the UI assets, but sourced from the rasterized atlas rather than ui_assets_).
        if (font_texture_.texture && !font_texture_.shader_read &&
            native_font_.atlas_rgba8().size() >=
                static_cast<std::size_t>(font_texture_.width) * font_texture_.height * 4) {
            void* mapped = nullptr;
            if (SUCCEEDED(font_texture_.upload->Map(0, nullptr, &mapped))) {
                auto* destination = static_cast<std::uint8_t*>(mapped) + font_texture_.footprint.Offset;
                const auto source_row_pitch = static_cast<std::size_t>(font_texture_.width) * 4;
                for (std::uint32_t row = 0; row < font_texture_.height; ++row) {
                    std::memcpy(destination + row * font_texture_.footprint.Footprint.RowPitch,
                                native_font_.atlas_rgba8().data() + row * source_row_pitch,
                                source_row_pitch);
                }
                font_texture_.upload->Unmap(0, nullptr);
                D3D12_TEXTURE_COPY_LOCATION destination_location{};
                destination_location.pResource = font_texture_.texture.Get();
                destination_location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                D3D12_TEXTURE_COPY_LOCATION source_location{};
                source_location.pResource = font_texture_.upload.Get();
                source_location.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                source_location.PlacedFootprint = font_texture_.footprint;
                command_list->CopyTextureRegion(&destination_location, 0, 0, 0, &source_location,
                                                 nullptr);
                D3D12_RESOURCE_BARRIER barrier{};
                barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barrier.Transition.pResource = font_texture_.texture.Get();
                barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                command_list->ResourceBarrier(1, &barrier);
                font_texture_.shader_read = true;
            }
        }
        const std::array<f2::NativeUiAsset, 36> assets = {
            f2::NativeUiAsset::TitleBackground, f2::NativeUiAsset::MainBackground,
            f2::NativeUiAsset::Logo, f2::NativeUiAsset::Accept, f2::NativeUiAsset::Back,
            f2::NativeUiAsset::MenuSurface, f2::NativeUiAsset::Sparkle1,
            f2::NativeUiAsset::Sparkle2, f2::NativeUiAsset::Sparkle3,
            f2::NativeUiAsset::Sparkle4, f2::NativeUiAsset::Sparkle5,
            f2::NativeUiAsset::Sparkle6, f2::NativeUiAsset::Sparkle7,
            f2::NativeUiAsset::Sparkle8, f2::NativeUiAsset::AmbientAtlas,
            f2::NativeUiAsset::AmbientBaseline, f2::NativeUiAsset::AmbientDetail,
            f2::NativeUiAsset::FrameElements,
            f2::NativeUiAsset::AbilityElements, f2::NativeUiAsset::MenuFrameOverlay,
            f2::NativeUiAsset::Frames04, f2::NativeUiAsset::CardBoy,
            f2::NativeUiAsset::CardGirl, f2::NativeUiAsset::CardBack,
            f2::NativeUiAsset::SideRailAtlas, f2::NativeUiAsset::MenuFrameLeftUpper,
            f2::NativeUiAsset::MenuFrameLeftLower, f2::NativeUiAsset::MenuFrameRightUpper,
            f2::NativeUiAsset::MenuFrameRightLower,
            f2::NativeUiAsset::FramesPage, f2::NativeUiAsset::FramesPageTexture,
            f2::NativeUiAsset::SliderFrame, f2::NativeUiAsset::Motifs,
            f2::NativeUiAsset::CalibrationImage, f2::NativeUiAsset::GoldCoin,
            f2::NativeUiAsset::LogoFlare};
        for (const auto asset : assets) {
            const auto* source = ui_assets_.texture(asset);
            if (!source) continue;
            auto& target = ui_textures_[ui_slot(asset)];
            if (!target.texture || target.shader_read) continue;
            void* mapped = nullptr;
            if (FAILED(target.upload->Map(0, nullptr, &mapped))) continue;
            auto* destination = static_cast<std::uint8_t*>(mapped) + target.footprint.Offset;
            const auto source_row_pitch = static_cast<std::size_t>(source->width) * 4;
            for (std::uint32_t row = 0; row < source->height; ++row) {
                std::memcpy(destination + row * target.footprint.Footprint.RowPitch,
                            source->rgba8.data() + row * source_row_pitch, source_row_pitch);
            }
            target.upload->Unmap(0, nullptr);
            D3D12_TEXTURE_COPY_LOCATION destination_location{};
            destination_location.pResource = target.texture.Get();
            destination_location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            D3D12_TEXTURE_COPY_LOCATION source_location{};
            source_location.pResource = target.upload.Get();
            source_location.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            source_location.PlacedFootprint = target.footprint;
            command_list->CopyTextureRegion(&destination_location, 0, 0, 0,
                                             &source_location, nullptr);
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = target.texture.Get();
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            command_list->ResourceBarrier(1, &barrier);
            target.shader_read = true;
        }
        upload_ambient_detail_frame(command_list);
    }

    void upload_ambient_detail_frame(ID3D12GraphicsCommandList* command_list) {
        if (!command_list || game_.frontend.state() != f2::FrontendState::Title) return;
        const auto frame_count = ui_assets_.ambient_detail_frame_count();
        const auto* main = ui_assets_.texture(f2::NativeUiAsset::AmbientAtlas);
        if (frame_count == 0 || !main) return;
        const auto elapsed = std::max(0.0, game_.frontend.state_time() - 5.90);
        const auto frame_index = static_cast<std::size_t>(std::floor(elapsed * 60.0)) % frame_count;
        if (frame_index == ambient_detail_frame_uploaded_) return;
        const auto* detail = ui_assets_.ambient_detail_frame(frame_index);
        f2::NativeTexture composed;
        if (!detail || !f2::compose_native_ambient_atlas(*main, *detail, composed)) return;
        auto& target = ui_textures_[ui_slot(f2::NativeUiAsset::AmbientAtlas)];
        if (!target.texture || !target.upload || target.width != composed.width ||
            target.height != composed.height) return;
        void* mapped = nullptr;
        if (FAILED(target.upload->Map(0, nullptr, &mapped))) return;
        auto* destination = static_cast<std::uint8_t*>(mapped) + target.footprint.Offset;
        const auto source_row_pitch = static_cast<std::size_t>(composed.width) * 4;
        for (std::uint32_t row = 0; row < composed.height; ++row) {
            std::memcpy(destination + row * target.footprint.Footprint.RowPitch,
                        composed.rgba8.data() + row * source_row_pitch, source_row_pitch);
        }
        target.upload->Unmap(0, nullptr);
        if (target.shader_read) {
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = target.texture.Get();
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            command_list->ResourceBarrier(1, &barrier);
        }
        D3D12_TEXTURE_COPY_LOCATION destination_location{};
        destination_location.pResource = target.texture.Get();
        destination_location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION source_location{};
        source_location.pResource = target.upload.Get();
        source_location.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        source_location.PlacedFootprint = target.footprint;
        command_list->CopyTextureRegion(&destination_location, 0, 0, 0,
                                        &source_location, nullptr);
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = target.texture.Get();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        command_list->ResourceBarrier(1, &barrier);
        target.shader_read = true;
        ambient_detail_frame_uploaded_ = frame_index;
    }

    void upload_video_frame(ID3D12GraphicsCommandList* command_list) {
        if (!video_texture_ || video_frame_.serial == 0 ||
            video_frame_.serial == uploaded_video_serial_) return;
        void* mapped = nullptr;
        if (FAILED(video_upload_->Map(0, nullptr, &mapped))) return;
        auto* destination = static_cast<std::uint8_t*>(mapped) + video_footprint_.Offset;
        const auto source_row_pitch = static_cast<std::size_t>(video_frame_.width) * 4;
        for (std::uint32_t row = 0; row < video_frame_.height; ++row) {
            std::memcpy(destination + row * video_footprint_.Footprint.RowPitch,
                        video_frame_.rgba8.data() + row * source_row_pitch, source_row_pitch);
        }
        video_upload_->Unmap(0, nullptr);
        if (video_texture_shader_state_) {
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = video_texture_.Get();
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            command_list->ResourceBarrier(1, &barrier);
        }
        D3D12_TEXTURE_COPY_LOCATION destination_location{};
        destination_location.pResource = video_texture_.Get();
        destination_location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION source_location{};
        source_location.pResource = video_upload_.Get();
        source_location.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        source_location.PlacedFootprint = video_footprint_;
        command_list->CopyTextureRegion(&destination_location, 0, 0, 0, &source_location, nullptr);
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = video_texture_.Get();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        command_list->ResourceBarrier(1, &barrier);
        video_texture_shader_state_ = true;
        uploaded_video_serial_ = video_frame_.serial;
    }


    void render_native_main_menu(ID3D12GraphicsCommandList* command_list) {
        if (!native_ui_renderer_.ready()) return;
        // Migrated onto the SHARED backend-neutral scene builder (docs/FRONTEND_ARCHITECTURE.md): the
        // menu carousel, the Options chrome/page, and the ChooseCard modal all build here so Vulkan
        // renders the identical scene. The app only resolves TextureIds to descriptors.
        f2::render::UiDrawList scene;
        scene_builder().build_main_menu(scene, static_cast<float>(width_),
                                        static_cast<float>(height_), game_,
                                        input_.using_controller_prompts());
        native_ui_renderer_.render(command_list, width_, height_, scene.quads(),
                                   [this](f2::render::TextureId id) { return resolve_ui_texture(id); });
    }

    void render_native_choose_card(ID3D12GraphicsCommandList* command_list) {
        if (!native_ui_renderer_.ready()) return;
        f2::render::UiDrawList scene;
        scene_builder().build_choose_card(scene, static_cast<float>(width_),
                                          static_cast<float>(height_));
        native_ui_renderer_.render(command_list, width_, height_, scene.quads(),
                                   [this](f2::render::TextureId id) { return resolve_ui_texture(id); });
    }

    // Native (ImGui-free) intro/attract video: a full-screen quad of the decoded video frame.
    void render_native_video(ID3D12GraphicsCommandList* command_list) {
        if (!native_ui_renderer_.ready() || !video_texture_ || !video_texture_shader_state_) return;
        f2::render::UiDrawList scene;
        scene_builder().build_video(scene, static_cast<float>(width_), static_cast<float>(height_),
                                    texture_id_for(kVideoTextureKey, video_gpu_handle_));
        native_ui_renderer_.render(command_list, width_, height_, scene.quads(),
                                   [this](f2::render::TextureId id) { return resolve_ui_texture(id); });
    }

    // Native (ImGui-free) loading screen — a centered caption while the world streams in.
    void render_native_loading(ID3D12GraphicsCommandList* command_list) {
        if (!native_ui_renderer_.ready()) return;
        f2::render::UiDrawList scene;
        scene_builder().build_loading(scene, static_cast<float>(width_), static_cast<float>(height_));
        native_ui_renderer_.render(command_list, width_, height_, scene.quads(),
                                   [this](f2::render::TextureId id) { return resolve_ui_texture(id); });
    }

    void render_native_title(ID3D12GraphicsCommandList* command_list) {
        if (!native_ui_renderer_.ready()) return;
        // Migrated onto the SHARED backend-neutral scene builder (docs/FRONTEND_ARCHITECTURE.md) so the
        // Vulkan frontend renders the identical title. The app just resolves TextureIds -> descriptors.
        f2::render::UiDrawList scene;
        // Debug A/B hook: FABLE2NATIVE_TITLE_TIME pins the title clock to a fixed value so a
        // screenshot lands on an exact reveal moment (for matched native-vs-retail comparison).
        double title_clock = game_.frontend.state_time();
        if (const char* pin = std::getenv("FABLE2NATIVE_TITLE_TIME")) {
            title_clock = std::atof(pin);
        }
        scene_builder().build_title(scene, static_cast<float>(width_), static_cast<float>(height_),
                                    title_clock,
                                    input_.prompt(f2::NativeInputAction::Accept),
                                    input_.using_controller_prompts());
        native_ui_renderer_.render(command_list, width_, height_, scene.quads(),
                                   [this](f2::render::TextureId id) { return resolve_ui_texture(id); });
    }


    void handle_input() {
        using Action = f2::NativeInputAction;
        const auto play_sound = [&](f2::NativeFrontendSound sound) {
            if (game_.frontend.sound_enabled()) audio_.play(sound);
        };
        // Retail fires a distinct SE_GUI event per direction (docs/RETAIL_FRONTEND_SPEC.md §8):
        // stick up/down = SE_GUI_SLIDE_MENU_UP/DOWN, value left/right = SE_GUI_SELECTION_LEFT/RIGHT.
        if (input_.pressed(Action::Up)) {
            game_.frontend.dispatch(f2::FrontendAction::Up);
            play_sound(f2::NativeFrontendSound::NavigateUp);
        }
        if (input_.pressed(Action::Down)) {
            game_.frontend.dispatch(f2::FrontendAction::Down);
            play_sound(f2::NativeFrontendSound::NavigateDown);
        }
        if (input_.pressed(Action::Left)) {
            game_.frontend.dispatch(f2::FrontendAction::Left);
            play_sound(f2::NativeFrontendSound::SelectionLeft);
        }
        if (input_.pressed(Action::Right)) {
            game_.frontend.dispatch(f2::FrontendAction::Right);
            play_sound(f2::NativeFrontendSound::SelectionRight);
        }
        if (input_.pressed(Action::Accept)) {
            game_.frontend.dispatch(f2::FrontendAction::Accept);
            play_sound(f2::NativeFrontendSound::Accept);
        }
        if (input_.pressed(Action::Back)) {
            game_.frontend.dispatch(f2::FrontendAction::Back);
            play_sound(f2::NativeFrontendSound::Back);
        }
        if (input_.pressed(Action::Skip)) game_.frontend.dispatch(f2::FrontendAction::Skip);
        // The front end is controller/keyboard driven BY DESIGN (a centered animated cursor). No
        // mouse hit-testing: on a centered-wheel menu, hover/click re-centers rows and reads as
        // erratic. Arrows/stick move the cursor; Enter/A activates; Esc/B backs out.
    }

    // Free-fly camera for level inspection (World state only): WASD = move on the view plane,
    // Q/E = down/up, arrow keys = look (yaw/pitch), Shift = boost. Reads the raw keyboard so it
    // is independent of the frontend action bindings (which drive menu navigation). The camera is
    // lazily framed to a 3/4 vantage of the cooked scene on the first World frame.
    void update_free_camera(double delta) {
        if (game_.frontend.state() != f2::FrontendState::World) {
            world_cam_initialised_ = false;  // re-frame next time we enter the world
            world_renderer_.clear_free_camera();
            return;
        }
        // Gameplay mode: the follow camera already wrote game_.camera in game_.tick;
        // just hand it to the renderer (skip the free-fly WASD/arrow overwrite).
        if (gameplay_mode_) {
            world_renderer_.set_free_camera(game_.camera.position, game_.camera.yaw,
                                            game_.camera.pitch);
            world_cam_initialised_ = false;  // re-frame the inspection cam when toggled back off
            return;
        }
        auto center = world_renderer_.scene_center();
        float radius = world_renderer_.scene_radius();
        const bool hero_view = game_.scene.has_hero_start;
        if (hero_view) {
            center = game_.scene.hero_start;
            center[1] += 0.8f;
            radius = 8.0f;
        }
        if (!world_cam_initialised_) {
            // Match the auto-orbit's default 3/4 framing so the first view is familiar, then hand
            // control to the keys.
            game_.camera.yaw = 0.6f;
            game_.camera.pitch = -0.32f;
            const float cp = std::cos(game_.camera.pitch);
            const std::array<float, 3> fwd{cp * std::sin(game_.camera.yaw), std::sin(game_.camera.pitch),
                                           cp * std::cos(game_.camera.yaw)};
            const float dist = hero_view ? 8.0f : radius * 2.4f;
            game_.camera.position = {center[0] - fwd[0] * dist, center[1] - fwd[1] * dist,
                                     center[2] - fwd[2] * dist};
            world_cam_initialised_ = true;
        }
        const bool focused = GetForegroundWindow() == window_;
        const auto down = [&](int vk) { return focused && (GetAsyncKeyState(vk) & 0x8000) != 0; };
        const float dt = static_cast<float>(delta);
        const float look = 1.6f * dt;  // radians/sec
        if (down(VK_LEFT)) game_.camera.yaw -= look;
        if (down(VK_RIGHT)) game_.camera.yaw += look;
        if (down(VK_UP)) game_.camera.pitch += look;
        if (down(VK_DOWN)) game_.camera.pitch -= look;
        const float pit_lim = 1.55f;  // avoid gimbal flip near straight up/down
        game_.camera.pitch = std::clamp(game_.camera.pitch, -pit_lim, pit_lim);
        const float cp = std::cos(game_.camera.pitch);
        const std::array<float, 3> fwd{cp * std::sin(game_.camera.yaw), std::sin(game_.camera.pitch),
                                       cp * std::cos(game_.camera.yaw)};
        const std::array<float, 3> world_up{0.0f, 1.0f, 0.0f};
        std::array<float, 3> rt{fwd[1] * world_up[2] - fwd[2] * world_up[1],
                                fwd[2] * world_up[0] - fwd[0] * world_up[2],
                                fwd[0] * world_up[1] - fwd[1] * world_up[0]};
        const float rl = std::sqrt(rt[0] * rt[0] + rt[1] * rt[1] + rt[2] * rt[2]);
        if (rl > 1e-4f) { rt[0] /= rl; rt[1] /= rl; rt[2] /= rl; }
        const float boost = down(VK_SHIFT) ? 4.0f : 1.0f;
        const float speed = std::max(radius * 0.35f, 2.0f) * boost * dt;
        auto& p = game_.camera.position;
        const auto move = [&](const std::array<float, 3>& d, float s) {
            p[0] += d[0] * s; p[1] += d[1] * s; p[2] += d[2] * s;
        };
        if (down('W')) move(fwd, speed);
        if (down('S')) move(fwd, -speed);
        if (down('D')) move(rt, speed);
        if (down('A')) move(rt, -speed);
        if (down('E')) move(world_up, speed);
        if (down('Q')) move(world_up, -speed);
        world_renderer_.set_free_camera(p, game_.camera.yaw, game_.camera.pitch);
    }

    // Lightweight hero locomotion for cooked scenes: IJKL moves the hero draw ranges without
    // rebuilding the static world buffers. It is intentionally separate from WASD free flight.
    void update_character_controller(double delta) {
        if (game_.frontend.state() != f2::FrontendState::World || !scene_has_hero()) {
            character_offset_ = {0.0f, 0.0f, 0.0f};
            character_motion_phase_ = 0.0f;
            character_motion_strength_ = 0.0f;
            world_renderer_.set_character_offset(character_offset_);
            world_renderer_.set_character_motion(character_motion_phase_, character_motion_strength_);
            return;
        }
        // Gameplay mode: the hero draw follows the character controller (collision-
        // resolved) instead of the free IJKL offset. character_offset_ is the delta
        // from the RE'd PlayerStart the renderer draws the hero mesh relative to.
        if (gameplay_mode_) {
            const auto& p = game_.player.position();
            const auto& hs = game_.scene.hero_start;
            character_offset_ = {p[0] - hs[0], p[1] - hs[1], p[2] - hs[2]};
            const float move_mag = std::sqrt(game_.input.move[0] * game_.input.move[0] +
                                             game_.input.move[1] * game_.input.move[1]);
            character_motion_strength_ = move_mag > 0.05f ? 1.0f : 0.0f;
            if (character_motion_strength_ > 0.0f)
                character_motion_phase_ += static_cast<float>(delta) * 6.0f;
            world_renderer_.set_character_offset(character_offset_);
            world_renderer_.set_character_motion(character_motion_phase_, character_motion_strength_);
            return;
        }
        const bool focused = GetForegroundWindow() == window_;
        const auto down = [&](int vk) {
            return focused && (GetAsyncKeyState(vk) & 0x8000) != 0;
        };
        if (down('F') && game_.scene.has_hero_start) world_cam_initialised_ = false;
        const float speed = (down(VK_SHIFT) ? 16.0f : 4.0f) * static_cast<float>(delta);
        if (down('R')) character_offset_ = {0.0f, 0.0f, 0.0f};
        const bool moving = down('I') || down('K') || down('J') || down('L') || down('U') || down('O');
        if (moving) character_motion_phase_ += static_cast<float>(delta) * (down(VK_SHIFT) ? 10.0f : 6.0f);
        character_motion_strength_ = moving ? 1.0f : 0.0f;
        if (down('I')) character_offset_[2] += speed;
        if (down('K')) character_offset_[2] -= speed;
        if (down('L')) character_offset_[0] += speed;
        if (down('J')) character_offset_[0] -= speed;
        if (down('U')) character_offset_[1] += speed;
        if (down('O')) character_offset_[1] -= speed;
        world_renderer_.set_character_offset(character_offset_);
        world_renderer_.set_character_motion(character_motion_phase_, character_motion_strength_);
    }

    bool scene_has_hero() const {
        for (const auto& mesh : game_.scene.meshes) {
            if (mesh.name.rfind("hero", 0) == 0) return true;
        }
        return false;
    }

    void draw() {
        if ((game_.frontend.state() == f2::FrontendState::IntroVideo ||
             game_.frontend.state() == f2::FrontendState::AttractVideo) &&
            video_frame_.serial != 0) {
            ensure_video_texture(video_frame_);
        }
        if (game_.frontend.state() == f2::FrontendState::Title ||
            game_.frontend.state() == f2::FrontendState::MainMenu ||
            game_.frontend.state() == f2::FrontendState::ChooseCard ||
            game_.frontend.state() == f2::FrontendState::Options) {
            ensure_ui_textures();
        } else if (game_.frontend.state() == f2::FrontendState::World) {
            ensure_font_texture();
        }
        const auto state = game_.frontend.state();
        const UINT frame_index = swap_chain_->GetCurrentBackBufferIndex();
        auto& frame = frames_[frame_index];
        frame.allocator->Reset();
        command_list_->Reset(frame.allocator.Get(), nullptr);
        upload_video_frame(command_list_.Get());
        upload_ui_textures(command_list_.Get());
        // The title wordmark reveals over BLACK before the winter panorama fades/scrolls in
        // (build_title fades the background in from ~5.9s).
        // World clears to the cooked scene's own sky_color (the level sits against its sky, not a
        // black void); Title reveals over black; other frontend states use the dim menu backdrop.
        const std::array<float, 4> clear =
            state == f2::FrontendState::World
                ? game_.scene.sky_color
                : (state == f2::FrontendState::Title
                       ? std::array<float, 4>{0.0f, 0.0f, 0.0f, 1.0f}
                       : std::array<float, 4>{0.015f, 0.02f, 0.035f, 1.0f});
        const auto transition = [](ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                                   D3D12_RESOURCE_STATES after) {
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = resource;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = before;
            b.Transition.StateAfter = after;
            return b;
        };
        // MSAA (Options > Anti-Aliasing): render the frontend UI into a multisampled target and resolve
        // it into the back buffer. World renders directly (its pipeline is single-sample).
        const bool msaa =
            msaa_samples_ > 1 && msaa_target_ && state != f2::FrontendState::World;
        // World renders into the HDR scene target (RGBA16F), then the tonemap compositor resolves it
        // to the LDR back buffer (rendering_pipeline.txt §D.3). If the compositor is unavailable, the
        // World branch falls back to rendering directly to the back buffer (old clamp path).
        const bool use_hdr =
            state == f2::FrontendState::World && tonemap_renderer_.ready() && hdr_scene_;
        if (!msaa) {
            const auto to_rt = transition(frame.render_target.Get(), D3D12_RESOURCE_STATE_PRESENT,
                                          D3D12_RESOURCE_STATE_RENDER_TARGET);
            command_list_->ResourceBarrier(1, &to_rt);
        }
        const D3D12_CPU_DESCRIPTOR_HANDLE target_rtv = msaa ? msaa_rtv_ : frame.rtv;
        // The World passes draw into scene_rtv (the HDR target when compositing, else the back
        // buffer); the compositor and every non-World state target the back buffer directly.
        const D3D12_CPU_DESCRIPTOR_HANDLE scene_rtv = use_hdr ? hdr_rtv_ : target_rtv;
        command_list_->OMSetRenderTargets(1, &scene_rtv, FALSE, nullptr);
        command_list_->ClearRenderTargetView(scene_rtv, clear.data(), 0, nullptr);
        // Bind the SRV heap BEFORE any draw: the world renderer sets a root descriptor
        // table (its material textures), which requires the heap already bound.
        ID3D12DescriptorHeap* heaps[] = {descriptor_heap_.Get()};
        command_list_->SetDescriptorHeaps(1, heaps);
        const bool shadows = state == f2::FrontendState::World && shadow_map_;
        if (state == f2::FrontendState::World) {
            // Sun shadow pass FIRST (retail Render ShadowBuffers): a depth-only replay of opaque
            // geometry from the sun POV into the shadow map, then transitioned to a PS resource so
            // the world PS can sample it. Runs before the sky/world colour passes.
            if (shadows) {
                world_renderer_.render_shadow(command_list_.Get(), game_.scene,
                                              game_.elapsed_seconds);
                const auto to_srv =
                    transition(shadow_map_.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
                               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                command_list_->ResourceBarrier(1, &to_srv);
            }
            // Rebind the same color target WITH the depth buffer so the world renderer
            // gets real occlusion (frontend states render depthless above).
            if (depth_target_) {
                command_list_->OMSetRenderTargets(1, &scene_rtv, FALSE, &dsv_handle_);
                // Reversed-Z: clear to 0.0 (the far value); the world PSO tests GREATER_EQUAL.
                command_list_->ClearDepthStencilView(dsv_handle_, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0,
                                                     0, nullptr);
            }
            // Procedural sky FIRST (fills every pixel behind the world; no depth test/write),
            // replacing the flat sky_color clear as the backdrop. It binds its OWN descriptor
            // heap, so re-bind the app heap afterwards for the world renderer's material table.
            if (sky_renderer_.ready()) {
                const auto sky_camera =
                    world_renderer_.compute_camera(width_, height_, game_.elapsed_seconds);
                sky_renderer_.render(command_list_.Get(), game_.scene, sky_camera, width_, height_,
                                     game_.elapsed_seconds);
                command_list_->SetDescriptorHeaps(1, heaps);
            }
            // Cloud layers over the sky, still behind the world (own descriptor heap like the sky;
            // re-bind the app heap for the world material table afterwards).
            if (cloud_renderer_.ready() && !game_.scene.clouds.empty()) {
                const auto cam =
                    world_renderer_.compute_camera(width_, height_, game_.elapsed_seconds);
                const auto vp =
                    world_renderer_.compute_view_projection(width_, height_, game_.elapsed_seconds);
                cloud_renderer_.render(command_list_.Get(), game_.scene, vp, cam.position,
                                       cam.forward, width_, height_, game_.elapsed_seconds);
                command_list_->SetDescriptorHeaps(1, heaps);
            }
            // Celestial billboards (night moon + glare) over the clouds, behind the world.
            if (billboard_renderer_.ready() && game_.scene.has_moon) {
                const auto cam =
                    world_renderer_.compute_camera(width_, height_, game_.elapsed_seconds);
                billboard_renderer_.render(command_list_.Get(), game_.scene, cam, width_, height_);
                command_list_->SetDescriptorHeaps(1, heaps);
            }
            // Procedural night stars (additive point sprites), drawn last of the sky passes
            // (retail order), still behind the world.
            if (stars_renderer_.ready() && game_.scene.star_brightness > 0.0f) {
                const auto cam =
                    world_renderer_.compute_camera(width_, height_, game_.elapsed_seconds);
                stars_renderer_.render(command_list_.Get(), game_.scene, cam, width_, height_,
                                       game_.elapsed_seconds);
            }
            world_renderer_.render(command_list_.Get(), game_.scene, width_, height_,
                                   game_.elapsed_seconds);
            // HDR -> LDR compositor: resolve the RGBA16F scene target to the LDR back buffer
            // (tonemap/exposure). The UI overlay below then draws on the composited back buffer.
            if (use_hdr) {
                const auto to_srv =
                    transition(hdr_scene_.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                command_list_->ResourceBarrier(1, &to_srv);
                tonemap_renderer_.render(command_list_.Get(), target_rtv, width_, height_,
                                         hdr_exposure_, hdr_bloom_threshold_, hdr_bloom_intensity_);
                const auto to_rt =
                    transition(hdr_scene_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                               D3D12_RESOURCE_STATE_RENDER_TARGET);
                command_list_->ResourceBarrier(1, &to_rt);
                // The compositor bound its own descriptor heap; restore the app heap for the UI.
                command_list_->SetDescriptorHeaps(1, heaps);
            }
            // Restore the shadow map to DEPTH_WRITE for next frame's shadow pass.
            if (shadows) {
                const auto to_depth =
                    transition(shadow_map_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                               D3D12_RESOURCE_STATE_DEPTH_WRITE);
                command_list_->ResourceBarrier(1, &to_depth);
            }
        }
        if (state == f2::FrontendState::MainMenu || state == f2::FrontendState::ChooseCard ||
            state == f2::FrontendState::Options) {
            render_native_main_menu(command_list_.Get());
        } else if (state == f2::FrontendState::Title) {
            render_native_title(command_list_.Get());
        } else if (state == f2::FrontendState::IntroVideo ||
                   state == f2::FrontendState::AttractVideo) {
            render_native_video(command_list_.Get());
        } else if (state == f2::FrontendState::Loading) {
            render_native_loading(command_list_.Get());
        }
        if (state == f2::FrontendState::World && native_ui_renderer_.ready()) {
            f2::render::UiDrawList scene;
            scene_builder().build_world_overlay(scene, static_cast<float>(width_),
                                                static_cast<float>(height_), scene_has_hero(),
                                                character_offset_, character_motion_strength_ > 0.5f);
            native_ui_renderer_.render(command_list_.Get(), width_, height_, scene.quads(),
                                       [this](f2::render::TextureId id) {
                                           return resolve_ui_texture(id);
                                       });
        }
        // Optional on-screen FPS counter (Video options toggle). Drawn over the frontend UI states,
        // where the font atlas is guaranteed uploaded.
        if (game_.frontend.fps_display_enabled() &&
            (state == f2::FrontendState::Title || state == f2::FrontendState::MainMenu ||
             state == f2::FrontendState::ChooseCard || state == f2::FrontendState::Options)) {
            f2::render::UiDrawList scene;
            scene_builder().build_fps_overlay(scene, static_cast<float>(width_),
                                              static_cast<float>(height_), current_fps_);
            native_ui_renderer_.render(command_list_.Get(), width_, height_, scene.quads(),
                                       [this](f2::render::TextureId id) { return resolve_ui_texture(id); });
        }
        if (msaa) {
            const D3D12_RESOURCE_BARRIER pre[] = {
                transition(frame.render_target.Get(), D3D12_RESOURCE_STATE_PRESENT,
                           D3D12_RESOURCE_STATE_RESOLVE_DEST),
                transition(msaa_target_.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                           D3D12_RESOURCE_STATE_RESOLVE_SOURCE)};
            command_list_->ResourceBarrier(2, pre);
            command_list_->ResolveSubresource(frame.render_target.Get(), 0, msaa_target_.Get(), 0,
                                              kBackBufferFormat);
            const D3D12_RESOURCE_BARRIER post[] = {
                transition(frame.render_target.Get(), D3D12_RESOURCE_STATE_RESOLVE_DEST,
                           D3D12_RESOURCE_STATE_PRESENT),
                transition(msaa_target_.Get(), D3D12_RESOURCE_STATE_RESOLVE_SOURCE,
                           D3D12_RESOURCE_STATE_RENDER_TARGET)};
            command_list_->ResourceBarrier(2, post);
        } else {
            const auto to_present = transition(frame.render_target.Get(),
                                               D3D12_RESOURCE_STATE_RENDER_TARGET,
                                               D3D12_RESOURCE_STATE_PRESENT);
            command_list_->ResourceBarrier(1, &to_present);
        }
        command_list_->Close();
        ID3D12CommandList* lists[] = {command_list_.Get()};
        queue_->ExecuteCommandLists(1, lists);
        swap_chain_->Present(1, 0);
        wait_for_gpu();
        drain_d3d_debug();
    }

    void drain_d3d_debug() {
        if (!info_queue_) return;
        const UINT64 n = info_queue_->GetNumStoredMessages();
        if (n == 0) return;
        std::ofstream log("d3d_debug.log", std::ios::app);
        for (UINT64 i = 0; i < n; ++i) {
            SIZE_T len = 0;
            info_queue_->GetMessage(i, nullptr, &len);
            std::vector<char> buf(len);
            auto* msg = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
            if (SUCCEEDED(info_queue_->GetMessage(i, msg, &len))) {
                log << "[D3D12 sev=" << msg->Severity << " id=" << msg->ID << "] "
                    << std::string(msg->pDescription, msg->DescriptionByteLength) << "\n";
            }
        }
        info_queue_->ClearStoredMessages();
    }

    void wait_for_gpu() {
        if (!queue_ || !fence_) return;
        const UINT64 value = ++fence_value_;
        queue_->Signal(fence_.Get(), value);
        if (fence_->GetCompletedValue() < value) {
            fence_->SetEventOnCompletion(value, fence_event_);
            WaitForSingleObject(fence_event_, INFINITE);
        }
    }

    void shutdown() {
        wait_for_gpu();
        video_decoder_.close();
        if (video_runtime_started_) {
            f2::stop_native_video_runtime();
            video_runtime_started_ = false;
        }
        if (fence_event_) CloseHandle(fence_event_);
    }

    HWND window_ = nullptr;
    UINT width_ = 1280;
    UINT height_ = 720;
    double current_fps_ = 0.0;  // smoothed FPS for the optional on-screen counter
    bool world_cam_initialised_ = false;  // free-fly camera lazily framed on first World frame
    // Opt-in P2 gameplay (--gameplay / toggle 'G'): drives the hero via the character
    // controller + follow camera instead of the free-fly inspection cam. Default OFF
    // leaves the existing World-inspection behaviour untouched.
    bool gameplay_mode_ = false;
    bool gameplay_toggle_held_ = false;
    std::array<float, 3> character_offset_{0.0f, 0.0f, 0.0f};
    float character_motion_phase_ = 0.0f;
    float character_motion_strength_ = 0.0f;
    int last_resolution_index_ = -1;  // tracks the applied Options "Resolution" value
    int last_aa_index_ = -1;  // tracks the applied Options "Anti-Aliasing" value
    UINT msaa_samples_ = 1;  // current MSAA sample count (1 = off)
    ComPtr<ID3D12Resource> msaa_target_;  // multisampled color target resolved into the back buffer
    D3D12_CPU_DESCRIPTOR_HANDLE msaa_rtv_{};
    ComPtr<ID3D12Resource> hdr_scene_;  // RGBA16F HDR scene target the World passes render into
    D3D12_CPU_DESCRIPTOR_HANDLE hdr_rtv_{};
    D3D12_GPU_DESCRIPTOR_HANDLE hdr_gpu_handle_{};  // SRV for the tonemap compositor
    UINT hdr_descriptor_index_ = 0;
    ComPtr<ID3D12Resource> shadow_map_;  // sun shadow-map depth (retail Render ShadowBuffers)
    D3D12_CPU_DESCRIPTOR_HANDLE shadow_dsv_{};
    D3D12_GPU_DESCRIPTOR_HANDLE shadow_srv_gpu_{};
    UINT shadow_descriptor_index_ = 0;
    float hdr_exposure_ = 1.0f;  // compositor exposure (1.0 == the old direct-to-LDR clamp)
    float hdr_bloom_threshold_ = 0.62f;  // HDR level above which bloom is extracted
    float hdr_bloom_intensity_ = 0.90f;  // bloom add strength (0 == no bloom = byte-identical)
    ComPtr<ID3D12Resource> depth_target_;  // D32 depth buffer for the World state (occlusion)
    ComPtr<ID3D12Resource> depth_copy_;    // shader-readable copy for water shoreline depth
    ComPtr<ID3D12DescriptorHeap> dsv_heap_;
    D3D12_CPU_DESCRIPTOR_HANDLE dsv_handle_{};
    D3D12_GPU_DESCRIPTOR_HANDLE depth_gpu_handle_{};
    UINT rtv_stride_ = 0;
    UINT descriptor_stride_ = 0;
    UINT video_descriptor_index_ = 0;
    UINT64 fence_value_ = 0;
    HANDLE fence_event_ = nullptr;
    std::optional<f2::GameSource> source_;
    std::filesystem::path video_root_;
    std::filesystem::path ui_root_;
    f2::NativeUiAssets ui_assets_;
    std::array<UiGpuTexture, kUiTextureCount> ui_textures_;
    std::size_t ambient_detail_frame_uploaded_ = std::numeric_limits<std::size_t>::max();
    // Native (ImGui-free) font atlas for UI text. Rasterized once from the retail title font (or a
    // system fallback) and uploaded to its own SRV slot; add_native_text samples it instead of
    // ImGui's font atlas (docs/FRONTEND_ARCHITECTURE.md ImGui removal).
    f2::NativeFont native_font_;
    UiGpuTexture font_texture_;
    UINT font_descriptor_index_ = 0;
    UINT depth_descriptor_index_ = 0;
    f2::NativeFrontendAudio audio_;
    std::filesystem::path active_video_path_;
    f2::NativeVideoDecoder video_decoder_;
    f2::NativeVideoFrame video_frame_;
    std::string video_error_;
    bool video_runtime_started_ = false;
    ComPtr<ID3D12Resource> video_texture_;
    ComPtr<ID3D12Resource> video_upload_;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT video_footprint_{};
    UINT video_row_count_ = 0;
    UINT64 video_row_size_ = 0;
    UINT64 video_upload_size_ = 0;
    UINT video_width_ = 0;
    UINT video_height_ = 0;
    UINT64 uploaded_video_serial_ = 0;
    double video_next_frame_time_ = 0.0;
    bool video_texture_shader_state_ = false;
    D3D12_GPU_DESCRIPTOR_HANDLE video_gpu_handle_{};
    f2::NativeWorldRenderer world_renderer_;
    f2::NativeSkyRenderer sky_renderer_;  // procedural atmosphere drawn behind the world
    f2::NativeCloudRenderer cloud_renderer_;  // scrolling cloud layers over the sky, behind world
    f2::NativeSkyBillboardRenderer billboard_renderer_;  // night moon + glare over the clouds
    f2::NativeSkyStarsRenderer stars_renderer_;  // procedural night star field
    f2::NativeTonemapRenderer tonemap_renderer_;  // HDR->LDR compositor (tonemap/exposure)
    f2::NativeUiRenderer native_ui_renderer_;
    f2::render::TextureRegistry texture_registry_;  // maps ui slots -> stable TextureIds (neutral scene)
    std::optional<f2::FrontendSceneBuilder> scene_builder_;  // shared backend-neutral scene builder
    f2::NativeGame game_;
    f2::NativeInputRouter input_;
    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12CommandQueue> queue_;
    bool d3d_debug_enabled_ = false;
    ComPtr<ID3D12InfoQueue> info_queue_;
    ComPtr<IDXGISwapChain4> swap_chain_;
    ComPtr<ID3D12DescriptorHeap> rtv_heap_;
    ComPtr<ID3D12DescriptorHeap> descriptor_heap_;
    ComPtr<ID3D12GraphicsCommandList> command_list_;
    ComPtr<ID3D12Fence> fence_;
    std::array<FrameContext, kFrameCount> frames_;
};

}  // namespace

int f2::run_d3d12_frontend(HINSTANCE instance) {
    FrontendApp app;
    return app.initialise(instance) ? app.run() : 1;
}
