#include "f2/native_game.h"
#include "f2/native_audio.h"
#include "f2/native_frontend_entry.h"
#include "f2/native_install.h"
#include "f2/native_input.h"
#include "f2/native_font.h"
#include "f2/frontend_scene_builder.h"
#include "f2/native_logo_effects.h"
#include "f2/native_ui.h"
#include "f2/native_ui_renderer.h"
#include "f2/native_video_decoder.h"
#include "f2/render/texture_registry.h"
#include "f2/render/ui_draw_list.h"
#include "f2/native_world_renderer.h"


#include <windows.h>
#include <d3d12.h>
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
constexpr UINT kUiTextureCount = 35;

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
        std::string ui_renderer_error;
        if (!native_ui_renderer_.initialise(device_.Get(), ui_renderer_error)) {
            MessageBoxA(window_, ui_renderer_error.c_str(),
                        "Fable II Native - UI renderer failed", MB_OK | MB_ICONERROR);
            return false;
        }

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
            game_.tick(delta);
            update_video();
            input_.poll();
            handle_input();
            apply_resolution_setting();
            audio_.tick();
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
        rtv_desc.NumDescriptors = kFrameCount;
        rtv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        if (FAILED(device_->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtv_heap_)))) return false;
        D3D12_DESCRIPTOR_HEAP_DESC srv_desc{};
        srv_desc.NumDescriptors =
            f2::NativeWorldRenderer::kMaxMaterialTextures + kUiTextureCount + 2;
        srv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        srv_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(device_->CreateDescriptorHeap(&srv_desc, IID_PPV_ARGS(&descriptor_heap_)))) return false;
        descriptor_stride_ = device_->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        video_descriptor_index_ = f2::NativeWorldRenderer::kMaxMaterialTextures + kUiTextureCount;
        font_descriptor_index_ = video_descriptor_index_ + 1;

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
        const std::array<f2::NativeUiAsset, 35> assets = {
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
            f2::NativeUiAsset::CalibrationImage, f2::NativeUiAsset::GoldCoin};
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

    // Emit `text` into `scene` as native-font glyph sprites (ImGui-free). x,y = pen top; size in px.
    // Shared by every native screen (menu/title/options) so text has one code path.
    void emit_text(f2::render::UiDrawList& scene, std::string_view text, float x, float y,
                   float size, std::uint32_t color) {
        if (!font_texture_.texture || !font_texture_.shader_read || !native_font_.ready() ||
            native_font_.pixel_height() <= 0.0f) {
            return;
        }
        const float scale = size / native_font_.pixel_height();
        float cursor = x;
        for (std::size_t offset = 0; offset < text.size();) {
            const auto first = static_cast<unsigned char>(text[offset]);
            std::uint32_t codepoint = '?';
            std::size_t length = 1;
            if (first < 0x80) {
                codepoint = first;
            } else if ((first & 0xe0) == 0xc0 && offset + 1 < text.size()) {
                codepoint = ((first & 0x1f) << 6) |
                            (static_cast<unsigned char>(text[offset + 1]) & 0x3f);
                length = 2;
            } else if ((first & 0xf0) == 0xe0 && offset + 2 < text.size()) {
                codepoint = ((first & 0x0f) << 12) |
                            ((static_cast<unsigned char>(text[offset + 1]) & 0x3f) << 6) |
                            (static_cast<unsigned char>(text[offset + 2]) & 0x3f);
                length = 3;
            } else if ((first & 0xf8) == 0xf0 && offset + 3 < text.size()) {
                codepoint = ((first & 0x07) << 18) |
                            ((static_cast<unsigned char>(text[offset + 1]) & 0x3f) << 12) |
                            ((static_cast<unsigned char>(text[offset + 2]) & 0x3f) << 6) |
                            (static_cast<unsigned char>(text[offset + 3]) & 0x3f);
                length = 4;
            }
            offset += length;
            const auto& glyph = native_font_.glyph(codepoint);
            if (!glyph.valid) continue;
            if (glyph.u1 > glyph.u0 && glyph.v1 > glyph.v0) {  // has a non-empty sprite
                scene.add_sprite(texture_id_for(kFontTextureKey, font_texture_.gpu),
                                 cursor + glyph.x0 * scale, y + glyph.y0 * scale,
                                 cursor + glyph.x1 * scale, y + glyph.y1 * scale, glyph.u0,
                                 glyph.v0, glyph.u1, glyph.v1, color);
            }
            cursor += glyph.advance * scale;
        }
    }

    // Pixel width of `text` at `size` px in the native font (for centering/measuring).
    [[nodiscard]] float text_width(std::string_view text, float size) const {
        return native_font_.measure(text, size);
    }

    // Horizontally-centered text at center-x `cx`.
    void emit_centered(f2::render::UiDrawList& scene, std::string_view text, float cx, float y,
                       float size, std::uint32_t color) {
        emit_text(scene, text, cx - text_width(text, size) * 0.5f, y, size, color);
    }

    // Solid-color rect (samples the font atlas' opaque white block as a point).
    void add_rect(f2::render::UiDrawList& scene, float x0, float y0, float x1, float y1,
                  std::uint32_t color) {
        if (!font_texture_.texture || !font_texture_.shader_read) return;
        const float su = native_font_.solid_u();
        const float sv = native_font_.solid_v();
        scene.add_sprite(texture_id_for(kFontTextureKey, font_texture_.gpu), x0, y0, x1, y1, su, sv,
                         su, sv, color);
    }

    // A retail menu capsule (leather three-slice + frame-elements rim), as used by the Options title
    // and footer pills. Mirrors the ImGui draw_capsule (menu_surface 0.125/0.875 slices + rim band).
    void add_menu_capsule(f2::render::UiDrawList& scene, float x0, float y0, float x1, float y1) {
        const float slice = 52.0f * (width_ / 1280.0f);
        const std::uint32_t white = 0xffffffffu;
        if (ui_textures_[ui_slot(f2::NativeUiAsset::MenuSurface)].texture) {
            const auto id = ui_texture_id(f2::NativeUiAsset::MenuSurface);
            scene.add_sprite(id, x0, y0, x0 + slice, y1, 0.0f, 0.0f, 0.125f, 1.0f, white);
            scene.add_sprite(id, x0 + slice, y0, x1 - slice, y1, 0.125f, 0.0f, 0.875f, 1.0f, white);
            scene.add_sprite(id, x1 - slice, y0, x1, y1, 0.875f, 0.0f, 1.0f, 1.0f, white);
        }
        if (ui_textures_[ui_slot(f2::NativeUiAsset::FrameElements)].texture) {
            const auto id = ui_texture_id(f2::NativeUiAsset::FrameElements);
            const float v0 = 4.0f / 512.0f, v1 = 84.0f / 512.0f;
            const float parts[4] = {0.0f, 0.125f, 0.813f, 0.945f};
            const float xs[4] = {x0, x0 + slice, x1 - slice, x1};
            const float us[4] = {0.0f, 0.125f, 0.820f, 0.945f};
            for (int i = 0; i < 3; ++i) {
                scene.add_sprite(id, xs[i], y0, xs[i + 1], y1, us[i], v0,
                                 (i == 0 ? 0.125f : i == 1 ? 0.813f : 0.945f), v1, white);
                scene.set_last_key_black(true);
            }
            (void)parts;
        }
    }

    // The Options title pill + Back/gold footer pill (native port of the ImGui Options chrome).
    void render_native_options_chrome(f2::render::UiDrawList& scene) {
        const auto rgba = [](std::uint32_t r, std::uint32_t g, std::uint32_t b, std::uint32_t a) {
            return r | (g << 8u) | (b << 16u) | (a << 24u);
        };
        const float scale = width_ / 1280.0f;
        // Title pill.
        const float tx = 220.0f * scale, ty = 53.0f * scale, tw = 390.0f * scale, th = 47.0f * scale;
        add_menu_capsule(scene, tx, ty, tx + tw, ty + th);
        emit_centered(scene, "Options", tx + tw * 0.5f, ty + 8.0f * scale, 27.0f * scale,
                      rgba(238, 238, 238, 255));
        // Footer pill: [B] Back .... (coin) 5400
        const float fx = 220.0f * scale, fy = 594.0f * scale, fw = 390.0f * scale, fh = 47.0f * scale;
        add_menu_capsule(scene, fx, fy, fx + fw, fy + fh);
        const auto& controller = ui_textures_[ui_slot(f2::NativeUiAsset::Accept)];
        if (controller.texture && controller.shader_read) {
            scene.add_sprite(ui_texture_id(f2::NativeUiAsset::Accept), fx + 7.0f * scale,
                             fy + 0.5f * scale, fx + 53.0f * scale, fy + 46.5f * scale, 0.25f, 0.0f,
                             0.50f, 0.25f, rgba(255, 255, 255, 255));
        }
        emit_text(scene, "Back", fx + 58.0f * scale, fy + 10.0f * scale, 22.0f * scale,
                  rgba(238, 238, 238, 255));
        const auto& coin = ui_textures_[ui_slot(f2::NativeUiAsset::GoldCoin)];
        if (coin.texture && coin.shader_read) {
            scene.add_sprite(ui_texture_id(f2::NativeUiAsset::GoldCoin), fx + 247.0f * scale,
                             fy + 7.0f * scale, fx + 279.0f * scale, fy + 39.0f * scale, 0.0f, 0.0f,
                             1.0f, 1.0f, rgba(255, 255, 255, 255));
        }
        emit_text(scene, "5400", fx + 288.0f * scale, fy + 10.0f * scale, 22.0f * scale,
                  rgba(245, 222, 65, 255));
    }

    // Native (ImGui-free) options page panel — ported 1:1 from the legacy ImGui block. Draws into
    // the shared menu scene when state==Options && the options page is open.
    void render_native_options_page(f2::render::UiDrawList& scene) {
        const auto rgba = [](std::uint32_t r, std::uint32_t g, std::uint32_t b, std::uint32_t a) {
            return r | (g << 8u) | (b << 16u) | (a << 24u);
        };
        const float unit = width_ / 1280.0f;
        const auto& options = game_.frontend.options_items();
        const std::size_t selected =
            options.empty() ? std::size_t{0}
                            : std::min(game_.frontend.selected_item(), options.size() - 1);
        const std::string_view page_id =
            options.empty() ? std::string_view{} : std::string_view(options[selected].id);
        const float page_x = width_ * 0.495f;
        const float page_w = width_ * 0.325f;
        const float center_x = page_x + page_w * 0.50f;
        add_rect(scene, page_x - 10.0f * unit, 0.0f, page_x + page_w + 10.0f * unit,
                 static_cast<float>(height_), rgba(0, 0, 0, 110));
        if (ui_textures_[ui_slot(f2::NativeUiAsset::FramesPageTexture)].texture) {
            scene.add_sprite(ui_texture_id(f2::NativeUiAsset::FramesPageTexture), page_x, 0.0f,
                             page_x + page_w, static_cast<float>(height_), 0.0f, 0.0f, 1.0f, 1.0f,
                             rgba(255, 255, 255, 245));
        }
        emit_centered(scene, options.empty() ? "Options" : options[selected].label, center_x,
                      height_ * 0.105f, 27.0f * unit, rgba(105, 55, 27, 255));
        if (ui_textures_[ui_slot(f2::NativeUiAsset::Motifs)].texture) {
            scene.add_sprite(ui_texture_id(f2::NativeUiAsset::Motifs), page_x + page_w * 0.09f,
                             height_ * 0.165f, page_x + page_w * 0.91f, height_ * 0.195f, 0.043f,
                             0.020f, 0.72f, 0.070f, rgba(133, 78, 28, 255));
        }
        const auto arrows = [&](float y) {
            const float aw = 20.0f * unit;
            emit_text(scene, "<", page_x + page_w * 0.12f, y - 24.0f * unit, 28.0f * unit,
                      rgba(192, 107, 57, 235));
            emit_text(scene, ">", page_x + page_w * 0.88f - aw, y - 24.0f * unit, 28.0f * unit,
                      rgba(192, 107, 57, 235));
        };
        const auto value = [&](std::string_view label, std::string_view val, float y) {
            emit_centered(scene, label, center_x, y, 21.0f * unit, rgba(105, 55, 27, 255));
            emit_centered(scene, val, center_x, y + 34.0f * unit, 21.0f * unit, rgba(24, 24, 24, 255));
            arrows(y + 32.0f * unit);
        };
        const auto slider = [&](std::string_view label, int v, float y) {
            if (!label.empty())
                emit_centered(scene, label, center_x, y, 21.0f * unit, rgba(105, 55, 27, 255));
            const float x0 = page_x + page_w * 0.22f;
            const float x1 = page_x + page_w * 0.78f;
            const float by = y + 34.0f * unit;
            add_rect(scene, x0, by, x1, by + 5.0f * unit, rgba(43, 40, 45, 255));
            add_rect(scene, x1 - 18.0f * unit * (static_cast<float>(v) / 100.0f), by, x1,
                     by + 5.0f * unit, rgba(190, 105, 66, 255));
        };
        if (page_id == "game") {
            value("Subtitles", game_.frontend.subtitles_enabled() ? "On" : "Off", height_ * 0.235f);
            value("Glowing Trail Brightness",
                  game_.frontend.breadcrumb_size() == 0   ? "Off"
                  : game_.frontend.breadcrumb_size() == 1 ? "Medium"
                                                          : "Bright",
                  height_ * 0.355f);
            value("Tutorials", game_.frontend.tutorial_boxes_enabled() ? "On" : "Off",
                  height_ * 0.475f);
            value("Online Orbs",
                  game_.frontend.multiplayer_orbs_enabled() ? "Friends Only" : "Off",
                  height_ * 0.595f);
            value("Auto Joinable", game_.frontend.auto_joinable_enabled() ? "On" : "Off",
                  height_ * 0.715f);
        } else if (page_id == "controls") {
            value("Invert Aim", game_.frontend.invert_aim_enabled() ? "On" : "Off", height_ * 0.235f);
        } else if (page_id == "audio") {
            slider("Sounds", game_.frontend.sounds_volume(), height_ * 0.255f);
            slider("Music", game_.frontend.music_volume(), height_ * 0.405f);
            slider("Voice", game_.frontend.voice_volume(), height_ * 0.555f);
            value("Speakers", game_.frontend.speaker_mode() == 0 ? "5.1 Surround" : "Stereo",
                  height_ * 0.695f);
        } else if (page_id == "video") {
            if (ui_textures_[ui_slot(f2::NativeUiAsset::CalibrationImage)].texture) {
                scene.add_sprite(ui_texture_id(f2::NativeUiAsset::CalibrationImage),
                                 page_x + page_w * 0.16f, height_ * 0.23f, page_x + page_w * 0.86f,
                                 height_ * 0.55f, 0.0f, 0.0f, 1.0f, 0.75f, rgba(255, 255, 255, 255));
            }
            const std::uint32_t sel = rgba(145, 72, 30, 255);
            const std::uint32_t nrm = rgba(105, 55, 27, 255);
            emit_centered(scene, "Gamma", center_x, height_ * 0.575f, 21.0f * unit,
                          game_.frontend.video_setting_row() == 0 ? sel : nrm);
            slider("", game_.frontend.gamma_percent(), height_ * 0.615f);
            emit_centered(scene, "Adjust the gamma so that you are just", center_x, height_ * 0.685f,
                          17.0f * unit, rgba(35, 35, 35, 255));
            emit_centered(scene, "able to see the text on the left side", center_x, height_ * 0.725f,
                          17.0f * unit, rgba(35, 35, 35, 255));
            emit_centered(scene, "of the circular image.", center_x, height_ * 0.765f, 17.0f * unit,
                          rgba(35, 35, 35, 255));
            emit_centered(scene, "Display", center_x, height_ * 0.805f, 17.0f * unit,
                          rgba(105, 55, 27, 255));
            const auto display_value = [&](std::string_view label, std::string_view val, float y,
                                           int row) {
                const std::uint32_t col = game_.frontend.video_setting_row() == row ? sel : nrm;
                emit_text(scene, label, page_x + page_w * 0.19f, y, 16.0f * unit, col);
                emit_text(scene, val, page_x + page_w * 0.81f - text_width(val, 16.0f * unit), y,
                          16.0f * unit, rgba(24, 24, 24, 255));
            };
            const char* resolutions[] = {"1280 x 720", "1920 x 1080", "2560 x 1440"};
            const char* anti_aliasing[] = {"Off", "2x", "4x", "8x"};
            display_value("Resolution", resolutions[game_.frontend.resolution_index()],
                          height_ * 0.835f, 1);
            display_value("Anti-Aliasing", anti_aliasing[game_.frontend.anti_aliasing_index()],
                          height_ * 0.875f, 2);
        }
        const auto& controller = ui_textures_[ui_slot(f2::NativeUiAsset::Accept)];
        const bool has_controller = controller.texture && controller.shader_read;
        const auto page_prompt = [&](std::string_view label, float y, float u0) {
            const float icon = 27.0f * unit;
            const float gap = 6.0f * unit;
            const float ts = 17.0f * unit;
            const float mw = text_width(label, ts);
            const float right = page_x + page_w * 0.88f;
            const float left = right - (icon + gap + mw);
            emit_text(scene, label, left + 1.0f * unit, y + 1.0f * unit, ts, rgba(0, 0, 0, 70));
            emit_text(scene, label, left, y, ts, rgba(35, 35, 35, 255));
            if (has_controller) {
                const float icon_x = left + mw + gap;
                scene.add_sprite(ui_texture_id(f2::NativeUiAsset::Accept), icon_x, y - 3.0f * unit,
                                 icon_x + icon, y - 3.0f * unit + icon, u0, 0.0f, u0 + 0.25f, 0.25f,
                                 rgba(255, 255, 255, 255));
            }
        };
        page_prompt("Cancel", height_ * 0.900f, 0.25f);
        page_prompt("Accept", height_ * 0.950f, 0.0f);
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

    static f2::NativeUiAsset sparkle_asset(std::uint8_t index) {
        constexpr std::array<f2::NativeUiAsset, 8> kSparkles = {
            f2::NativeUiAsset::Sparkle1, f2::NativeUiAsset::Sparkle2,
            f2::NativeUiAsset::Sparkle3, f2::NativeUiAsset::Sparkle4,
            f2::NativeUiAsset::Sparkle5, f2::NativeUiAsset::Sparkle6,
            f2::NativeUiAsset::Sparkle7, f2::NativeUiAsset::Sparkle8};
        return kSparkles[index % kSparkles.size()];
    }

    // Seed the title sparkle field once, sampling the logo alpha mask so particles cluster on the
    // FABLE II glyphs (retail's burst forms the wordmark shape). Positions are normalized (0..1)
    // within the logo rect and mapped to pixels each frame.
    void build_title_sparkles() {
        constexpr std::size_t kCount = 160;
        title_sparkles_.clear();
        title_sparkles_.reserve(kCount);
        const f2::NativeTexture* logo = ui_assets_.texture(f2::NativeUiAsset::Logo);
        const bool have_mask = logo && logo->width > 0 && logo->height > 0 &&
                               logo->rgba8.size() >= static_cast<std::size_t>(logo->width) *
                                                          logo->height * 4;
        std::uint32_t seed = 0xF2A11CEu;
        const auto rnd = [&seed]() {
            seed = seed * 1664525u + 1013904223u;
            return static_cast<float>(seed & 0x00ffffffu) / 16777215.0f;
        };
        for (std::size_t i = 0; i < kCount; ++i) {
            float sx = rnd();
            float sy = rnd();
            if (have_mask) {
                for (int attempt = 0; attempt < 48; ++attempt) {
                    sx = rnd();
                    sy = rnd();
                    const auto px = (static_cast<std::size_t>(sy * logo->height) * logo->width +
                                     static_cast<std::size_t>(sx * logo->width)) * 4;
                    if (px + 3 < logo->rgba8.size() && logo->rgba8[px + 3] > 24) break;
                }
            }
            TitleSparkle s;
            s.x = std::clamp(sx + (rnd() - 0.5f) * 0.05f, 0.0f, 1.0f);
            s.y = std::clamp(sy + (rnd() - 0.5f) * 0.05f, 0.0f, 1.0f);
            s.delay = rnd();
            s.life = 0.8f + rnd() * 1.4f;
            s.size = 8.0f + rnd() * 12.0f;
            s.texture = static_cast<std::uint8_t>(rnd() * 8.0f);
            title_sparkles_.push_back(s);
        }
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
        scene_builder().build_title(scene, static_cast<float>(width_), static_cast<float>(height_),
                                    game_.frontend.state_time(),
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
        }
        const auto state = game_.frontend.state();
        const UINT frame_index = swap_chain_->GetCurrentBackBufferIndex();
        auto& frame = frames_[frame_index];
        frame.allocator->Reset();
        command_list_->Reset(frame.allocator.Get(), nullptr);
        upload_video_frame(command_list_.Get());
        upload_ui_textures(command_list_.Get());
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = frame.render_target.Get();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        command_list_->ResourceBarrier(1, &barrier);
        const std::array<float, 4> clear = state == f2::FrontendState::Title
                                                ? std::array<float, 4>{0.0f, 0.0f, 0.0f, 1.0f}
                                                : std::array<float, 4>{0.015f, 0.02f, 0.035f, 1.0f};
        command_list_->OMSetRenderTargets(1, &frame.rtv, FALSE, nullptr);
        command_list_->ClearRenderTargetView(frame.rtv, clear.data(), 0, nullptr);
        if (state == f2::FrontendState::World) {
            world_renderer_.render(command_list_.Get(), game_.scene, width_, height_,
                                   game_.elapsed_seconds);
        }
        ID3D12DescriptorHeap* heaps[] = {descriptor_heap_.Get()};
        command_list_->SetDescriptorHeaps(1, heaps);
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
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        command_list_->ResourceBarrier(1, &barrier);
        command_list_->Close();
        ID3D12CommandList* lists[] = {command_list_.Get()};
        queue_->ExecuteCommandLists(1, lists);
        swap_chain_->Present(1, 0);
        wait_for_gpu();
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
    int last_resolution_index_ = -1;  // tracks the applied Options "Resolution" value
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
    f2::NativeLogoSparkles logo_sparkles_;
    // Title reveal sparkle field. Positions are normalized (0..1) within the wordmark rect,
    // seeded once from the logo alpha mask so sparkles cluster on the FABLE II glyphs. Emitted
    // continuously with per-particle life cycles (retail's burst never one-shots to nothing).
    struct TitleSparkle {
        float x = 0.5f;
        float y = 0.5f;
        float delay = 0.0f;
        float life = 1.0f;
        float size = 12.0f;
        std::uint8_t texture = 0;
    };
    std::vector<TitleSparkle> title_sparkles_;
    // Native (ImGui-free) font atlas for UI text. Rasterized once from the retail title font (or a
    // system fallback) and uploaded to its own SRV slot; add_native_text samples it instead of
    // ImGui's font atlas (docs/FRONTEND_ARCHITECTURE.md ImGui removal).
    f2::NativeFont native_font_;
    UiGpuTexture font_texture_;
    UINT font_descriptor_index_ = 0;
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
    f2::NativeUiRenderer native_ui_renderer_;
    f2::render::TextureRegistry texture_registry_;  // maps ui slots -> stable TextureIds (neutral scene)
    std::optional<f2::FrontendSceneBuilder> scene_builder_;  // shared backend-neutral scene builder
    f2::NativeGame game_;
    f2::NativeInputRouter input_;
    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12CommandQueue> queue_;
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
