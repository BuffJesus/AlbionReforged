#include "f2/native_game.h"
#include "f2/native_install.h"
#include "f2/native_input.h"
#include "f2/native_ui.h"
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
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>

using Microsoft::WRL::ComPtr;

extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND window, UINT message,
                                               WPARAM wparam, LPARAM lparam);

namespace {

constexpr UINT kFrameCount = 2;
constexpr DXGI_FORMAT kBackBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr UINT kUiTextureCount = 6;

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
                                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
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
        ui_root_ = command_line_path(L"--ui-root").value_or(
            source_->data_root / "art" / "gui" / "native_ui");
        std::string ui_error;
        if (!ui_assets_.load(ui_root_, ui_error) && !ui_error.empty()) {
            MessageBoxA(window_, ui_error.c_str(), "Fable II Native - UI asset warning",
                        MB_OK | MB_ICONWARNING);
        }
        input_.load_bindings(source_config_path().parent_path() / "bindings.ini");
        if (command_line_flag(L"--skip-intro")) {
            game_.frontend.dispatch(f2::FrontendAction::Skip);
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

        ShowWindow(window_, SW_SHOWDEFAULT);
        UpdateWindow(window_);
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui::StyleColorsDark();
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
            draw();
            if (game_.frontend.quit_requested()) PostMessageA(window_, WM_CLOSE, 0, 0);
        }
        shutdown();
        return static_cast<int>(message.wParam);
    }

