#include "f2/native_game.h"
#include "f2/native_audio.h"
#include "f2/native_frontend_entry.h"
#include "f2/native_install.h"
#include "f2/native_input.h"
#include "f2/native_logo_effects.h"
#include "f2/native_ui.h"
#include "f2/native_ui_renderer.h"
#include "f2/native_video_decoder.h"
#include "f2/native_world_renderer.h"

#include "imgui.h"
#include "imgui_impl_dx12.h"
#include "imgui_impl_win32.h"

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

extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND window, UINT message,
                                               WPARAM wparam, LPARAM lparam);

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
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui::StyleColorsDark();
        if (!ui_assets_.title_font_path().empty()) {
            const auto font_path = ui_assets_.title_font_path().string();
            if (auto* font = ImGui::GetIO().Fonts->AddFontFromFileTTF(font_path.c_str(), 26.0f)) {
                ImGui::GetIO().FontDefault = font;
            }
        }
        ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        ImGui_ImplWin32_Init(window_);
        if (!ImGui_ImplDX12_Init(device_.Get(), kFrameCount, kBackBufferFormat,
                                 descriptor_heap_.Get(),
                                 descriptor_heap_->GetCPUDescriptorHandleForHeapStart(),
                                 descriptor_heap_->GetGPUDescriptorHandleForHeapStart())) {
            return false;
        }
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
        if (app && ImGui_ImplWin32_WndProcHandler(window, message, wparam, lparam)) return 1;
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
        srv_desc.NumDescriptors = f2::NativeWorldRenderer::kMaxMaterialTextures + kUiTextureCount + 1;
        srv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        srv_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(device_->CreateDescriptorHeap(&srv_desc, IID_PPV_ARGS(&descriptor_heap_)))) return false;
        descriptor_stride_ = device_->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        video_descriptor_index_ = f2::NativeWorldRenderer::kMaxMaterialTextures + kUiTextureCount;

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
            if (!video_decoder_.open(path, video_error_)) return;
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
    }

    void upload_ui_textures(ID3D12GraphicsCommandList* command_list) {
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

    void render_native_main_menu(ID3D12GraphicsCommandList* command_list) {
        if (!native_ui_renderer_.ready()) return;
        std::vector<f2::NativeUiQuad> quads;
        const auto add = [&](f2::NativeUiAsset asset, float x0, float y0, float x1, float y1,
                             float u0, float v0, float u1, float v1, std::uint32_t color) {
            const auto& texture = ui_textures_[ui_slot(asset)];
            if (!texture.texture) return;
            quads.push_back({texture.gpu, x0, y0, x1, y1, u0, v0, u1, v1, color});
        };
        const auto alpha = [](float value) {
            return static_cast<std::uint32_t>(std::clamp(value, 0.0f, 1.0f) * 255.0f);
        };
        const auto rgba = [](std::uint32_t r, std::uint32_t g, std::uint32_t b,
                             std::uint32_t a) {
            return r | (g << 8u) | (b << 16u) | (a << 24u);
        };
        const auto add_native_text = [&](std::string_view text, float x, float y,
                                         float size, std::uint32_t color) {
            auto* font = ImGui::GetFont();
            if (!font || !font->ContainerAtlas || font->ContainerAtlas->TexID == 0 ||
                font->FontSize <= 0.0f) {
                return;
            }
            D3D12_GPU_DESCRIPTOR_HANDLE font_texture{};
            font_texture.ptr = static_cast<UINT64>(font->ContainerAtlas->TexID);
            const float scale = size / font->FontSize;
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
                const auto* glyph = font->FindGlyph(static_cast<ImWchar>(codepoint));
                if (!glyph) continue;
                if (glyph->Visible) {
                    quads.push_back({font_texture, cursor + glyph->X0 * scale,
                                     y + glyph->Y0 * scale, cursor + glyph->X1 * scale,
                                     y + glyph->Y1 * scale, glyph->U0, glyph->V0,
                                     glyph->U1, glyph->V1, color});
                }
                cursor += glyph->AdvanceX * scale;
            }
        };

        const auto& background = ui_textures_[ui_textures_[ui_slot(f2::NativeUiAsset::MainBackground)].texture
                                                   ? ui_slot(f2::NativeUiAsset::MainBackground)
                                                   : ui_slot(f2::NativeUiAsset::TitleBackground)];
        if (background.texture) {
            const float scale = height_ / static_cast<float>(background.height);
            const float image_width = background.width * scale;
            const float pan_scale = width_ / 1280.0f;
            const float offset = std::fmod(width_ * 0.78125f +
                                               static_cast<float>(game_.frontend.state_time()) *
                                                   29.0f * pan_scale,
                                           image_width);
            for (float x = -offset; x < static_cast<float>(width_); x += image_width) {
                quads.push_back({background.gpu, x, 0.0f, x + image_width,
                                 static_cast<float>(height_), 0.0f, 0.0f, 1.0f, 1.0f,
                                 rgba(255, 255, 255, 255)});
            }
        } else {
            add(f2::NativeUiAsset::MenuSurface, 0.0f, 0.0f, width_ * 0.16f,
                static_cast<float>(height_), 0.0f, 0.0f, 1.0f, 1.0f,
                rgba(255, 255, 255, 245));
            add(f2::NativeUiAsset::MenuSurface, width_ * 0.84f, 0.0f,
                static_cast<float>(width_), static_cast<float>(height_),
                0.0f, 0.0f, 1.0f, 1.0f, rgba(255, 255, 255, 245));
        }

        const auto& frame_elements = ui_textures_[ui_slot(f2::NativeUiAsset::FrameElements)];
        const float menu_unit_scale = width_ / 1280.0f;
        // Projected from the retail sprite shader at the reference 1280x720
        // presentation size. Rows are three-sliced, 450x68, and their left
        // edge follows the curved slot's x coordinate.
        const float row_x = 177.0f * menu_unit_scale;
        const float row_y = 160.0f * menu_unit_scale;
        const float row_width = 450.0f * menu_unit_scale;
        const float row_height = 68.0f * menu_unit_scale;
        const float slice_width = 52.0f * menu_unit_scale;
        const auto add_three_slice = [&](f2::NativeUiAsset asset, float x0, float y0,
                                         float x1, float y1, float v0, float v1,
                                         std::uint32_t color, bool key_black_matte) {
            const auto add_slice = [&](float sx0, float sy0, float sx1, float sy1,
                                       float su0, float sv0, float su1, float sv1) {
                const auto before = quads.size();
                add(asset, sx0, sy0, sx1, sy1, su0, sv0, su1, sv1, color);
                if (key_black_matte && quads.size() != before) {
                    quads.back().key_black_matte = true;
                }
            };
            const float center_x0 = x0 + slice_width;
            const float center_x1 = x1 - slice_width;
            add_slice(x0, y0, center_x0, y1, 0.0f, v0, 0.125f, v1);
            add_slice(center_x0, y0, center_x1, y1, 0.125f, v0, 0.813f, v1);
            add_slice(center_x1, y0, x1, y1, 0.820f, v0, 0.945f, v1);
        };
        const auto& ability_elements = ui_textures_[ui_slot(f2::NativeUiAsset::AbilityElements)];
        const auto& menu_surface = ui_textures_[ui_slot(f2::NativeUiAsset::MenuSurface)];
        const auto add_body_three_slice = [&](float x0, float y0, float x1, float y1,
                                              std::uint32_t color) {
            if (!ability_elements.texture || !menu_surface.texture) return;
            const float body_width = x1 - x0;
            const float body_slice_width = body_width * (52.0f / 452.0f);
            const auto add_body_slice = [&](float sx0, float sx1,
                                            float su0, float su1) {
                const auto before = quads.size();
                add(f2::NativeUiAsset::AbilityElements, sx0, y0, sx1, y1,
                    su0, 96.0f / 512.0f, su1, 152.0f / 512.0f, color);
                if (quads.size() == before) return;
                auto& body = quads.back();
                body.detail_texture = menu_surface.gpu;
                body.detail_u0 = (sx0 - x0) / body_width;
                body.detail_v0 = 0.0f;
                body.detail_u1 = (sx1 - x0) / body_width;
                body.detail_v1 = 1.0f;
                body.combine_detail = true;
            };
            const float center_x0 = x0 + body_slice_width;
            const float center_x1 = x1 - body_slice_width;
            add_body_slice(x0, center_x0, 12.0f / 512.0f, 64.0f / 512.0f);
            add_body_slice(center_x0, center_x1, 64.0f / 512.0f,
                           412.0f / 512.0f);
            add_body_slice(center_x1, x1, 412.0f / 512.0f, 464.0f / 512.0f);
        };
        const std::size_t selected_index = game_.frontend.selected_item();
        const std::size_t previous_selected_index = game_.frontend.previous_selected_item();
        const float selection_t = std::clamp(
            static_cast<float>(game_.frontend.selection_time() / 0.15), 0.0f, 1.0f);
        const bool selection_animating = game_.frontend.selection_animating();
        bool selected_prompt_ready = false;
        float selected_prompt_x = 0.0f;
        float selected_prompt_y = 0.0f;
        std::uint32_t selected_prompt_color = 0;
        struct RetailDrawRow {
            std::string label;
            float x = 0.0f;
            float y = 0.0f;
            float width = 0.0f;
            float height = 0.0f;
            float scale = 1.0f;
            std::uint32_t color = 0;
        };
        std::vector<RetailDrawRow> draw_rows;
        for (std::size_t index = 0; index < game_.frontend.menu_items().size(); ++index) {
            const RetailMenuSlot* target_slot = retail_menu_slot(index, selected_index);
            const RetailMenuSlot* source_slot = retail_menu_slot(index, previous_selected_index);
            if (!target_slot || (selection_animating && !source_slot)) continue;
            const bool selected = index == game_.frontend.selected_item();
            const RetailMenuSlot& from = selection_animating ? *source_slot : *target_slot;
            const auto lerp = [selection_t](float a, float b) {
                return a + (b - a) * selection_t;
            };
            const float slot_x = lerp(from.x, target_slot->x);
            const float slot_y = lerp(from.y, target_slot->y);
            const float slot_opacity = lerp(from.opacity, target_slot->opacity);
            const float slot_scale = lerp(from.scale, target_slot->scale);
            const float draw_width = row_width * slot_scale;
            const float draw_height = row_height * slot_scale;
            const float x = row_x + (slot_x - kRetailCenterSlotX) * menu_unit_scale;
            const float y = row_y + (kRetailCenterSlotY - slot_y) * menu_unit_scale +
                            (row_height - draw_height) * 0.5f;
            const float slot_alpha = slot_opacity / 100.0f;
            const auto row_color = rgba(255, 255, 255, alpha(slot_alpha));
            const auto& item = game_.frontend.menu_items()[index];
            draw_rows.push_back({item.label, x, y, draw_width, draw_height,
                                 slot_scale, row_color});
            if (selected && input_.using_controller_prompts() &&
                frame_elements.texture) {
                selected_prompt_ready = true;
                // MenuHighlight is a fixed component at the center slot. The
                // rows animate through it; the A bezel itself does not move or
                // refresh with the selected row.
                selected_prompt_x = row_x + 20.0f * menu_unit_scale;
                selected_prompt_y = row_y + 15.0f * menu_unit_scale;
                selected_prompt_color = row_color;
            }
        }
        // Render the recovered passes in their serialized order: all inner
        // brown layers, all frame rims, then all labels.
        for (const auto& row : draw_rows) {
            // Preserve the authored 452x56 body mask against the 484x80
            // frame region instead of stretching it to the full rim bounds.
            const float body_width = row.width * (452.0f / 484.0f);
            const float body_height = row.height * (56.0f / 80.0f);
            const float body_x0 = row.x + (row.width - body_width) * 0.5f;
            const float body_y0 = row.y + (row.height - body_height) * 0.5f;
            add_body_three_slice(body_x0, body_y0, body_x0 + body_width,
                                 body_y0 + body_height, row.color);
        }
        for (const auto& row : draw_rows) {
            if (!frame_elements.texture) continue;
            add_three_slice(f2::NativeUiAsset::FrameElements, row.x, row.y,
                            row.x + row.width, row.y + row.height,
                            4.0f / 512.0f, 84.0f / 512.0f, row.color, true);
        }
        for (const auto& row : draw_rows) {
            add_native_text(row.label, row.x + 92.0f * row.scale,
                            row.y + 18.0f * row.scale,
                            26.0f * menu_unit_scale * row.scale,
                            rgba(255, 224, 128, row.color >> 24u));
        }
        // Retail sequence order is: row slices, side panels, selected prompt.
        // The panels are deliberately an overlay: their curved inner edge must
        // occlude the left ends of the menu buttons.
        const auto& menu_frame = ui_textures_[ui_slot(f2::NativeUiAsset::MenuFrameOverlay)];
        const auto& left_upper = ui_textures_[ui_slot(f2::NativeUiAsset::MenuFrameLeftUpper)];
        const auto& left_lower = ui_textures_[ui_slot(f2::NativeUiAsset::MenuFrameLeftLower)];
        const auto& right_upper = ui_textures_[ui_slot(f2::NativeUiAsset::MenuFrameRightUpper)];
        const auto& right_lower = ui_textures_[ui_slot(f2::NativeUiAsset::MenuFrameRightLower)];
        const bool has_exact_menu_frame = left_upper.texture && left_lower.texture &&
                                           right_upper.texture && right_lower.texture;
        if (has_exact_menu_frame) {
            // These are the four serialized retail panel draws. Keeping the
            // source crops intact preserves the curved inner edge that is lost
            // when the panels are flattened into the old composite overlay.
            add(f2::NativeUiAsset::MenuFrameLeftUpper, -3.05f * menu_unit_scale,
                -3.95f * menu_unit_scale, 271.93f * menu_unit_scale,
                513.67f * menu_unit_scale, 0.0f, 0.0f, 0.531f, 1.0f,
                rgba(255, 255, 255, 255));
            add(f2::NativeUiAsset::MenuFrameLeftLower, -3.05f * menu_unit_scale,
                513.67f * menu_unit_scale, 271.93f * menu_unit_scale,
                723.95f * menu_unit_scale, 0.0f, 0.0f, 0.531f, 0.406f,
                rgba(255, 255, 255, 255));
            add(f2::NativeUiAsset::MenuFrameRightUpper, 1007.06f * menu_unit_scale,
                -4.95f * menu_unit_scale, 1283.56f * menu_unit_scale,
                515.53f * menu_unit_scale, 0.0f, 0.0f, 0.531f, 1.0f,
                rgba(255, 255, 255, 255));
            add(f2::NativeUiAsset::MenuFrameRightLower, 1007.06f * menu_unit_scale,
                515.53f * menu_unit_scale, 1283.56f * menu_unit_scale,
                726.98f * menu_unit_scale, 0.0f, 0.0f, 0.531f, 0.406f,
                rgba(255, 255, 255, 255));
        } else if (menu_frame.texture) {
            quads.push_back({menu_frame.gpu, 0.0f, 0.0f, static_cast<float>(width_),
                             static_cast<float>(height_), 0.0f, 0.0f, 1.0f, 1.0f,
                             rgba(255, 255, 255, 255)});
        }
        const auto& side_rail_atlas =
            ui_textures_[ui_slot(f2::NativeUiAsset::SideRailAtlas)];
        if (!has_exact_menu_frame && !menu_frame.texture && side_rail_atlas.texture) {
            // Fallback only when the serialized panel sources are unavailable.
            add(f2::NativeUiAsset::SideRailAtlas, width_ * 0.145f, 0.0f,
                width_ * 0.172f, static_cast<float>(height_), 0.0f, 0.0f,
                74.0f / 256.0f, 720.0f / 1024.0f,
                rgba(205, 145, 88, 245));
            add(f2::NativeUiAsset::SideRailAtlas, width_ * 0.828f, 0.0f,
                width_ * 0.855f, static_cast<float>(height_), 74.0f / 256.0f,
                0.0f, 150.0f / 256.0f, 720.0f / 1024.0f,
                rgba(205, 145, 88, 245));
        }
        if (selected_prompt_ready) {
            // MenuHighlight.rim_and_red is a frames-elements crop that contains
            // the metallic bezel and a red center. Retail covers that center
            // with the following green_top / green_top_translucent passes
            // (151305/151306), producing the selected A. The crop is larger
            // than the visible button; its transparent margins are part of the
            // serialized component and position the bezel around the glyph.
            const float source_prompt_width = 0.448f;
            const float source_rim_width = 1.024f;
            const float source_prompt_left_in_rim = 0.328f;
            // Draw 151304 bounds are z=.544..2.656 (height 2.112), while
            // draws 151305/151306 start at z=1.388. Preserve that recorded
            // bezel height and place its top relative to the green prompt's
            // top; shrinking the crop to the UV height makes the ring too
            // small even when its center is aligned.
            const float source_prompt_top_in_rim = 0.844f;
            const float source_prompt_bottom_in_rim = 1.268f;
            const float prompt_pixel_scale =
                (46.0f * menu_unit_scale) / source_prompt_width;
            const float rim_x0 = selected_prompt_x -
                                 source_prompt_left_in_rim * prompt_pixel_scale;
            const float rim_y0 = selected_prompt_y -
                                 source_prompt_top_in_rim * prompt_pixel_scale;
            const float rim_x1 = rim_x0 + source_rim_width * prompt_pixel_scale;
            const float rim_y1 = selected_prompt_y +
                                 source_prompt_bottom_in_rim * prompt_pixel_scale;
            add(f2::NativeUiAsset::FrameElements, rim_x0, rim_y0, rim_x1, rim_y1,
                0.0f, 0.171f, 0.25f, 0.687f, selected_prompt_color);
            const float prompt_y = selected_prompt_y - 2.0f * menu_unit_scale;
            add(f2::NativeUiAsset::FrameElements, selected_prompt_x, prompt_y,
                selected_prompt_x + 46.0f * menu_unit_scale,
                prompt_y + 46.0f * menu_unit_scale,
                8.0f / 512.0f, 369.0f / 512.0f,
                64.0f / 512.0f, 425.0f / 512.0f, selected_prompt_color);
            add(f2::NativeUiAsset::FrameElements, selected_prompt_x,
                prompt_y - 0.4f * menu_unit_scale,
                selected_prompt_x + 46.0f * menu_unit_scale,
                prompt_y + 45.6f * menu_unit_scale,
                8.0f / 512.0f, 369.0f / 512.0f,
                64.0f / 512.0f, 425.0f / 512.0f, selected_prompt_color);
        }
        if (game_.frontend.state() == f2::FrontendState::ChooseCard) {
            // choosecard is a modal layer in the retail front end: the menu and
            // panorama remain visible, but are pushed behind a translucent black
            // veil while the two cards stay crisp above it.
            const float card_fade = std::clamp(
                (static_cast<float>(game_.frontend.state_time()) - 0.04f) / 0.34f,
                0.0f, 1.0f);
            const float card_smooth = card_fade * card_fade * (3.0f - 2.0f * card_fade);
            add(f2::NativeUiAsset::MenuSurface, 0.0f, 0.0f, static_cast<float>(width_),
                static_cast<float>(height_), 0.0f, 0.0f, 1.0f, 1.0f,
                rgba(0, 0, 0, 76));
            const auto add_card = [&](f2::NativeUiAsset asset, float x0, float draw_width,
                                      float y_offset, float angle) {
                const auto& texture = ui_textures_[ui_slot(asset)];
                if (!texture.texture) return;
                const float card_height = 398.0f * menu_unit_scale;
                const float center_y = height_ * 0.502f + y_offset * menu_unit_scale;
                const float scaled_width = draw_width * menu_unit_scale;
                quads.push_back({texture.gpu, x0 * menu_unit_scale,
                                 center_y - card_height * 0.5f,
                                 x0 * menu_unit_scale + scaled_width,
                                 center_y + card_height * 0.5f, 0.0f, 0.0f, 1.0f, 1.0f,
                                 rgba(255, 255, 255,
                                      static_cast<std::uint32_t>(card_smooth * 255.0f)), angle});
            };
            add_card(f2::NativeUiAsset::CardBoy, 349.0f, 410.0f, -12.0f, -0.14f);
            add_card(f2::NativeUiAsset::CardGirl, 654.0f, 250.0f, -3.0f, 0.105f);

            // The retail card images are the pointer targets as well as the
            // visual choice. Keep these hit boxes tied to the measured draw
            // rectangles; selection itself remains in the controller so the
            // D3D12 and Vulkan front ends cannot diverge.
            const auto add_card_hitbox = [&](const char* id, bool girl, float x0,
                                             float draw_width, float y_offset) {
                const float card_height = 398.0f * menu_unit_scale;
                const float left = x0 * menu_unit_scale;
                const float top = height_ * 0.502f + y_offset * menu_unit_scale -
                                  card_height * 0.5f;
                ImGui::SetCursorScreenPos(ImVec2(left, top));
                if (ImGui::InvisibleButton(id,
                                           ImVec2(draw_width * menu_unit_scale, card_height))) {
                    game_.frontend.select_card(girl);
                }
            };
            add_card_hitbox("##choose_card_boy", false, 349.0f, 410.0f, -12.0f);
            add_card_hitbox("##choose_card_girl", true, 654.0f, 250.0f, -3.0f);
        }
        native_ui_renderer_.render(command_list, width_, height_, quads);
    }

    void render_native_choose_card(ID3D12GraphicsCommandList* command_list) {
        if (!native_ui_renderer_.ready()) return;
        std::vector<f2::NativeUiQuad> quads;
        const auto add_card = [&](f2::NativeUiAsset asset, float center_x, float angle) {
            const auto& texture = ui_textures_[ui_slot(asset)];
            if (!texture.texture) return;
            const float scale = width_ / 1280.0f;
            const float card_width = 256.0f * scale;
            const float card_height = 384.0f * scale;
            const float center_y = height_ * 0.502f;
            quads.push_back({texture.gpu, center_x * scale - card_width * 0.5f,
                             center_y - card_height * 0.5f,
                             center_x * scale + card_width * 0.5f,
                             center_y + card_height * 0.5f, 0.0f, 0.0f, 1.0f, 1.0f,
                             0xffffffffu, angle});
        };
        // These are the measured retail card centers and tilts from the captured
        // choosecard screen at the reference 1280x720 presentation size.
        add_card(f2::NativeUiAsset::CardBoy, 470.0f, -0.14f);
        add_card(f2::NativeUiAsset::CardGirl, 781.0f, 0.105f);
        native_ui_renderer_.render(command_list, width_, height_, quads);
    }

    void render_native_title(ID3D12GraphicsCommandList* command_list) {
        if (!native_ui_renderer_.ready()) return;
        std::vector<f2::NativeUiQuad> quads;
        const auto add = [&](f2::NativeUiAsset asset, float x0, float y0, float x1, float y1,
                             float u0, float v0, float u1, float v1, std::uint32_t color) {
            const auto& texture = ui_textures_[ui_slot(asset)];
            if (!texture.texture) return;
            quads.push_back({texture.gpu, x0, y0, x1, y1, u0, v0, u1, v1, color});
        };
        const auto alpha = [](float value) {
            return static_cast<std::uint32_t>(std::clamp(value, 0.0f, 1.0f) * 255.0f);
        };
        const auto rgba = [](std::uint32_t r, std::uint32_t g, std::uint32_t b,
                             std::uint32_t a) {
            return r | (g << 8u) | (b << 16u) | (a << 24u);
        };
        const float title_time = static_cast<float>(game_.frontend.state_time());
        const auto& background = ui_textures_[ui_slot(f2::NativeUiAsset::TitleBackground)];
        if (background.texture) {
            const float fade = std::clamp((title_time - 5.90f) / 1.10f, 0.0f, 1.0f);
            const float scale = height_ / static_cast<float>(background.height);
            const float image_width = background.width * scale;
            const float offset = std::fmod(title_time * 29.0f, image_width);
            for (float x = -offset; x < static_cast<float>(width_); x += image_width) {
                quads.push_back({background.gpu, x, 0.0f, x + image_width,
                                 static_cast<float>(height_), 0.0f, 0.0f, 1.0f, 1.0f,
                                 rgba(255, 255, 255, alpha(fade))});
            }
        }
        const float logo_fade = std::clamp((title_time - 0.86f) / 0.75f, 0.0f, 1.0f);
        const auto& logo = ui_textures_[ui_slot(f2::NativeUiAsset::Logo)];
        if (logo.texture && logo_fade > 0.0f) {
            const float logo_scale = width_ / (logo.width * 1.8f);
            const float logo_width = logo.width * logo_scale;
            const float logo_height = logo.height * logo_scale;
            const float logo_x = (width_ - logo_width) * 0.5f;
            const float logo_y = height_ * 0.47f - logo_height * 0.5f;
            add(f2::NativeUiAsset::Logo, logo_x, logo_y,
                logo_x + logo_width, logo_y + logo_height,
                0.0f, 0.0f, 1.0f, 1.0f, rgba(255, 255, 255, alpha(logo_fade)));
        }
        const auto& baseline = ui_textures_[ui_slot(f2::NativeUiAsset::AmbientBaseline)];
        if (baseline.texture && title_time >= 5.90f) {
            add(f2::NativeUiAsset::AmbientBaseline, 0.0f, 0.0f,
                static_cast<float>(width_), static_cast<float>(height_),
                0.0f, 0.0f, 1.0f, 1.0f, rgba(255, 255, 255, 255));
        }
        const auto& ambient_atlas = ui_textures_[ui_slot(f2::NativeUiAsset::AmbientAtlas)];
        if (ambient_atlas.texture && ambient_atlas.shader_read &&
            title_time >= 5.90f + 2.0f / 60.0f) {
            // This is the native equivalent of the recovered 12-draw sidecar
            // block. Keep the frame-relative interpolation identical to the
            // Vulkan/ImGui path so D3D12 does not introduce a second timing
            // interpretation for the same retail geometry.
            constexpr std::array<float, 10> slide_left_ndc = {
                -1.8962f, -1.6970f, -1.3355f, -1.1485f, -1.0475f,
                -0.9409f, -0.8442f, -0.7700f, -0.7247f, -0.7152f,
            };
            constexpr std::array<float, 4> row_bottom_ndc = {
                -0.1018f, 0.0605f, 0.2229f, 0.3936f,
            };
            constexpr std::array<float, 4> row_top_ndc = {
                0.0236f, 0.1859f, 0.3483f, 0.5190f,
            };
            constexpr std::array<float, 3> u0 = {0.740f, 0.871f, 0.875f};
            constexpr std::array<float, 3> u1 = {0.865f, 0.873f, 1.000f};
            constexpr float start_time = 5.90f + 2.0f / 60.0f;
            constexpr float final_left = -0.7152f;
            constexpr float middle_right = -0.0746f;
            constexpr float right_edge = -0.0371f;
            if (title_time >= start_time) {
                const float sample = std::clamp((title_time - start_time) * 60.0f,
                                                0.0f,
                                                static_cast<float>(slide_left_ndc.size() - 1));
                const auto lower = static_cast<std::size_t>(std::floor(sample));
                const auto upper = std::min(lower + 1, slide_left_ndc.size() - 1);
                const float fraction = sample - static_cast<float>(lower);
                const float left_ndc = std::lerp(slide_left_ndc[lower],
                                                 slide_left_ndc[upper], fraction);
                const float shift_ndc = left_ndc - final_left;
                const float middle_left = final_left + 0.0375f + shift_ndc;
                const float right_left = middle_right + shift_ndc;
                const std::array<float, 3> x0 = {left_ndc, middle_left, right_left};
                const std::array<float, 3> x1 = {
                    middle_left, right_left, right_edge + shift_ndc,
                };
                const auto to_x = [this](float ndc) {
                    return (ndc + 1.0f) * static_cast<float>(width_) * 0.5f;
                };
                const auto to_y = [this](float ndc) {
                    return (1.0f - ndc) * static_cast<float>(height_) * 0.5f;
                };
                for (std::size_t row = 0; row < row_bottom_ndc.size(); ++row) {
                    for (std::size_t column = 0; column < x0.size(); ++column) {
                        add(f2::NativeUiAsset::AmbientAtlas,
                            to_x(x0[column]), to_y(row_top_ndc[row]),
                            to_x(x1[column]), to_y(row_bottom_ndc[row]),
                            u0[column], 0.250f, u1[column], 0.500f,
                            rgba(255, 255, 255, 255));
                    }
                }
            }
        }
        native_ui_renderer_.render(command_list, width_, height_, quads);
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
        ImGui_ImplDX12_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        const auto state = game_.frontend.state();
        if (state == f2::FrontendState::Boot || state == f2::FrontendState::IntroVideo ||
            state == f2::FrontendState::AttractVideo) {
            ImGui::SetNextWindowPos(ImVec2(0, 0));
            ImGui::SetNextWindowSize(ImVec2(static_cast<float>(width_), static_cast<float>(height_)));
            ImGui::Begin("##intro", nullptr,
                         ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoSavedSettings);
            if ((state == f2::FrontendState::IntroVideo ||
                 state == f2::FrontendState::AttractVideo) && video_texture_) {
                ImGui::SetCursorPos(ImVec2(0, 0));
                ImGui::Image(static_cast<ImTextureID>(video_gpu_handle_.ptr),
                             ImVec2(static_cast<float>(width_), static_cast<float>(height_)));
            }
            ImGui::End();
        } else if (state == f2::FrontendState::Title) {
            ImGui::SetNextWindowPos(ImVec2(0, 0));
            ImGui::SetNextWindowSize(ImVec2(static_cast<float>(width_), static_cast<float>(height_)));
            const bool native_title_visuals = native_ui_renderer_.ready();
            if (native_title_visuals) ImGui::SetNextWindowBgAlpha(0.0f);
            ImGui::Begin("##title", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoSavedSettings);
            auto* draw_list = ImGui::GetWindowDrawList();
            const auto& background = ui_textures_[0];
            const float title_time = static_cast<float>(game_.frontend.state_time());
            const auto alpha = [](float value) {
                return static_cast<int>(std::clamp(value, 0.0f, 1.0f) * 255.0f);
            };
            if (!native_title_visuals) {
                draw_list->AddRectFilled(ImVec2(0, 0), ImVec2(width_, height_), IM_COL32(0, 0, 0, 255));
            }
            if (!native_title_visuals && background.texture) {
                // The independent startup reference keeps the title card
                // black/grey for about six seconds after the logo reveal;
                // the panorama then fades in over roughly one second.
                const float fade = std::clamp((title_time - 5.90f) / 1.10f, 0.0f, 1.0f);
                const float scale = height_ / static_cast<float>(background.height);
                const float image_width = background.width * scale;
                // Measured against the captured native title sequence: the
                // panorama advances roughly 116 px every four seconds at the
                // reference 720 px presentation height.
                const float offset = std::fmod(title_time * 29.0f, image_width);
                for (float x = -offset; x < static_cast<float>(width_); x += image_width) {
                    draw_list->AddImage(static_cast<ImTextureID>(background.gpu.ptr),
                                        ImVec2(x, 0), ImVec2(x + image_width, height_),
                                        ImVec2(0, 0), ImVec2(1, 1),
                                        IM_COL32(255, 255, 255, alpha(fade)));
                }
            }
            const float logo_fade = std::clamp((title_time - 0.86f) / 0.75f, 0.0f, 1.0f);
            if (ui_textures_[1].texture && logo_fade > 0.0f) {
                // This is the authored splash proportion used by the local
                // reference implementation: one logo width per 1.5 viewport
                // widths, with the vertical anchor measured from the native
                // title capture.
                const float logo_scale = width_ /
                                         (ui_textures_[1].width * 1.8f);
                const float logo_width = ui_textures_[1].width * logo_scale;
                const float logo_height = ui_textures_[1].height * logo_scale;
                const float logo_x = (width_ - logo_width) * 0.5f;
                const float logo_y = height_ * 0.47f - logo_height * 0.5f;
                std::array<ImTextureID, 8> sparkle_textures{};
                for (std::size_t index = 0; index < sparkle_textures.size(); ++index) {
                    const auto& sparkle = ui_textures_[ui_slot(f2::NativeUiAsset::Sparkle1) + index];
                    if (sparkle.texture && sparkle.shader_read) {
                        sparkle_textures[index] = static_cast<ImTextureID>(sparkle.gpu.ptr);
                    }
                }
                const auto& ambient_atlas =
                    ui_textures_[ui_slot(f2::NativeUiAsset::AmbientAtlas)];
                const auto& ambient_baseline =
                    ui_textures_[ui_slot(f2::NativeUiAsset::AmbientBaseline)];
                const bool native_atlas_visuals =
                    native_title_visuals && ambient_atlas.texture && ambient_atlas.shader_read &&
                    title_time >= 5.90f + 2.0f / 60.0f;
                if (!native_title_visuals && ambient_baseline.texture && ambient_baseline.shader_read && title_time >= 5.90f) {
                    draw_list->AddImage(
                        static_cast<ImTextureID>(ambient_baseline.gpu.ptr), ImVec2(0, 0),
                        ImVec2(static_cast<float>(width_), static_cast<float>(height_)),
                        ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, 255));
                }
                const bool atlas_visuals = ambient_atlas.texture && ambient_atlas.shader_read &&
                                           title_time >= 5.90f + 2.0f / 60.0f;
                if (!native_title_visuals && atlas_visuals) {
                    logo_sparkles_.draw_captured_atlas_slices(
                        draw_list, static_cast<ImTextureID>(ambient_atlas.gpu.ptr),
                        static_cast<float>(width_), static_cast<float>(height_), title_time, 1.0f);
                } else if (!native_atlas_visuals && !atlas_visuals &&
                           (title_time < 5.90f || !ambient_baseline.texture ||
                            !ambient_baseline.shader_read)) {
                    logo_sparkles_.draw_ambient(
                        draw_list, static_cast<ImTextureID>(ui_textures_[1].gpu.ptr), logo_x, logo_y,
                        logo_width, logo_height, title_time, 1.0f);
                }
                if (!atlas_visuals) {
                    logo_sparkles_.draw(draw_list, *ui_assets_.texture(f2::NativeUiAsset::Logo),
                                        sparkle_textures, logo_x, logo_y, logo_width, logo_height,
                                        title_time, logo_fade);
                }
                if (!native_title_visuals) {
                    draw_list->AddImage(static_cast<ImTextureID>(ui_textures_[1].gpu.ptr),
                                        ImVec2(logo_x, logo_y), ImVec2(logo_x + logo_width,
                                                                         logo_y + logo_height),
                                        ImVec2(0, 0), ImVec2(1, 1),
                                        IM_COL32(255, 255, 255, alpha(logo_fade)));
                }
            } else if (!ui_textures_[1].texture) {
                draw_list->AddText(ImGui::GetFont(), 92.0f,
                                   ImVec2(width_ * 0.24f, height_ * 0.32f),
                                   IM_COL32(255, 255, 255, alpha(logo_fade)), "FABLE II");
            }
            const auto prompt = input_.prompt(f2::NativeInputAction::Accept);
            const std::string prompt_text = "Press " + prompt + " to start";
            auto* title_font = ImGui::GetFont();
            constexpr float prompt_font_size = 22.0f;
            constexpr float legal_font_size = 18.0f;
            const auto measure_title_text = [&](const char* text, float font_size) {
                return title_font->CalcTextSizeA(font_size, width_, 0.0f, text);
            };
            const auto prompt_size = measure_title_text(prompt_text.c_str(), prompt_font_size);
            const float prompt_in = std::clamp((title_time - 0.12f) / 0.38f, 0.0f, 1.0f);
            const float prompt_out = 1.0f - std::clamp((title_time - 4.75f) / 0.45f, 0.0f, 1.0f);
            const int prompt_alpha = alpha(prompt_in * prompt_out);
            const float prompt_y = height_ * 0.56f;
            const auto& accept_texture = ui_textures_[ui_slot(f2::NativeUiAsset::Accept)];
            const bool use_accept_glyph = input_.using_controller_prompts() &&
                                          accept_texture.texture && accept_texture.shader_read;
            if (use_accept_glyph && prompt == "A") {
                const float icon_size = 28.0f;
                const auto prefix_size = measure_title_text("Press ", prompt_font_size);
                const auto suffix_size = measure_title_text(" to start", prompt_font_size);
                const float prompt_width = prefix_size.x + icon_size + suffix_size.x;
                const float prompt_x = (width_ - prompt_width) * 0.5f;
                const auto prompt_color = IM_COL32(235, 235, 235, prompt_alpha);
                const auto prompt_shadow = IM_COL32(0, 0, 0, prompt_alpha * 3 / 5);
                draw_list->AddText(title_font, prompt_font_size,
                                   ImVec2(prompt_x + 2.0f, prompt_y + 2.0f),
                                   prompt_shadow, "Press ");
                draw_list->AddText(title_font, prompt_font_size, ImVec2(prompt_x, prompt_y),
                                   prompt_color,
                                   "Press ");
                draw_list->AddImage(static_cast<ImTextureID>(accept_texture.gpu.ptr),
                                    ImVec2(prompt_x + prefix_size.x, prompt_y - 1.0f),
                                    ImVec2(prompt_x + prefix_size.x + icon_size,
                                           prompt_y - 1.0f + icon_size),
                                    ImVec2(0, 0), ImVec2(0.25f, 0.25f), prompt_color);
                draw_list->AddText(title_font, prompt_font_size,
                                   ImVec2(prompt_x + prefix_size.x + icon_size + 2.0f,
                                          prompt_y + 2.0f),
                                   prompt_shadow, " to start");
                draw_list->AddText(title_font, prompt_font_size,
                                   ImVec2(prompt_x + prefix_size.x + icon_size, prompt_y),
                                   prompt_color, " to start");
            } else {
                const auto prompt_pos = ImVec2((width_ - prompt_size.x) * 0.5f, prompt_y);
                draw_list->AddText(title_font, prompt_font_size,
                                   ImVec2(prompt_pos.x + 2.0f, prompt_pos.y + 2.0f),
                                   IM_COL32(0, 0, 0, prompt_alpha * 3 / 5), prompt_text.c_str());
                draw_list->AddText(title_font, prompt_font_size,
                                   prompt_pos,
                                   IM_COL32(235, 235, 235, prompt_alpha), prompt_text.c_str());
            }
            const float legal_in = std::clamp((title_time - 0.20f) / 0.45f, 0.0f, 1.0f);
            const float legal_out = 1.0f - std::clamp((title_time - 5.90f) / 0.65f, 0.0f, 1.0f);
            const int legal_alpha = alpha(legal_in * legal_out);
            const ImU32 legal_color = IM_COL32(242, 242, 242, legal_alpha);
            const auto draw_centered_legal = [&](const char* text, float y) {
                const auto measured = measure_title_text(text, legal_font_size);
                const auto position = ImVec2((width_ - measured.x) * 0.5f, y);
                draw_list->AddText(title_font, legal_font_size,
                                   ImVec2(position.x + 2.0f, position.y + 2.0f),
                                   IM_COL32(0, 0, 0, legal_alpha * 3 / 5), text);
                draw_list->AddText(title_font, legal_font_size, position, legal_color, text);
            };
            draw_centered_legal(
                "\xC2\xA9 & \xC2\xAE 2008 Microsoft Corporation. All rights reserved. Developed by",
                height_ * 0.73f);
            draw_centered_legal("Lionhead Studios.", height_ * 0.79f);
            draw_centered_legal("Online Interactions Not Rated by the ESRB", height_ * 0.87f);
            ImGui::SetCursorPos(ImVec2(0, 0));
            if (ImGui::InvisibleButton("##title_accept", ImVec2(static_cast<float>(width_),
                                                                  static_cast<float>(height_)))) {
                game_.frontend.dispatch(f2::FrontendAction::Accept);
            }
            ImGui::End();
        } else if (state == f2::FrontendState::MainMenu ||
                   state == f2::FrontendState::ChooseCard || state == f2::FrontendState::Options) {
            ImGui::SetNextWindowPos(ImVec2(0, 0));
            ImGui::SetNextWindowSize(ImVec2(static_cast<float>(width_), static_cast<float>(height_)));
            const bool native_menu_visuals = native_ui_renderer_.ready() &&
                                              (state == f2::FrontendState::MainMenu ||
                                               state == f2::FrontendState::ChooseCard);
            const bool native_menu_text = native_menu_visuals &&
                                          ImGui::GetIO().Fonts->TexID != 0;
            if ((state == f2::FrontendState::MainMenu ||
                 state == f2::FrontendState::ChooseCard) && native_menu_visuals) {
                ImGui::SetNextWindowBgAlpha(0.0f);
            }
            ImGui::Begin(state == f2::FrontendState::MainMenu ? "##main_menu" : "##options", nullptr,
                         ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoSavedSettings);
            auto* draw_list = ImGui::GetWindowDrawList();
            const auto alpha = [](float value) {
                return static_cast<int>(std::clamp(value, 0.0f, 1.0f) * 255.0f);
            };
            const float unit = width_ / 1280.0f;
            const auto& background = ui_textures_[4].texture ? ui_textures_[4] : ui_textures_[0];
            if (!native_menu_visuals && background.texture) {
                const float scale = height_ / static_cast<float>(background.height);
                const float image_width = background.width * scale;
                const float pan_scale = width_ / 1280.0f;
                const float offset = std::fmod(width_ * 0.78125f +
                                                   static_cast<float>(game_.frontend.state_time()) *
                                                       29.0f * pan_scale,
                                               image_width);
                for (float x = -offset; x < static_cast<float>(width_); x += image_width) {
                    draw_list->AddImage(static_cast<ImTextureID>(background.gpu.ptr),
                                        ImVec2(x, 0), ImVec2(x + image_width, height_));
                }
            } else if (!native_menu_visuals) {
                draw_list->AddRectFilled(ImVec2(0, 0), ImVec2(width_, height_), IM_COL32(8, 12, 22, 255));
                draw_list->AddRectFilled(ImVec2(0, 0), ImVec2(width_ * 0.46f, height_),
                                         IM_COL32(14, 24, 39, 255));
            }
            if (state == f2::FrontendState::Options && game_.frontend.options_page_open()) {
                const auto& options = game_.frontend.options_items();
                const std::size_t selected = options.empty()
                                                 ? std::size_t{0}
                                                 : std::min(game_.frontend.selected_item(),
                                                            options.size() - 1);
                const std::string_view page_id = options.empty()
                                                     ? std::string_view{}
                                                     : std::string_view(options[selected].id);
                const float page_x = width_ * 0.495f;
                const float page_w = width_ * 0.325f;
                const float page_y = 0.0f;
                const float page_h = height_;
                const auto& page_texture =
                    ui_textures_[ui_slot(f2::NativeUiAsset::FramesPageTexture)];
                const auto& motif_texture = ui_textures_[ui_slot(f2::NativeUiAsset::Motifs)];
                const auto& controller_texture =
                    ui_textures_[ui_slot(f2::NativeUiAsset::Accept)];
                const ImTextureID page_id_texture = static_cast<ImTextureID>(page_texture.gpu.ptr);
                const ImTextureID motif_id = static_cast<ImTextureID>(motif_texture.gpu.ptr);
                draw_list->AddRectFilled(ImVec2(page_x - 10.0f * unit, page_y),
                                         ImVec2(page_x + page_w + 10.0f * unit, page_h),
                                         IM_COL32(0, 0, 0, 110));
                if (page_texture.texture) {
                    draw_list->AddImage(page_id_texture, ImVec2(page_x, page_y),
                                        ImVec2(page_x + page_w, page_h), ImVec2(0, 0),
                                        ImVec2(1, 1), IM_COL32(255, 255, 255, 245));
                }
                const float center_x = page_x + page_w * 0.50f;
                const auto draw_centered = [&](const char* text, float y, float size, ImU32 color) {
                    const ImVec2 measured = ImGui::GetFont()->CalcTextSizeA(size, width_, 0.0f, text);
                    draw_list->AddText(ImGui::GetFont(), size,
                                       ImVec2(center_x - measured.x * 0.5f, y), color, text);
                };
                draw_centered(options.empty() ? "Options" : options[selected].label.c_str(),
                              height_ * 0.105f, 27.0f * unit, IM_COL32(105, 55, 27, 255));
                if (motif_texture.texture) {
                    draw_list->AddImage(motif_id,
                                        ImVec2(page_x + page_w * 0.09f, height_ * 0.165f),
                                        ImVec2(page_x + page_w * 0.91f, height_ * 0.195f),
                                        // The upper engraved rule is the first
                                        // serialized motif in motifs.tex. Crop
                                        // to the authored ink, not the empty
                                        // atlas padding around it.
                                        ImVec2(0.043f, 0.020f), ImVec2(0.72f, 0.070f),
                                        IM_COL32(133, 78, 28, 255));
                }
                const auto draw_arrows = [&](float y) {
                    const float arrow_w = 20.0f * unit;
                    const float arrow_h = 28.0f * unit;
                    const float left = page_x + page_w * 0.12f;
                    const float right = page_x + page_w * 0.88f;
                    draw_list->AddTriangleFilled(ImVec2(left + arrow_w, y),
                                                 ImVec2(left, y + arrow_h * 0.5f),
                                                 ImVec2(left + arrow_w, y + arrow_h),
                                                 IM_COL32(192, 107, 57, 235));
                    draw_list->AddTriangleFilled(ImVec2(right - arrow_w, y),
                                                 ImVec2(right, y + arrow_h * 0.5f),
                                                 ImVec2(right - arrow_w, y + arrow_h),
                                                 IM_COL32(192, 107, 57, 235));
                };
                const auto draw_value = [&](const char* label, const std::string& value, float y) {
                    draw_centered(label, y, 21.0f * unit, IM_COL32(105, 55, 27, 255));
                    draw_centered(value.c_str(), y + 34.0f * unit, 21.0f * unit,
                                  IM_COL32(24, 24, 24, 255));
                    draw_arrows(y + 32.0f * unit);
                };
                const auto draw_slider = [&](const char* label, int value, float y) {
                    draw_centered(label, y, 21.0f * unit, IM_COL32(105, 55, 27, 255));
                    const float x0 = page_x + page_w * 0.22f;
                    const float x1 = page_x + page_w * 0.78f;
                    const float bar_y = y + 34.0f * unit;
                    draw_list->AddRectFilled(ImVec2(x0, bar_y), ImVec2(x1, bar_y + 5.0f * unit),
                                             IM_COL32(43, 40, 45, 255), 3.0f * unit);
                    draw_list->AddRectFilled(ImVec2(x1 - 18.0f * unit * (value / 100.0f), bar_y),
                                             ImVec2(x1, bar_y + 5.0f * unit),
                                             IM_COL32(190, 105, 66, 255), 3.0f * unit);
                };
                if (page_id == "game") {
                    draw_value("Subtitles", game_.frontend.subtitles_enabled() ? "On" : "Off",
                               height_ * 0.235f);
                    draw_value("Glowing Trail Brightness",
                               game_.frontend.breadcrumb_size() == 0 ? "Off" :
                               game_.frontend.breadcrumb_size() == 1 ? "Medium" : "Bright",
                               height_ * 0.355f);
                    draw_value("Tutorials", game_.frontend.tutorial_boxes_enabled() ? "On" : "Off",
                               height_ * 0.475f);
                    draw_value("Online Orbs", game_.frontend.multiplayer_orbs_enabled()
                               ? "Friends Only" : "Off", height_ * 0.595f);
                    draw_value("Auto Joinable", game_.frontend.auto_joinable_enabled() ? "On" : "Off",
                               height_ * 0.715f);
                } else if (page_id == "controls") {
                    draw_value("Invert Aim", game_.frontend.invert_aim_enabled() ? "On" : "Off",
                               height_ * 0.235f);
                } else if (page_id == "audio") {
                    draw_slider("Sounds", game_.frontend.sounds_volume(), height_ * 0.255f);
                    draw_slider("Music", game_.frontend.music_volume(), height_ * 0.405f);
                    draw_slider("Voice", game_.frontend.voice_volume(), height_ * 0.555f);
                    draw_value("Speakers", game_.frontend.speaker_mode() == 0
                               ? "5.1 Surround" : "Stereo", height_ * 0.695f);
                } else if (page_id == "video") {
                    const auto& calibration_texture =
                        ui_textures_[ui_slot(f2::NativeUiAsset::CalibrationImage)];
                    if (calibration_texture.texture) {
                        const float image_min_x = page_x + page_w * 0.16f;
                        const float image_max_x = page_x + page_w * 0.86f;
                        const float image_min_y = height_ * 0.23f;
                        const float image_max_y = height_ * 0.55f;
                        draw_list->AddImage(
                            static_cast<ImTextureID>(calibration_texture.gpu.ptr),
                            ImVec2(image_min_x, image_min_y),
                            ImVec2(image_max_x, image_max_y), ImVec2(0, 0), ImVec2(1, 0.75f),
                            IM_COL32(255, 255, 255, 255));
                    }
                    const ImU32 selected_setting_color = IM_COL32(145, 72, 30, 255);
                    const ImU32 normal_setting_color = IM_COL32(105, 55, 27, 255);
                    draw_centered("Gamma", height_ * 0.575f, 21.0f * unit,
                                  game_.frontend.video_setting_row() == 0
                                      ? selected_setting_color : normal_setting_color);
                    draw_slider("", game_.frontend.gamma_percent(), height_ * 0.615f);
                    draw_centered("Adjust the gamma so that you are just", height_ * 0.685f,
                                  17.0f * unit, IM_COL32(35, 35, 35, 255));
                    draw_centered("able to see the text on the left side", height_ * 0.725f,
                                  17.0f * unit, IM_COL32(35, 35, 35, 255));
                    draw_centered("of the circular image.", height_ * 0.765f,
                                  17.0f * unit, IM_COL32(35, 35, 35, 255));
                    draw_centered("Display", height_ * 0.805f, 17.0f * unit,
                                  IM_COL32(105, 55, 27, 255));
                    const auto draw_display_value = [&](const char* label, const char* value,
                                                        float y, int row) {
                        const ImU32 color = game_.frontend.video_setting_row() == row
                                                ? selected_setting_color : normal_setting_color;
                        draw_list->AddText(ImGui::GetFont(), 16.0f * unit,
                                           ImVec2(page_x + page_w * 0.19f, y), color, label);
                        const ImVec2 measured = ImGui::GetFont()->CalcTextSizeA(
                            16.0f * unit, width_, 0.0f, value);
                        draw_list->AddText(ImGui::GetFont(), 16.0f * unit,
                                           ImVec2(page_x + page_w * 0.81f - measured.x, y),
                                           IM_COL32(24, 24, 24, 255), value);
                    };
                    const char* resolutions[] = {"1280 x 720", "1920 x 1080", "2560 x 1440"};
                    const char* anti_aliasing[] = {"Off", "2x", "4x", "8x"};
                    draw_display_value("Resolution", resolutions[game_.frontend.resolution_index()],
                                       height_ * 0.835f, 1);
                    draw_display_value("Anti-Aliasing", anti_aliasing[game_.frontend.anti_aliasing_index()],
                                       height_ * 0.875f, 2);
                }
                const bool has_controller_atlas = controller_texture.texture &&
                                                   controller_texture.shader_read;
                const auto draw_page_prompt = [&](const char* label, float y, float u0) {
                    const float icon_size = 27.0f * unit;
                    const float gap = 6.0f * unit;
                    const float text_size = 17.0f * unit;
                    const ImVec2 measured = ImGui::GetFont()->CalcTextSizeA(
                        text_size, width_, 0.0f, label);
                    const float group_width = icon_size + gap + measured.x;
                    const float right = page_x + page_w * 0.88f;
                    const float left = right - group_width;
                    const float text_x = left;
                    draw_list->AddText(ImGui::GetFont(), text_size,
                                       ImVec2(text_x + 1.0f * unit, y + 1.0f * unit),
                                       IM_COL32(0, 0, 0, 70), label);
                    draw_list->AddText(ImGui::GetFont(), text_size, ImVec2(text_x, y),
                                       IM_COL32(35, 35, 35, 255), label);
                    if (has_controller_atlas) {
                        const float icon_x = left + measured.x + gap;
                        draw_list->AddImage(
                            static_cast<ImTextureID>(controller_texture.gpu.ptr),
                            ImVec2(icon_x, y - 3.0f * unit),
                            ImVec2(icon_x + icon_size, y - 3.0f * unit + icon_size),
                            ImVec2(u0, 0.0f), ImVec2(u0 + 0.25f, 0.25f),
                            IM_COL32(255, 255, 255, 255));
                    }
                };
                draw_page_prompt("Cancel", height_ * 0.900f, 0.25f);
                draw_page_prompt("Accept", height_ * 0.950f, 0.0f);
            }
            const bool retail_root_menu =
                state == f2::FrontendState::MainMenu ||
                state == f2::FrontendState::ChooseCard ||
                state == f2::FrontendState::Options;
            if (retail_root_menu) {
                const auto& menu_frame = ui_textures_[ui_slot(f2::NativeUiAsset::MenuFrameOverlay)];
                const auto& left_upper = ui_textures_[ui_slot(f2::NativeUiAsset::MenuFrameLeftUpper)];
                const auto& left_lower = ui_textures_[ui_slot(f2::NativeUiAsset::MenuFrameLeftLower)];
                const auto& right_upper = ui_textures_[ui_slot(f2::NativeUiAsset::MenuFrameRightUpper)];
                const auto& right_lower = ui_textures_[ui_slot(f2::NativeUiAsset::MenuFrameRightLower)];
                const bool has_exact_menu_frame = left_upper.texture && left_lower.texture &&
                                                   right_upper.texture && right_lower.texture &&
                                                   left_upper.shader_read && left_lower.shader_read &&
                                                   right_upper.shader_read && right_lower.shader_read;
                const bool has_menu_frame = has_exact_menu_frame ||
                                             (menu_frame.texture && menu_frame.shader_read);
                const auto& frame_elements = ui_textures_[ui_slot(f2::NativeUiAsset::FrameElements)];
                const ImTextureID frame_texture = static_cast<ImTextureID>(frame_elements.gpu.ptr);
                const bool has_frame_elements = frame_elements.texture != nullptr;
                const float menu_unit_scale = width_ / 1280.0f;
                const float row_x = 177.0f * menu_unit_scale;
                const float row_y = 160.0f * menu_unit_scale;
                const float row_width = 450.0f * menu_unit_scale;
                const float row_height = 68.0f * menu_unit_scale;
                const auto& ability_elements = ui_textures_[ui_slot(f2::NativeUiAsset::AbilityElements)];
                const bool has_ability_elements = ability_elements.texture != nullptr;
                const float slice_width = 52.0f * menu_unit_scale;
                const auto add_three_slice_imgui = [&](ImTextureID texture,
                                                       const ImVec2& min,
                                                       const ImVec2& max,
                                                       float v0, float v1,
                                                       ImU32 color) {
                    const float center_min = min.x + slice_width;
                    const float center_max = max.x - slice_width;
                    draw_list->AddImage(texture, min,
                                        ImVec2(center_min, max.y),
                                        ImVec2(0.0f, v0), ImVec2(0.125f, v1), color);
                    draw_list->AddImage(texture,
                                        ImVec2(center_min, min.y),
                                        ImVec2(center_max, max.y),
                                        ImVec2(0.125f, v0), ImVec2(0.813f, v1), color);
                    draw_list->AddImage(texture,
                                        ImVec2(center_max, min.y), max,
                                         ImVec2(0.820f, v0),
                                         ImVec2(0.945f, v1), color);
                };
                const auto& displayed_items = state == f2::FrontendState::Options
                                                   ? game_.frontend.options_items()
                                                   : game_.frontend.menu_items();
                const std::size_t selected_index = game_.frontend.selected_item();
                const std::size_t previous_selected_index = game_.frontend.previous_selected_item();
                const float selection_t = std::clamp(
                    static_cast<float>(game_.frontend.selection_time() / 0.15), 0.0f, 1.0f);
                const bool selection_animating = game_.frontend.selection_animating();
                bool selected_prompt_ready = false;
                ImVec2 selected_prompt_min;
                ImVec2 selected_prompt_max;
                ImU32 selected_prompt_color = 0;
                for (std::size_t index = 0; index < displayed_items.size(); ++index) {
                    const RetailMenuSlot* target_slot = retail_menu_slot(index, selected_index);
                    const RetailMenuSlot* source_slot = retail_menu_slot(index, previous_selected_index);
                    if (!target_slot || (selection_animating && !source_slot)) continue;
                    const auto& item = displayed_items[index];
                    const bool selected = index == selected_index;
                    const RetailMenuSlot& from = selection_animating ? *source_slot : *target_slot;
                    const auto lerp = [selection_t](float a, float b) {
                        return a + (b - a) * selection_t;
                    };
                    const float slot_x = lerp(from.x, target_slot->x);
                    const float slot_y = lerp(from.y, target_slot->y);
                    const float slot_opacity = lerp(from.opacity, target_slot->opacity);
                    const float slot_scale = lerp(from.scale, target_slot->scale);
                    const float row_fade = 1.0f;
                    const float draw_width = row_width * slot_scale;
                    const float draw_height = row_height * slot_scale;
                    const ImVec2 row_position(
                        row_x + (slot_x - kRetailCenterSlotX) * menu_unit_scale,
                        row_y + (kRetailCenterSlotY - slot_y) * menu_unit_scale +
                            (row_height - draw_height) * 0.5f);
                    const float slot_alpha = slot_opacity / 100.0f;
                    const auto window_position = ImGui::GetWindowPos();
                    const ImVec2 row_min(window_position.x + row_position.x,
                                        window_position.y + row_position.y);
                    const ImVec2 row_max(row_min.x + draw_width, row_min.y + draw_height);
                    const ImU32 row_color = IM_COL32(255, 255, 255,
                                                      alpha(slot_alpha));
                    // The retail purple page supplies an opaque body behind
                    // these translucent rails. Our requested panorama replaces
                    // that page, so restore the same visual weight locally or
                    // the moving image shines through the button faces.
                    const ImVec2 body_min(row_min.x + 4.0f * menu_unit_scale,
                                          row_min.y + 4.0f * menu_unit_scale);
                    const ImVec2 body_max(row_max.x - 4.0f * menu_unit_scale,
                                          row_max.y - 4.0f * menu_unit_scale);
                    draw_list->AddRectFilled(
                        body_min, body_max,
                        selected ? IM_COL32(48, 31, 30, 205)
                                 : IM_COL32(25, 19, 28, 165),
                        22.0f * menu_unit_scale);
                    if (!native_menu_visuals && has_ability_elements) {
                        const ImVec2 inner_min(row_min.x - 1.5f * menu_unit_scale,
                                               row_min.y + 2.0f * menu_unit_scale);
                        const ImVec2 inner_max(row_max.x + 1.5f * menu_unit_scale,
                                               row_max.y - 1.5f * menu_unit_scale);
                        add_three_slice_imgui(static_cast<ImTextureID>(ability_elements.gpu.ptr),
                                              inner_min, inner_max, 0.0f,
                                              84.0f / 512.0f, row_color);
                    } else if (!native_menu_visuals && ui_textures_[5].texture) {
                        draw_list->AddImage(static_cast<ImTextureID>(ui_textures_[5].gpu.ptr),
                                            row_min, row_max, ImVec2(0, 0), ImVec2(1, 1),
                                                 IM_COL32(255, 255, 255,
                                                          alpha(row_fade * slot_alpha * 0.95f)));
                    } else if (!native_menu_visuals) {
                        draw_list->AddRectFilled(row_min, row_max,
                                                 selected ? IM_COL32(60, 43, 27,
                                                                     alpha(row_fade * slot_alpha))
                                                           : IM_COL32(19, 16, 15,
                                                                      alpha(row_fade * slot_alpha)),
                                                 24.0f);
                    }
                    if (!native_menu_visuals && !has_ability_elements) {
                        draw_list->AddRectFilled(row_min, row_max,
                                                 selected ? IM_COL32(60, 43, 27,
                                                                     alpha(row_fade * slot_alpha * 0.35f))
                                                           : IM_COL32(19, 16, 15,
                                                                      alpha(row_fade * slot_alpha * 0.25f)),
                                                 24.0f);
                        draw_list->AddRect(row_min, row_max,
                                           selected ? IM_COL32(223, 166, 91,
                                                               alpha(row_fade * slot_alpha))
                                                    : IM_COL32(139, 91, 48,
                                                               alpha(row_fade * slot_alpha)),
                                           24.0f, 0, selected ? 3.0f : 2.0f);
                    }
                    if (!native_menu_visuals && has_frame_elements) {
                        add_three_slice_imgui(frame_texture, row_min, row_max,
                                              4.0f / 512.0f, 84.0f / 512.0f,
                                              row_color);
                    }
                    ImGui::SetCursorPos(row_position);
                    if (ImGui::InvisibleButton(("##menu_" + item.id).c_str(),
                                               ImVec2(draw_width, draw_height))) {
                        game_.frontend.select_menu_item(item.id);
                        game_.frontend.dispatch(f2::FrontendAction::Accept);
                    }
                    if (ImGui::IsItemHovered() && !input_.using_controller_prompts()) {
                        game_.frontend.select_menu_item(item.id);
                    }
                    if (!native_menu_text) {
                        draw_list->AddText(ImGui::GetFont(),
                                           18.0f * menu_unit_scale * slot_scale,
                                           ImVec2(row_min.x + 62.0f * slot_scale,
                                                  row_min.y + 13.0f * slot_scale),
                                           selected ? IM_COL32(255, 226, 143,
                                                               alpha(row_fade * slot_alpha))
                                                    : IM_COL32(226, 195, 125,
                                                               alpha(row_fade * slot_alpha)),
                                           item.label.c_str());
                    }
                    if (selected && input_.using_controller_prompts() && has_frame_elements) {
                        if (!native_menu_visuals) {
                            selected_prompt_ready = true;
                            selected_prompt_min = ImVec2(row_min.x + 20.0f * menu_unit_scale,
                                                         row_min.y + 15.0f * menu_unit_scale);
                            selected_prompt_max = ImVec2(selected_prompt_min.x + 46.0f * menu_unit_scale,
                                                         selected_prompt_min.y + 46.0f * menu_unit_scale);
                            selected_prompt_color = row_color;
                        }
                    } else if (selected) {
                        draw_list->AddText(ImGui::GetFont(), 16.0f,
                                           ImVec2(row_min.x - 70.0f, row_min.y + 18.0f),
                                           IM_COL32(240, 220, 160, 255),
                                           input_.prompt(f2::NativeInputAction::Accept).c_str());
                    }
                }
                // Retail submits all row slices before the side panels. The
                // selected prompt is a final pass so the rail can occlude the
                // row without hiding the A prompt.
                if (!native_menu_visuals && has_exact_menu_frame) {
                    draw_list->AddImage(static_cast<ImTextureID>(left_upper.gpu.ptr),
                                        ImVec2(-3.05f * menu_unit_scale, -3.95f * menu_unit_scale),
                                        ImVec2(271.93f * menu_unit_scale, 513.67f * menu_unit_scale),
                                        ImVec2(0, 0), ImVec2(0.531f, 1.0f),
                                        IM_COL32(255, 255, 255, 255));
                    draw_list->AddImage(static_cast<ImTextureID>(left_lower.gpu.ptr),
                                        ImVec2(-3.05f * menu_unit_scale, 513.67f * menu_unit_scale),
                                        ImVec2(271.93f * menu_unit_scale, 723.95f * menu_unit_scale),
                                        ImVec2(0, 0), ImVec2(0.531f, 0.406f),
                                        IM_COL32(255, 255, 255, 255));
                    draw_list->AddImage(static_cast<ImTextureID>(right_upper.gpu.ptr),
                                        ImVec2(1007.06f * menu_unit_scale, -4.95f * menu_unit_scale),
                                        ImVec2(1283.56f * menu_unit_scale, 515.53f * menu_unit_scale),
                                        ImVec2(0, 0), ImVec2(0.531f, 1.0f),
                                        IM_COL32(255, 255, 255, 255));
                    draw_list->AddImage(static_cast<ImTextureID>(right_lower.gpu.ptr),
                                        ImVec2(1007.06f * menu_unit_scale, 515.53f * menu_unit_scale),
                                        ImVec2(1283.56f * menu_unit_scale, 726.98f * menu_unit_scale),
                                        ImVec2(0, 0), ImVec2(0.531f, 0.406f),
                                        IM_COL32(255, 255, 255, 255));
                } else if (!native_menu_visuals && has_menu_frame) {
                    draw_list->AddImage(static_cast<ImTextureID>(menu_frame.gpu.ptr),
                                        ImVec2(0, 0), ImVec2(width_, height_),
                                        ImVec2(0, 0), ImVec2(1, 1),
                                        IM_COL32(255, 255, 255, 255));
                }
                if (!native_menu_visuals && !has_menu_frame && ui_textures_[5].texture) {
                    draw_list->AddImage(static_cast<ImTextureID>(ui_textures_[5].gpu.ptr),
                                        ImVec2(0, 0), ImVec2(width_ * 0.16f, height_),
                                        ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, 245));
                    draw_list->AddImage(static_cast<ImTextureID>(ui_textures_[5].gpu.ptr),
                                        ImVec2(width_ * 0.84f, 0), ImVec2(width_, height_),
                                        ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, 245));
                }
                if (!native_menu_visuals && !has_menu_frame && has_frame_elements) {
                    const ImVec2 rail_uv_min(263.0f / 512.0f, 151.0f / 512.0f);
                    const ImVec2 rail_uv_max(320.0f / 512.0f, 512.0f / 512.0f);
                    draw_list->AddImage(frame_texture,
                                        ImVec2(width_ * 0.145f, 0),
                                        ImVec2(width_ * 0.185f, height_),
                                        rail_uv_min, rail_uv_max,
                                        IM_COL32(205, 145, 88, 245));
                    draw_list->AddImage(frame_texture,
                                        ImVec2(width_ * 0.815f, 0),
                                        ImVec2(width_ * 0.855f, height_),
                                        ImVec2(rail_uv_max.x, rail_uv_min.y),
                                        ImVec2(rail_uv_min.x, rail_uv_max.y),
                                        IM_COL32(205, 145, 88, 245));
                }
                if (selected_prompt_ready) {
                    // Match the serialized MenuHighlight composition: bezel
                    // first, then the green A glyph. The old flat glyph-only
                    // pass hid the bezel and made the prompt look undersized.
                    const float source_prompt_width = 0.448f;
                    const float source_rim_width = 1.024f;
                    const float source_prompt_left_in_rim = 0.328f;
                    const float source_prompt_top_in_rim = 0.844f;
                    const float source_prompt_bottom_in_rim = 1.268f;
                    const float prompt_pixel_scale =
                        (46.0f * menu_unit_scale) / source_prompt_width;
                    const float rim_x0 = selected_prompt_min.x -
                                         source_prompt_left_in_rim * prompt_pixel_scale;
                    const float rim_y0 = selected_prompt_min.y -
                                         source_prompt_top_in_rim * prompt_pixel_scale;
                    const float rim_x1 = rim_x0 + source_rim_width * prompt_pixel_scale;
                    const float rim_y1 = selected_prompt_min.y +
                                         source_prompt_bottom_in_rim * prompt_pixel_scale;
                    draw_list->AddImage(
                        frame_texture, ImVec2(rim_x0, rim_y0), ImVec2(rim_x1, rim_y1),
                        ImVec2(0.0f, 0.171f), ImVec2(0.25f, 0.687f), selected_prompt_color);
                    draw_list->AddImage(
                        frame_texture,
                        ImVec2(selected_prompt_min.x, selected_prompt_min.y -
                               2.0f * menu_unit_scale),
                        ImVec2(selected_prompt_max.x, selected_prompt_max.y -
                               2.0f * menu_unit_scale),
                        ImVec2(8.0f / 512.0f, 369.0f / 512.0f),
                        ImVec2(64.0f / 512.0f, 425.0f / 512.0f), selected_prompt_color);
                }
                if (state == f2::FrontendState::Options) {
                    const auto& menu_surface =
                        ui_textures_[ui_slot(f2::NativeUiAsset::MenuSurface)];
                    const ImTextureID surface_id = static_cast<ImTextureID>(menu_surface.gpu.ptr);
                    const float title_x = 220.0f * menu_unit_scale;
                    const float title_y = 53.0f * menu_unit_scale;
                    const float title_w = 390.0f * menu_unit_scale;
                    const float title_h = 47.0f * menu_unit_scale;
                    const auto draw_capsule = [&](const ImVec2& min, const ImVec2& max) {
                        const float slice = 52.0f * menu_unit_scale;
                        if (menu_surface.texture) {
                            draw_list->AddImage(surface_id, min,
                                                ImVec2(min.x + slice, max.y),
                                                ImVec2(0, 0), ImVec2(0.125f, 1));
                            draw_list->AddImage(surface_id, ImVec2(min.x + slice, min.y),
                                                ImVec2(max.x - slice, max.y),
                                                ImVec2(0.125f, 0), ImVec2(0.875f, 1));
                            draw_list->AddImage(surface_id, ImVec2(max.x - slice, min.y), max,
                                                ImVec2(0.875f, 0), ImVec2(1, 1));
                        }
                        if (has_frame_elements) {
                            add_three_slice_imgui(frame_texture, min, max,
                                                  4.0f / 512.0f, 84.0f / 512.0f,
                                                  IM_COL32(255, 255, 255, 255));
                        }
                    };
                    draw_capsule(ImVec2(title_x, title_y),
                                 ImVec2(title_x + title_w, title_y + title_h));
                    const char* title = "Options";
                    const float title_size = 27.0f * menu_unit_scale;
                    const ImVec2 title_measure = ImGui::GetFont()->CalcTextSizeA(
                        title_size, width_, 0.0f, title);
                    draw_list->AddText(ImGui::GetFont(), title_size,
                                       ImVec2(title_x + (title_w - title_measure.x) * 0.5f,
                                              title_y + 8.0f * menu_unit_scale),
                                       IM_COL32(238, 238, 238, 255), title);
                    const float footer_x = 220.0f * menu_unit_scale;
                    const float footer_y = 594.0f * menu_unit_scale;
                    const float footer_w = 390.0f * menu_unit_scale;
                    const float footer_h = 47.0f * menu_unit_scale;
                    draw_capsule(ImVec2(footer_x, footer_y),
                                 ImVec2(footer_x + footer_w, footer_y + footer_h));
                    const auto& controller_texture =
                        ui_textures_[ui_slot(f2::NativeUiAsset::Accept)];
                    const auto& gold_coin_texture =
                        ui_textures_[ui_slot(f2::NativeUiAsset::GoldCoin)];
                    if (controller_texture.texture && controller_texture.shader_read) {
                        // The recovered retail controller atlas is a 4x2 ABXY sheet:
                        // A is the top-left cell and B is the next cell.
                        draw_list->AddImage(
                            static_cast<ImTextureID>(controller_texture.gpu.ptr),
                            ImVec2(footer_x + 7.0f * menu_unit_scale,
                                   footer_y + 0.5f * menu_unit_scale),
                            ImVec2(footer_x + 53.0f * menu_unit_scale,
                                   footer_y + 46.5f * menu_unit_scale),
                            ImVec2(0.25f, 0.0f), ImVec2(0.50f, 0.25f),
                            IM_COL32(255, 255, 255, 255));
                    }
                    draw_list->AddText(ImGui::GetFont(), 22.0f * menu_unit_scale,
                                       ImVec2(footer_x + 58.0f * menu_unit_scale,
                                              footer_y + 10.0f * menu_unit_scale),
                                       IM_COL32(238, 238, 238, 255), "Back");
                    if (gold_coin_texture.texture && gold_coin_texture.shader_read) {
                        draw_list->AddImage(
                            static_cast<ImTextureID>(gold_coin_texture.gpu.ptr),
                            ImVec2(footer_x + 247.0f * menu_unit_scale,
                                   footer_y + 7.0f * menu_unit_scale),
                            ImVec2(footer_x + 279.0f * menu_unit_scale,
                                   footer_y + 39.0f * menu_unit_scale),
                            ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, 255));
                    }
                    draw_list->AddText(ImGui::GetFont(), 22.0f * menu_unit_scale,
                                       ImVec2(footer_x + 288.0f * menu_unit_scale,
                                              footer_y + 10.0f * menu_unit_scale),
                                       IM_COL32(245, 222, 65, 255), "5400");
                }
            } else {
                const auto& options = game_.frontend.options_items();
                const float unit = width_ / 1280.0f;
                if (!game_.frontend.options_page_open()) {
                    // Retail's OptionsMenu root is the opened book itself:
                    // purple embossed page, leather side leaves, one title
                    // capsule, and the four category rows anchored on the
                    // left.  Keep this pass independent from the custom
                    // category-page contents below.
                    const auto& page_texture =
                        ui_textures_[ui_slot(f2::NativeUiAsset::FramesPageTexture)];
                    const auto& overlay =
                        ui_textures_[ui_slot(f2::NativeUiAsset::MenuFrameOverlay)];
                    const auto& frame_elements =
                        ui_textures_[ui_slot(f2::NativeUiAsset::FrameElements)];
                    const auto& ability_elements =
                        ui_textures_[ui_slot(f2::NativeUiAsset::AbilityElements)];
                    const auto& menu_surface =
                        ui_textures_[ui_slot(f2::NativeUiAsset::MenuSurface)];
                    const ImTextureID frame_id =
                        static_cast<ImTextureID>(frame_elements.gpu.ptr);
                    const ImTextureID ability_id =
                        static_cast<ImTextureID>(ability_elements.gpu.ptr);
                    const ImTextureID surface_id =
                        static_cast<ImTextureID>(menu_surface.gpu.ptr);
                    const float book_left = width_ * 0.16f;
                    const float book_right = width_ * 0.84f;
                    draw_list->AddRectFilled(ImVec2(0, 0), ImVec2(width_, height_),
                                             IM_COL32(19, 8, 32, 255));
                    draw_list->AddRectFilled(ImVec2(book_left, 0),
                                             ImVec2(book_right, height_),
                                             IM_COL32(46, 20, 69, 255));
                    if (page_texture.texture) {
                        draw_list->AddImage(static_cast<ImTextureID>(page_texture.gpu.ptr),
                                            ImVec2(book_left, 0), ImVec2(book_right, height_),
                                            ImVec2(0, 0), ImVec2(1, 1),
                                            IM_COL32(135, 82, 165, 105));
                    }
                    if (overlay.texture) {
                        draw_list->AddImage(static_cast<ImTextureID>(overlay.gpu.ptr),
                                            ImVec2(0, 0), ImVec2(width_, height_),
                                            ImVec2(0, 0), ImVec2(1, 1),
                                            IM_COL32(255, 255, 255, 255));
                    }
                    const auto add_capsule = [&](const ImVec2& min, const ImVec2& max,
                                                 ImU32 color) {
                        const float slice = 52.0f * unit;
                        const float center_min = min.x + slice;
                        const float center_max = max.x - slice;
                        if (menu_surface.texture) {
                            draw_list->AddImage(surface_id, min, ImVec2(center_min, max.y),
                                                ImVec2(0, 0), ImVec2(0.125f, 1), color);
                            draw_list->AddImage(surface_id, ImVec2(center_min, min.y),
                                                ImVec2(center_max, max.y),
                                                ImVec2(0.125f, 0), ImVec2(0.875f, 1), color);
                            draw_list->AddImage(surface_id, ImVec2(center_max, min.y), max,
                                                ImVec2(0.875f, 0), ImVec2(1, 1), color);
                        }
                        if (frame_elements.texture) {
                            draw_list->AddImage(frame_id, min, ImVec2(center_min, max.y),
                                                ImVec2(0, 0), ImVec2(0.125f, 84.0f / 512.0f),
                                                color);
                            draw_list->AddImage(frame_id, ImVec2(center_min, min.y),
                                                ImVec2(center_max, max.y),
                                                ImVec2(0.125f, 0), ImVec2(0.813f, 84.0f / 512.0f),
                                                color);
                            draw_list->AddImage(frame_id, ImVec2(center_max, min.y), max,
                                                ImVec2(0.820f, 0), ImVec2(0.945f, 84.0f / 512.0f),
                                                color);
                        }
                    };
                    const float title_x = width_ * 0.175f;
                    const float title_y = height_ * 0.074f;
                    const float title_w = width_ * 0.305f;
                    const float title_h = height_ * 0.064f;
                    add_capsule(ImVec2(title_x, title_y),
                                ImVec2(title_x + title_w, title_y + title_h),
                                IM_COL32(255, 255, 255, 255));
                    draw_list->AddText(ImGui::GetFont(), 27.0f * unit,
                                       ImVec2(title_x + title_w * 0.39f, title_y + 8.0f * unit),
                                       IM_COL32(238, 238, 238, 255), "Options");
                    const float row_x = width_ * 0.151f;
                    const float row_y = height_ * 0.233f;
                    const float row_w = width_ * 0.332f;
                    const float row_h = height_ * 0.068f;
                    const float row_step = height_ * 0.085f;
                    const std::size_t selected = options.empty()
                                                     ? std::size_t{0}
                                                     : std::min(game_.frontend.selected_item(),
                                                                options.size() - 1);
                    for (std::size_t index = 0; index < options.size(); ++index) {
                        const bool highlighted = index == selected;
                        const ImVec2 row_min(row_x, row_y + row_step * static_cast<float>(index));
                        const ImVec2 row_max(row_min.x + row_w, row_min.y + row_h);
                        add_capsule(row_min, row_max, IM_COL32(255, 255, 255, 255));
                        if (highlighted && frame_elements.texture) {
                            const float prompt_size = 56.0f * unit;
                            const ImVec2 prompt_min(row_min.x, row_min.y - 4.0f * unit);
                            draw_list->AddImage(frame_id, prompt_min,
                                                ImVec2(prompt_min.x + prompt_size,
                                                       prompt_min.y + prompt_size),
                                                ImVec2(8.0f / 512.0f, 369.0f / 512.0f),
                                                ImVec2(64.0f / 512.0f, 425.0f / 512.0f),
                                                IM_COL32(255, 255, 255, 255));
                        }
                        draw_list->AddText(ImGui::GetFont(), 22.0f * unit,
                                           ImVec2(row_min.x + 66.0f * unit,
                                                  row_min.y + 11.0f * unit),
                                           highlighted ? IM_COL32(255, 226, 143, 255)
                                                      : IM_COL32(226, 195, 125, 255),
                                           options[index].label.c_str());
                        ImGui::SetCursorScreenPos(row_min);
                        if (ImGui::InvisibleButton(("##retail_option_" + options[index].id).c_str(),
                                                   ImVec2(row_w, row_h))) {
                            game_.frontend.select_menu_item(options[index].id);
                        }
                        if (ImGui::IsItemHovered() && !input_.using_controller_prompts()) {
                            game_.frontend.select_menu_item(options[index].id);
                        }
                    }
                    const float footer_x = width_ * 0.175f;
                    const float footer_y = height_ * 0.832f;
                    const float footer_w = width_ * 0.302f;
                    const float footer_h = height_ * 0.066f;
                    add_capsule(ImVec2(footer_x, footer_y),
                                ImVec2(footer_x + footer_w, footer_y + footer_h),
                                IM_COL32(255, 255, 255, 255));
                    if (frame_elements.texture) {
                        draw_list->AddImage(frame_id,
                                            ImVec2(footer_x + 7.0f * unit, footer_y + 5.0f * unit),
                                            ImVec2(footer_x + 53.0f * unit, footer_y + 51.0f * unit),
                                            ImVec2(8.0f / 512.0f, 184.0f / 512.0f),
                                            ImVec2(64.0f / 512.0f, 240.0f / 512.0f),
                                            IM_COL32(255, 255, 255, 255));
                    }
                    draw_list->AddText(ImGui::GetFont(), 22.0f * unit,
                                       ImVec2(footer_x + 58.0f * unit, footer_y + 12.0f * unit),
                                       IM_COL32(238, 238, 238, 255), "Back");
                } else {
                // Each category owns a real retail PageN in the BGF.  This
                // branch supplies our custom settings inside that page.
                const float page_x = width_ * 0.075f;
                const float page_y = height_ * 0.075f;
                const float page_w = width_ * 0.50f;
                const float page_h = height_ * 0.835f;
                const auto& page_texture =
                    ui_textures_[ui_slot(f2::NativeUiAsset::FramesPageTexture)];
                const auto& motif_texture = ui_textures_[ui_slot(f2::NativeUiAsset::Motifs)];
                const auto& slider_texture = ui_textures_[ui_slot(f2::NativeUiAsset::SliderFrame)];
                const ImTextureID page_id = static_cast<ImTextureID>(page_texture.gpu.ptr);
                const ImTextureID motif_id = static_cast<ImTextureID>(motif_texture.gpu.ptr);
                const ImTextureID slider_id = static_cast<ImTextureID>(slider_texture.gpu.ptr);
                const ImVec2 page_min(page_x, page_y);
                const ImVec2 page_max(page_x + page_w, page_y + page_h);
                draw_list->AddRectFilled(ImVec2(page_x - 8.0f * unit, page_y - 4.0f * unit),
                                         ImVec2(page_max.x + 8.0f * unit, page_max.y + 8.0f * unit),
                                         IM_COL32(0, 0, 0, 84), 10.0f * unit);
                if (page_texture.texture) {
                    draw_list->AddImage(page_id, page_min, page_max,
                                        ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, 238));
                } else {
                    draw_list->AddRectFilled(page_min, page_max, IM_COL32(224, 213, 179, 238));
                }
                if (motif_texture.texture) {
                    const ImVec2 motif_min(page_x + page_w * 0.10f, page_y + 18.0f * unit);
                    const ImVec2 motif_max(page_x + page_w * 0.90f, page_y + 37.0f * unit);
                    draw_list->AddImage(motif_id, motif_min, motif_max,
                                        ImVec2(0.02f, 0.0f), ImVec2(0.72f, 0.10f),
                                        IM_COL32(130, 78, 27, 210));
                }
                const auto& frame_elements = ui_textures_[ui_slot(f2::NativeUiAsset::FrameElements)];
                const auto& ability_elements = ui_textures_[ui_slot(f2::NativeUiAsset::AbilityElements)];

                const std::size_t selected_page = options.empty()
                                                       ? std::size_t{0}
                                                       : std::min(game_.frontend.selected_item(), options.size() - 1);
                const std::string_view selected_id = options.empty()
                                                         ? std::string_view{}
                                                         : std::string_view(options[selected_page].id);
                const char* selected_label = options.empty() ? "Options" : options[selected_page].label.c_str();
                draw_list->AddText(ImGui::GetFont(), 27.0f * unit,
                                   ImVec2(page_x + page_w * 0.09f, page_y + 46.0f * unit),
                                   IM_COL32(92, 54, 26, 255), selected_label);
                draw_list->AddLine(ImVec2(page_x + page_w * 0.10f, page_y + 82.0f * unit),
                                   ImVec2(page_x + page_w * 0.90f, page_y + 82.0f * unit),
                                   IM_COL32(137, 96, 47, 105), 1.0f * unit);

                const float content_x = page_x + page_w * 0.10f;
                const float content_y = page_y + 112.0f * unit;
                const float content_w = page_w * 0.80f;
                const float value_x = content_x + content_w * 0.67f;
                const auto draw_rule = [&](float y) {
                    draw_list->AddLine(ImVec2(content_x, y), ImVec2(content_x + content_w, y),
                                       IM_COL32(137, 96, 47, 105), 1.0f * unit);
                };
                const auto draw_option_row = [&](const char* label, const std::string& value,
                                                 float y, bool emphasized = false) {
                    draw_list->AddText(ImGui::GetFont(), 18.0f * unit,
                                       ImVec2(content_x, y),
                                       emphasized ? IM_COL32(91, 48, 20, 255)
                                                   : IM_COL32(106, 71, 37, 245), label);
                    draw_list->AddText(ImGui::GetFont(), 17.0f * unit,
                                       ImVec2(value_x, y), IM_COL32(126, 72, 22, 255),
                                       value.c_str());
                    draw_rule(y + 28.0f * unit);
                };
                const auto draw_slider = [&](const char* label, int value, float y) {
                    draw_list->AddText(ImGui::GetFont(), 18.0f * unit,
                                       ImVec2(content_x, y), IM_COL32(106, 71, 37, 245), label);
                    const float slider_x = value_x - 4.0f * unit;
                    const float slider_y = y + 3.0f * unit;
                    const float slider_w = content_w * 0.31f;
                    draw_list->AddRectFilled(ImVec2(slider_x, slider_y + 5.0f * unit),
                                             ImVec2(slider_x + slider_w, slider_y + 11.0f * unit),
                                             IM_COL32(117, 78, 34, 150), 3.0f * unit);
                    draw_list->AddRectFilled(ImVec2(slider_x, slider_y + 5.0f * unit),
                                             ImVec2(slider_x + slider_w * (value / 100.0f),
                                                    slider_y + 11.0f * unit),
                                             IM_COL32(159, 91, 27, 235), 3.0f * unit);
                    if (slider_texture.texture) {
                        draw_list->AddImage(slider_id,
                                            ImVec2(slider_x - 3.0f * unit, slider_y - 2.0f * unit),
                                            ImVec2(slider_x + slider_w + 3.0f * unit,
                                                   slider_y + 18.0f * unit),
                                            ImVec2(0.02f, 0.84f), ImVec2(0.97f, 0.98f),
                                            IM_COL32(255, 255, 255, 190));
                    }
                    draw_list->AddText(ImGui::GetFont(), 15.0f * unit,
                                       ImVec2(slider_x + slider_w + 10.0f * unit, y + 1.0f * unit),
                                       IM_COL32(126, 72, 22, 255),
                                       (std::to_string(value) + "%").c_str());
                    draw_rule(y + 28.0f * unit);
                };
                if (selected_id == "game") {
                    draw_option_row("Subtitles", game_.frontend.subtitles_enabled() ? "On" : "Off",
                                    content_y, true);
                    draw_option_row("Breadcrumb Size",
                                    game_.frontend.breadcrumb_size() == 0 ? "Off" :
                                    game_.frontend.breadcrumb_size() == 1 ? "Small" : "Large",
                                    content_y + 42.0f * unit);
                    draw_option_row("Tutorial Boxes",
                                    game_.frontend.tutorial_boxes_enabled() ? "On" : "Off",
                                    content_y + 84.0f * unit);
                    draw_option_row("Multiplayer Orbs",
                                    game_.frontend.multiplayer_orbs_enabled() ? "On" : "Off",
                                    content_y + 126.0f * unit);
                    draw_option_row("Auto-Joinable",
                                    game_.frontend.auto_joinable_enabled() ? "On" : "Off",
                                    content_y + 168.0f * unit);
                } else if (selected_id == "controls") {
                    draw_option_row("Invert Aim",
                                    game_.frontend.invert_aim_enabled() ? "On" : "Off",
                                    content_y, true);
                    draw_option_row("Accept", input_.prompt(f2::NativeInputAction::Accept),
                                    content_y + 52.0f * unit);
                    draw_option_row("Cancel", input_.prompt(f2::NativeInputAction::Back),
                                    content_y + 94.0f * unit);
                    draw_option_row("Move", input_.prompt(f2::NativeInputAction::Up) + " / " +
                                    input_.prompt(f2::NativeInputAction::Down),
                                    content_y + 136.0f * unit);
                } else if (selected_id == "video") {
                    draw_slider("Gamma", game_.frontend.gamma_percent(), content_y);
                    draw_option_row("Presentation", "Panoramic", content_y + 52.0f * unit);
                    draw_option_row("VSync", "Enabled", content_y + 94.0f * unit);
                } else if (selected_id == "audio") {
                    draw_slider("Sounds", game_.frontend.sounds_volume(), content_y);
                    draw_slider("Music", game_.frontend.music_volume(), content_y + 52.0f * unit);
                    draw_slider("Voice", game_.frontend.voice_volume(), content_y + 104.0f * unit);
                    draw_option_row("Speakers", game_.frontend.speaker_mode() == 0 ? "Stereo" : "Mono",
                                    content_y + 156.0f * unit);
                }

                // These are the twelve exact OptionsMenuY formatter slots:
                // positions are authored in the GUI script, with slot 4 as
                // the highlighted item and the surrounding opacity/scale
                // table supplied by g_MenuOpacityScaleSlots.
                struct OptionMenuSlot { float x; float y; float opacity; float scale; };
                static constexpr OptionMenuSlot kOptionMenuSlots[] = {
                    {0.0f, 85.0f, 0.00f, 0.65f}, {22.0f, 69.0f, 0.50f, 0.75f},
                    {42.0f, 15.0f, 0.75f, 0.85f}, {56.0f, -45.0f, 1.00f, 1.00f},
                    {56.0f, -106.0f, 1.00f, 1.00f}, {56.0f, -164.0f, 1.00f, 1.00f},
                    {56.0f, -222.0f, 1.00f, 1.00f}, {56.0f, -280.0f, 1.00f, 1.00f},
                    {50.0f, -338.0f, 1.00f, 1.00f}, {42.0f, -396.0f, 0.75f, 0.85f},
                    {22.0f, -454.0f, 0.50f, 0.75f}, {0.0f, -475.0f, 0.00f, 0.65f},
                };
                const float menu_origin_x = width_ * 0.61f;
                const float menu_origin_y = height_ * 0.38f;
                const float menu_w = width_ * 0.245f;
                const float menu_h = 48.0f * unit;
                const float frame_slice = 52.0f * unit;
                const auto add_ability_slice = [&](const ImVec2& min, const ImVec2& max, ImU32 color) {
                    if (!ability_elements.texture) return;
                    const float slice = 52.0f * unit;
                    const float center_min = min.x + slice;
                    const float center_max = max.x - slice;
                    const ImTextureID id = static_cast<ImTextureID>(ability_elements.gpu.ptr);
                    draw_list->AddImage(id, min, ImVec2(center_min, max.y),
                                        ImVec2(0.0f, 0.0f), ImVec2(0.125f, 84.0f / 512.0f), color);
                    draw_list->AddImage(id, ImVec2(center_min, min.y),
                                        ImVec2(center_max, max.y),
                                        ImVec2(0.125f, 0.0f), ImVec2(0.813f, 84.0f / 512.0f), color);
                    draw_list->AddImage(id, ImVec2(center_max, min.y), max,
                                        ImVec2(0.820f, 0.0f), ImVec2(0.945f, 84.0f / 512.0f), color);
                };
                const auto add_menu_slice = [&](const ImVec2& min, const ImVec2& max, ImU32 color) {
                    if (!frame_elements.texture) return;
                    const float center_min = min.x + frame_slice;
                    const float center_max = max.x - frame_slice;
                    const ImTextureID id = static_cast<ImTextureID>(frame_elements.gpu.ptr);
                    draw_list->AddImage(id, min, ImVec2(center_min, max.y),
                                        ImVec2(0.0f, 4.0f / 512.0f),
                                        ImVec2(0.125f, 84.0f / 512.0f), color);
                    draw_list->AddImage(id, ImVec2(center_min, min.y),
                                        ImVec2(center_max, max.y),
                                        ImVec2(0.125f, 4.0f / 512.0f),
                                        ImVec2(0.813f, 84.0f / 512.0f), color);
                    draw_list->AddImage(id, ImVec2(center_max, min.y), max,
                                        ImVec2(0.820f, 4.0f / 512.0f),
                                        ImVec2(0.945f, 84.0f / 512.0f), color);
                };
                for (std::size_t index = 0; index < options.size(); ++index) {
                    const int delta = static_cast<int>(index) - static_cast<int>(selected_page);
                    const int slot_index = 3 + delta;
                    if (slot_index < 0 || slot_index >= static_cast<int>(std::size(kOptionMenuSlots))) continue;
                    const auto& slot = kOptionMenuSlots[slot_index];
                    const bool selected = index == selected_page;
                    const float item_w = menu_w * slot.scale;
                    const float item_h = menu_h * slot.scale;
                    const ImVec2 center(menu_origin_x + slot.x * unit,
                                        menu_origin_y - slot.y * 0.78f * unit);
                    const ImVec2 item_min(center.x - item_w * 0.5f,
                                          center.y - item_h * 0.5f);
                    const ImVec2 item_max(center.x + item_w * 0.5f,
                                          center.y + item_h * 0.5f);
                    const int item_alpha = alpha(slot.opacity);
                    draw_list->AddRectFilled(item_min, item_max,
                                             selected ? IM_COL32(92, 58, 28, item_alpha)
                                                      : IM_COL32(24, 19, 14, item_alpha * 3 / 4),
                                             7.0f * unit);
                    add_ability_slice(ImVec2(item_min.x - 1.5f * unit, item_min.y + 2.0f * unit),
                                      ImVec2(item_max.x + 1.5f * unit, item_max.y - 1.5f * unit),
                                      IM_COL32(255, 255, 255, item_alpha));
                    add_menu_slice(item_min, item_max, IM_COL32(255, 255, 255, item_alpha));
                    const float text_x = item_min.x + (selected ? 66.0f : 24.0f) * unit * slot.scale;
                    if (selected && frame_elements.texture) {
                        const ImTextureID frame_id = static_cast<ImTextureID>(frame_elements.gpu.ptr);
                        const ImVec2 prompt_min(item_min.x + 14.0f * unit * slot.scale,
                                                item_min.y + 8.0f * unit * slot.scale);
                        const ImVec2 prompt_max(prompt_min.x + 46.0f * unit * slot.scale,
                                                prompt_min.y + 46.0f * unit * slot.scale);
                        draw_list->AddImage(frame_id, prompt_min, prompt_max,
                                            ImVec2(8.0f / 512.0f, 369.0f / 512.0f),
                                            ImVec2(64.0f / 512.0f, 425.0f / 512.0f),
                                            IM_COL32(255, 255, 255, item_alpha));
                    }
                    draw_list->AddText(ImGui::GetFont(), 18.0f * unit * slot.scale,
                                       ImVec2(text_x,
                                              item_min.y + 12.0f * unit * slot.scale),
                                       selected ? IM_COL32(255, 226, 143, item_alpha)
                                                : IM_COL32(226, 195, 125, item_alpha),
                                       options[index].label.c_str());
                    ImGui::SetCursorScreenPos(item_min);
                    if (ImGui::InvisibleButton(("##option_menu_" + options[index].id).c_str(),
                                               ImVec2(item_w, item_h))) {
                        game_.frontend.select_menu_item(options[index].id);
                    }
                    if (ImGui::IsItemHovered() && !input_.using_controller_prompts()) {
                        game_.frontend.select_menu_item(options[index].id);
                    }
                }
                draw_list->AddText(ImGui::GetFont(), 15.0f * unit,
                                   ImVec2(page_x + 28.0f * unit, page_max.y - 25.0f * unit),
                                   IM_COL32(98, 60, 28, 235),
                                     (input_.prompt(f2::NativeInputAction::Accept) +
                                     " DONE    " + input_.prompt(f2::NativeInputAction::Back) +
                                     " CANCEL").c_str());
                }
            }
            ImGui::End();
        } else if (state == f2::FrontendState::Loading) {
            ImGui::SetNextWindowPos(ImVec2(width_ * 0.5f, height_ * 0.5f),
                                    ImGuiCond_Always, ImVec2(0.5f, 0.5f));
            ImGui::Begin("##loading", nullptr, ImGuiWindowFlags_AlwaysAutoResize |
                         ImGuiWindowFlags_NoDecoration);
            ImGui::TextUnformatted("Loading native world...");
            ImGui::End();
        } else if (state == f2::FrontendState::World) {
            ImGui::SetNextWindowPos(ImVec2(16, 16));
            ImGui::Begin("Native world", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
            ImGui::TextUnformatted("Native D3D12 world renderer active.");
            ImGui::Text("Meshes: %zu  Instances: %zu", game_.scene.meshes.size(),
                        game_.scene.instances.size());
            ImGui::TextUnformatted("Geometry comes from a user-cooked F2SCENE package when supplied.");
            if (!game_.scene.materials.empty() && !game_.scene.materials[0].albedo.empty()) {
                ImGui::Text("Albedo: %s", game_.scene.materials[0].albedo.c_str());
            }
            ImGui::End();
        }

        ImGui::Render();
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
        }
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), command_list_.Get());
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
        ImGui_ImplDX12_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
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
