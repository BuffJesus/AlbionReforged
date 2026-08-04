#include "f2/native_game.h"
#include "f2/native_install.h"
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

struct FrameContext {
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12Resource> render_target;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
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
        srv_desc.NumDescriptors = 2;
        srv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        srv_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(device_->CreateDescriptorHeap(&srv_desc, IID_PPV_ARGS(&descriptor_heap_)))) return false;

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

    void handle_input() {
        auto& io = ImGui::GetIO();
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true)) game_.frontend.dispatch(f2::FrontendAction::Up);
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true)) game_.frontend.dispatch(f2::FrontendAction::Down);
        if (ImGui::IsKeyPressed(ImGuiKey_Enter, true)) game_.frontend.dispatch(f2::FrontendAction::Accept);
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, true)) game_.frontend.dispatch(f2::FrontendAction::Back);
        if (ImGui::IsKeyPressed(ImGuiKey_Space, true)) game_.frontend.dispatch(f2::FrontendAction::Skip);
        (void)io;
    }

    void draw() {
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
            ImGui::SetCursorPos(ImVec2(width_ * 0.5f - 180.0f, height_ * 0.45f));
            if (state == f2::FrontendState::Boot) {
                ImGui::TextUnformatted("FABLE II NATIVE");
            } else if (const auto* clip = game_.frontend.intro_videos().current_clip()) {
                ImGui::Text("INTRO VIDEO: %s", clip->id.c_str());
                ImGui::Text("Asset: %s", clip->asset.string().c_str());
            } else {
                ImGui::TextUnformatted("INTRO VIDEO PLAYER");
            }
            ImGui::SetCursorPos(ImVec2(width_ * 0.5f - 120.0f, height_ * 0.55f));
            ImGui::TextUnformatted("Press Space to skip");
            ImGui::End();
        } else if (state == f2::FrontendState::Title) {
            ImGui::SetNextWindowPos(ImVec2(width_ * 0.5f, height_ * 0.5f),
                                    ImGuiCond_Always, ImVec2(0.5f, 0.5f));
            ImGui::Begin("##title", nullptr, ImGuiWindowFlags_AlwaysAutoResize |
                         ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
            ImGui::TextUnformatted("FABLE II");
            ImGui::TextUnformatted("Press Enter to start");
            ImGui::End();
        } else if (state == f2::FrontendState::MainMenu || state == f2::FrontendState::Options) {
            ImGui::SetNextWindowPos(ImVec2(width_ * 0.5f, height_ * 0.5f),
                                    ImGuiCond_Always, ImVec2(0.5f, 0.5f));
            ImGui::Begin(state == f2::FrontendState::MainMenu ? "Main Menu" : "Options",
                         nullptr, ImGuiWindowFlags_AlwaysAutoResize);
            if (state == f2::FrontendState::MainMenu) {
                for (std::size_t index = 0; index < game_.frontend.menu_items().size(); ++index) {
                    const auto& item = game_.frontend.menu_items()[index];
                    const bool selected = index == game_.frontend.selected_item();
                    if (selected) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.8f, 0.2f, 1));
                    ImGui::Text("%s%s", selected ? "> " : "  ", item.label.c_str());
                    if (selected) ImGui::PopStyleColor();
                }
            } else {
                ImGui::TextUnformatted("Native PC options");
                ImGui::TextUnformatted("Resolution, audio, input, mods, and accessibility will live here.");
                ImGui::TextUnformatted("Press Escape to return.");
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
        if (fence_event_) CloseHandle(fence_event_);
    }

    HWND window_ = nullptr;
    UINT width_ = 1280;
    UINT height_ = 720;
    UINT rtv_stride_ = 0;
    UINT64 fence_value_ = 0;
    HANDLE fence_event_ = nullptr;
    std::optional<f2::GameSource> source_;
    f2::NativeWorldRenderer world_renderer_;
    f2::NativeGame game_;
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