private:
    bool select_game_source() {
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
        srv_desc.NumDescriptors = f2::NativeWorldRenderer::kMaxMaterialTextures + kUiTextureCount;
        srv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        srv_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(device_->CreateDescriptorHeap(&srv_desc, IID_PPV_ARGS(&descriptor_heap_)))) return false;
        descriptor_stride_ = device_->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        video_descriptor_index_ = f2::NativeWorldRenderer::kMaxMaterialTextures + kUiTextureCount - 1;

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

    void update_video() {
        if (game_.frontend.state() != f2::FrontendState::IntroVideo || video_root_.empty()) {
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
        const auto* clip = game_.frontend.intro_videos().current_clip();
        if (!clip) return;
        const auto path = video_root_ / clip->asset;
        if (path != active_video_path_) {
            active_video_path_ = path;
            video_frame_ = {};
            video_next_frame_time_ = 0.0;
            if (!video_decoder_.open(path, video_error_)) return;
        }
        const double target_time = game_.frontend.intro_videos().current_time();
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
    }

    void upload_ui_textures(ID3D12GraphicsCommandList* command_list) {
        const std::array<f2::NativeUiAsset, 5> assets = {
            f2::NativeUiAsset::TitleBackground, f2::NativeUiAsset::MainBackground,
            f2::NativeUiAsset::Logo, f2::NativeUiAsset::Accept, f2::NativeUiAsset::Back};
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

    void handle_input() {
        using Action = f2::NativeInputAction;
        if (input_.pressed(Action::Up)) game_.frontend.dispatch(f2::FrontendAction::Up);
        if (input_.pressed(Action::Down)) game_.frontend.dispatch(f2::FrontendAction::Down);
        if (input_.pressed(Action::Accept)) game_.frontend.dispatch(f2::FrontendAction::Accept);
        if (input_.pressed(Action::Back)) game_.frontend.dispatch(f2::FrontendAction::Back);
        if (input_.pressed(Action::Skip)) game_.frontend.dispatch(f2::FrontendAction::Skip);
    }

    void draw() {
        if (game_.frontend.state() == f2::FrontendState::IntroVideo && video_frame_.serial != 0) {
            ensure_video_texture(video_frame_);
        }
        if (game_.frontend.state() == f2::FrontendState::Title ||
            game_.frontend.state() == f2::FrontendState::MainMenu) {
            ensure_ui_textures();
        }
        ImGui_ImplDX12_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        const auto state = game_.frontend.state();
        if (state == f2::FrontendState::Boot || state == f2::FrontendState::IntroVideo) {
            ImGui::SetNextWindowPos(ImVec2(0, 0));
            ImGui::SetNextWindowSize(ImVec2(static_cast<float>(width_), static_cast<float>(height_)));
            ImGui::Begin("##intro", nullptr,
                         ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoSavedSettings);
            if (state == f2::FrontendState::IntroVideo && video_texture_) {
                ImGui::SetCursorPos(ImVec2(0, 0));
                ImGui::Image(static_cast<ImTextureID>(video_gpu_handle_.ptr),
                             ImVec2(static_cast<float>(width_), static_cast<float>(height_)));
            }
            ImGui::End();
        } else if (state == f2::FrontendState::Title) {
            ImGui::SetNextWindowPos(ImVec2(0, 0));
            ImGui::SetNextWindowSize(ImVec2(static_cast<float>(width_), static_cast<float>(height_)));
            ImGui::Begin("##title", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoSavedSettings);
            auto* draw_list = ImGui::GetWindowDrawList();
            const auto& background = ui_textures_[0];
            if (background.texture) {
                ImGui::SetCursorPos(ImVec2(0, 0));
                ImGui::Image(static_cast<ImTextureID>(background.gpu.ptr),
                             ImVec2(static_cast<float>(width_), static_cast<float>(height_)));
            } else {
                draw_list->AddRectFilled(ImVec2(0, 0), ImVec2(width_, height_), IM_COL32(8, 12, 22, 255));
            }
            if (ui_textures_[1].texture) {
                const auto logo_height = height_ * 0.25f;
                ImGui::SetCursorPos(ImVec2((width_ - logo_height * ui_textures_[1].width /
                                           ui_textures_[1].height) * 0.5f, height_ * 0.27f));
                ImGui::Image(static_cast<ImTextureID>(ui_textures_[1].gpu.ptr),
                             ImVec2(logo_height * ui_textures_[1].width / ui_textures_[1].height,
                                    logo_height));
            } else {
                draw_list->AddText(ImGui::GetFont(), 92.0f,
                                   ImVec2(width_ * 0.24f, height_ * 0.32f),
                                   IM_COL32(255, 255, 255, 245), "FABLE II");
            }
            const auto prompt = input_.prompt(f2::NativeInputAction::Accept);
            const std::string prompt_text = "Press " + prompt + " to start";
            const auto prompt_size = ImGui::CalcTextSize(prompt_text.c_str());
            draw_list->AddText(ImGui::GetFont(), 26.0f,
                               ImVec2((width_ - prompt_size.x) * 0.5f, height_ * 0.66f),
                               IM_COL32(235, 235, 235, 235), prompt_text.c_str());
            ImGui::SetCursorPos(ImVec2(0, 0));
            if (ImGui::InvisibleButton("##title_accept", ImVec2(static_cast<float>(width_),
                                                                  static_cast<float>(height_)))) {
                game_.frontend.dispatch(f2::FrontendAction::Accept);
            }
            ImGui::End();
        } else if (state == f2::FrontendState::MainMenu || state == f2::FrontendState::Options) {
            ImGui::SetNextWindowPos(ImVec2(0, 0));
            ImGui::SetNextWindowSize(ImVec2(static_cast<float>(width_), static_cast<float>(height_)));
            ImGui::Begin(state == f2::FrontendState::MainMenu ? "##main_menu" : "##options", nullptr,
                         ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoSavedSettings);
            auto* draw_list = ImGui::GetWindowDrawList();
            const auto& background = ui_textures_[4].texture ? ui_textures_[4] : ui_textures_[0];
            if (background.texture) {
                ImGui::SetCursorPos(ImVec2(0, 0));
                ImGui::Image(static_cast<ImTextureID>(background.gpu.ptr),
                             ImVec2(static_cast<float>(width_), static_cast<float>(height_)));
            } else {
                draw_list->AddRectFilled(ImVec2(0, 0), ImVec2(width_, height_), IM_COL32(8, 12, 22, 255));
                draw_list->AddRectFilled(ImVec2(0, 0), ImVec2(width_ * 0.46f, height_),
                                         IM_COL32(14, 24, 39, 255));
            }
            if (state == f2::FrontendState::MainMenu) {
                const float row_x = width_ * 0.16f;
                const float row_y = height_ * 0.23f;
                const float row_width = width_ * 0.32f;
                const float row_height = 52.0f;
                const float row_gap = 10.0f;
                for (std::size_t index = 0; index < game_.frontend.menu_items().size(); ++index) {
                    const auto& item = game_.frontend.menu_items()[index];
                    const bool selected = index == game_.frontend.selected_item();
                    const ImVec2 row_position(row_x, row_y + index * (row_height + row_gap));
                    const auto window_position = ImGui::GetWindowPos();
                    const ImVec2 row_min(window_position.x + row_position.x,
                                        window_position.y + row_position.y);
                    const ImVec2 row_max(row_min.x + row_width, row_min.y + row_height);
                    draw_list->AddRectFilled(row_min, row_max,
                                             selected ? IM_COL32(60, 43, 27, 245)
                                                       : IM_COL32(19, 16, 15, 225),
                                             24.0f);
                    draw_list->AddRect(row_min, row_max,
                                       selected ? IM_COL32(223, 166, 91, 255)
                                                : IM_COL32(139, 91, 48, 235),
                                       24.0f, 0, selected ? 3.0f : 2.0f);
                    ImGui::SetCursorPos(row_position);
                    if (ImGui::InvisibleButton(("##menu_" + item.id).c_str(),
                                               ImVec2(row_width, row_height))) {
                        game_.frontend.select_menu_item(item.id);
                        game_.frontend.dispatch(f2::FrontendAction::Accept);
                    }
                    if (ImGui::IsItemHovered()) game_.frontend.select_menu_item(item.id);
                    draw_list->AddText(ImGui::GetFont(), 24.0f,
                                       ImVec2(row_min.x + 42.0f, row_min.y + 13.0f),
                                       selected ? IM_COL32(255, 226, 143, 255)
                                                : IM_COL32(226, 195, 125, 245),
                                       item.label.c_str());
                    if (selected && input_.using_controller_prompts() && ui_textures_[2].texture) {
                        ImGui::SetCursorPos(ImVec2(row_x - 47.0f, row_y + index *
                                                   (row_height + row_gap) + 6.0f));
                        ImGui::Image(static_cast<ImTextureID>(ui_textures_[2].gpu.ptr),
                                     ImVec2(40.0f, 40.0f));
                    } else if (selected) {
                        draw_list->AddText(ImGui::GetFont(), 16.0f,
                                           ImVec2(row_min.x - 70.0f, row_min.y + 18.0f),
                                           IM_COL32(240, 220, 160, 255),
                                           input_.prompt(f2::NativeInputAction::Accept).c_str());
                    }
                }
            } else {
                ImGui::SetCursorPos(ImVec2(width_ * 0.08f, height_ * 0.10f));
                ImGui::TextUnformatted("Options");
                ImGui::TextUnformatted("Language and subtitle settings will use the native PC configuration.");
                ImGui::Text("%s  BACK", input_.prompt(f2::NativeInputAction::Back).c_str());
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
        const float clear[] = {0.015f, 0.02f, 0.035f, 1.0f};
        command_list_->OMSetRenderTargets(1, &frame.rtv, FALSE, nullptr);
        command_list_->ClearRenderTargetView(frame.rtv, clear, 0, nullptr);
        if (state == f2::FrontendState::World) {
            world_renderer_.render(command_list_.Get(), game_.scene, width_, height_,
                                   game_.elapsed_seconds);
        }
        ID3D12DescriptorHeap* heaps[] = {descriptor_heap_.Get()};
        command_list_->SetDescriptorHeaps(1, heaps);
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
    UINT rtv_stride_ = 0;
    UINT descriptor_stride_ = 0;
    UINT video_descriptor_index_ = 0;
    UINT64 fence_value_ = 0;
    HANDLE fence_event_ = nullptr;
    std::optional<f2::GameSource> source_;
    std::filesystem::path video_root_;
    std::filesystem::path ui_root_;
    f2::NativeUiAssets ui_assets_;
    std::array<UiGpuTexture, 5> ui_textures_;
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

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int) {
    FrontendApp app;
    return app.initialise(instance) ? app.run() : 1;
}
