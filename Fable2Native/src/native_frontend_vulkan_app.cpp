#define VK_USE_PLATFORM_WIN32_KHR

#include "f2/native_game.h"
#include "f2/native_audio.h"
#include "f2/native_font.h"
#include "f2/frontend_scene_builder.h"
#include "f2/native_frontend_entry.h"
#include "f2/native_install.h"
#include "f2/native_input.h"
#include "f2/native_ui.h"
#include "f2/native_video_decoder.h"
#include "f2/native_vulkan_ui_renderer.h"
#include "f2/native_vulkan_video_texture.h"
#include "f2/native_vulkan_world_renderer.h"
#include "f2/native_vulkan_sky_renderer.h"
#include "f2/native_vulkan_cloud_renderer.h"
#include "f2/native_vulkan_sky_billboard_renderer.h"
#include "f2/native_vulkan_sky_stars_renderer.h"
#include "f2/native_vulkan_tonemap_renderer.h"
#include "f2/render/ui_draw_list.h"

#include <windows.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <vulkan/vulkan.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {

// UI texture slots — identical mapping to the D3D12 frontend so the shared FrontendSceneBuilder
// resolves the same logical asset to the same slot on both backends.
constexpr std::uint32_t kUiTextureCount = 36;

std::size_t ui_slot(f2::NativeUiAsset asset) {
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

// TextureId scheme for the neutral scene: UI slot s -> s+1, plus font and video ids above the slots.
constexpr f2::render::TextureId kFontTextureId = kUiTextureCount + 1;
constexpr f2::render::TextureId kVideoTextureId = kUiTextureCount + 2;

constexpr std::uint32_t kFrameCount = 2;

std::optional<std::filesystem::path> show_source_picker(HWND owner, bool pick_iso) {
    Microsoft::WRL::ComPtr<IFileOpenDialog> dialog;
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

    Microsoft::WRL::ComPtr<IShellItem> item;
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
    const auto path = source_config_path();
    if (path.empty()) return std::nullopt;
    std::wifstream input(path);
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

const char* vulkan_result_name(VkResult result) {
    switch (result) {
    case VK_SUCCESS: return "success";
    case VK_ERROR_INITIALIZATION_FAILED: return "initialization failed";
    case VK_ERROR_DEVICE_LOST: return "device lost";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "out of host memory";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "out of device memory";
    default: return "unknown Vulkan error";
    }
}

class FrontendApp {
public:
    ~FrontendApp() { shutdown(); }

    bool initialise(HINSTANCE instance) {
        const HRESULT com_result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        com_initialized_ = SUCCEEDED(com_result);
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
        window_class.lpszClassName = "Fable2NativeVulkanFrontend";
        window_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
        if (!RegisterClassA(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            return false;
        }
        window_ = CreateWindowA(window_class.lpszClassName, "Fable II Native - Vulkan",
                                WS_POPUP, CW_USEDEFAULT, CW_USEDEFAULT,
                                width_, height_, nullptr, nullptr, instance, this);
        if (!window_ || !select_game_source()) return false;

        if (const auto scene = command_line_path(L"--scene")) {
            std::string error;
            if (!game_.load_scene(*scene, error)) {
                MessageBoxA(window_, error.c_str(), "Fable II Native - scene load failed",
                            MB_OK | MB_ICONERROR);
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
        audio_.initialise(command_line_path(L"--audio-root").value_or(ui_root_ / "native_audio"),
                          audio_error);
        input_.load_bindings(source_config_path().parent_path() / "bindings.ini");
        if (command_line_flag(L"--controller-prompts")) input_.force_controller_prompts(true);
        if (command_line_flag(L"--skip-intro")) {
            game_.frontend.dispatch(f2::FrontendAction::Skip);
            if (command_line_flag(L"--start-menu")) {
                game_.frontend.dispatch(f2::FrontendAction::Accept);
            }
        }
        if (command_line_flag(L"--show-fps")) game_.frontend.set_fps_display_enabled(true);
        if (command_line_flag(L"--start-world")) {
            game_.frontend.debug_jump_to(f2::FrontendState::World);
        }
        if (!create_vulkan()) return false;
        // The World passes bake against the HDR render pass when the compositor is available, so they
        // write RGBA16F; otherwise they bake against the swap-chain pass (old direct clamp path).
        const VkRenderPass world_pass = hdr_enabled_ ? hdr_render_pass_ : render_pass_;
        std::string renderer_error;
        if (!world_renderer_.initialise(physical_device_, device_, command_pool_, queue_,
                                        world_pass, hdr_enabled_ ? kHdrFormat : surface_format_.format,
                                        msaa_samples_, depth_resolve_view_,
                                        source_ ? source_->data_root : std::filesystem::path{},
                                        F2NATIVE_VULKAN_SHADER_DIR, game_.scene,
                                        renderer_error)) {
            MessageBoxA(window_, renderer_error.c_str(), "Fable II Native - Vulkan renderer failed",
                        MB_OK | MB_ICONERROR);
            return false;
        }
        std::string sky_error;
        if (!sky_renderer_.initialise(physical_device_, device_, world_pass,
                                      hdr_enabled_ ? kHdrFormat : surface_format_.format,
                                      msaa_samples_, F2NATIVE_VULKAN_SHADER_DIR, sky_error)) {
            MessageBoxA(window_, sky_error.c_str(), "Fable II Native - Vulkan sky failed",
                        MB_OK | MB_ICONWARNING);
        }
        // Cloud-layer pass (non-fatal on failure, like the sky): scrolling theme cloud layers
        // drawn over the sky and behind the world.
        std::string cloud_error;
        if (!cloud_renderer_.initialise(physical_device_, device_, command_pool_, queue_,
                                        world_pass, msaa_samples_, F2NATIVE_VULKAN_SHADER_DIR,
                                        cloud_error)) {
            OutputDebugStringA(
                ("Fable2Native: Vulkan cloud renderer disabled: " + cloud_error + "\n").c_str());
        }
        std::string billboard_error;
        if (!billboard_renderer_.initialise(physical_device_, device_, command_pool_, queue_,
                                            world_pass, msaa_samples_, F2NATIVE_VULKAN_SHADER_DIR,
                                            billboard_error)) {
            OutputDebugStringA(
                ("Fable2Native: Vulkan billboard renderer disabled: " + billboard_error + "\n")
                    .c_str());
        }
        std::string stars_error;
        if (!stars_renderer_.initialise(physical_device_, device_, world_pass, msaa_samples_,
                                        F2NATIVE_VULKAN_SHADER_DIR, stars_error)) {
            OutputDebugStringA(
                ("Fable2Native: Vulkan stars renderer disabled: " + stars_error + "\n").c_str());
        }

        ShowWindow(window_, SW_SHOWDEFAULT);
        UpdateWindow(window_);
        if (!create_native_ui()) return false;
        return true;
    }

    int run() {
        MSG message{};
        auto previous = std::chrono::steady_clock::now();
        last_resolution_index_ = game_.frontend.resolution_index();  // don't resize on the first frame
        while (message.message != WM_QUIT) {
            while (PeekMessageA(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message);
                DispatchMessageA(&message);
            }
            const auto now = std::chrono::steady_clock::now();
            const double delta = std::chrono::duration<double>(now - previous).count();
            previous = now;
            if (delta > 0.0) current_fps_ = current_fps_ * 0.9 + (1.0 / delta) * 0.1;
            game_.tick(delta);
            update_video();
            input_.poll();
            handle_input();
            update_free_camera(delta);
            update_character_controller(delta);
            apply_resolution_setting();
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
                        "Run f2native_installer.exe to extract the ISO into a user-selected directory, then relaunch the Vulkan frontend.",
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
            app->width_ = std::max<UINT>(1, LOWORD(lparam));
            app->height_ = std::max<UINT>(1, HIWORD(lparam));
            app->framebuffer_resized_ = true;
            return 0;
        }
        if (message == WM_DESTROY) {
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcA(window, message, wparam, lparam);
    }

    bool create_vulkan() {
        const char* instance_extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME,
                                             VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
        VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        application.pApplicationName = "Fable II Native";
        application.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
        application.pEngineName = "Fable II Native Runtime";
        application.engineVersion = VK_MAKE_VERSION(0, 1, 0);
        application.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        instance_info.pApplicationInfo = &application;
        instance_info.enabledExtensionCount = static_cast<std::uint32_t>(std::size(instance_extensions));
        instance_info.ppEnabledExtensionNames = instance_extensions;
        VkResult result = vkCreateInstance(&instance_info, nullptr, &instance_);
        if (result != VK_SUCCESS) return fail("Vulkan instance creation failed: ", result);

        VkWin32SurfaceCreateInfoKHR surface_info{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
        surface_info.hinstance = GetModuleHandleA(nullptr);
        surface_info.hwnd = window_;
        result = vkCreateWin32SurfaceKHR(instance_, &surface_info, nullptr, &surface_);
        if (result != VK_SUCCESS) return fail("Vulkan Win32 surface creation failed: ", result);

        if (!select_physical_device()) return false;
        const float priority = 1.0f;
        VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queue_info.queueFamilyIndex = queue_family_;
        queue_info.queueCount = 1;
        queue_info.pQueuePriorities = &priority;
        const bool has_render_pass2 = device_extension_available(
            VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME);
        const bool has_depth_resolve = device_extension_available(
            VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME);
        depth_resolve_supported_ = has_render_pass2 && has_depth_resolve &&
                                   supports_max_depth_resolve();
        std::vector<const char*> device_extensions{VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        if (has_render_pass2) device_extensions.push_back(VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME);
        if (has_depth_resolve) device_extensions.push_back(VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME);
        VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        device_info.queueCreateInfoCount = 1;
        device_info.pQueueCreateInfos = &queue_info;
        device_info.enabledExtensionCount = static_cast<std::uint32_t>(device_extensions.size());
        device_info.ppEnabledExtensionNames = device_extensions.data();
        result = vkCreateDevice(physical_device_, &device_info, nullptr, &device_);
        if (result != VK_SUCCESS) return fail("Vulkan device creation failed: ", result);
        if (has_render_pass2) {
            create_render_pass2_ = reinterpret_cast<PFN_vkCreateRenderPass2>(
                vkGetDeviceProcAddr(device_, "vkCreateRenderPass2KHR"));
            if (!create_render_pass2_) {
                create_render_pass2_ = reinterpret_cast<PFN_vkCreateRenderPass2>(
                    vkGetDeviceProcAddr(device_, "vkCreateRenderPass2"));
            }
        }
        if (!create_render_pass2_) depth_resolve_supported_ = false;
        vkGetDeviceQueue(device_, queue_family_, 0, &queue_);
        msaa_samples_ = choose_sample_count(game_.frontend.anti_aliasing_index());  // AA option

        VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool_info.queueFamilyIndex = queue_family_;
        if (vkCreateCommandPool(device_, &pool_info, nullptr, &command_pool_) != VK_SUCCESS) {
            return false;
        }
        if (!create_swapchain() || !create_render_pass() || !create_framebuffers()) return false;

        // HDR compositor path (mirror D3D12): build the HDR World render pass + scene target(s), and
        // the tonemap compositor (baked against the swap-chain render_pass_ + msaa_samples_). Any
        // failure disables HDR and the World renders straight into render_pass_ (old clamp path).
        read_hdr_options();
        setup_hdr();

        const char* resolve_status = depth_resolve_view_ ?
            "Fable II Native - Vulkan [MSAA depth resolve]" :
            "Fable II Native - Vulkan [depth resolve fallback]";
        SetWindowTextA(window_, resolve_status);

        VkCommandBufferAllocateInfo command_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        command_info.commandPool = command_pool_;
        command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        command_info.commandBufferCount = static_cast<std::uint32_t>(command_buffers_.size());
        command_buffers_.resize(swapchain_images_.size());
        command_info.commandBufferCount = static_cast<std::uint32_t>(command_buffers_.size());
        if (vkAllocateCommandBuffers(device_, &command_info, command_buffers_.data()) != VK_SUCCESS) {
            return false;
        }
        for (std::uint32_t index = 0; index < kFrameCount; ++index) {
            VkSemaphoreCreateInfo semaphore_info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            if (vkCreateSemaphore(device_, &semaphore_info, nullptr, &image_available_[index]) != VK_SUCCESS ||
                vkCreateSemaphore(device_, &semaphore_info, nullptr, &render_finished_[index]) != VK_SUCCESS ||
                vkCreateFence(device_, &fence_info, nullptr, &in_flight_[index]) != VK_SUCCESS) {
                return false;
            }
        }
        return true;
    }

    bool select_physical_device() {
        std::uint32_t count = 0;
        if (vkEnumeratePhysicalDevices(instance_, &count, nullptr) != VK_SUCCESS || count == 0) return false;
        std::vector<VkPhysicalDevice> devices(count);
        vkEnumeratePhysicalDevices(instance_, &count, devices.data());
        for (const auto candidate : devices) {
            std::uint32_t family_count = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, nullptr);
            std::vector<VkQueueFamilyProperties> families(family_count);
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, families.data());
            for (std::uint32_t index = 0; index < family_count; ++index) {
                VkBool32 present = VK_FALSE;
                vkGetPhysicalDeviceSurfaceSupportKHR(candidate, index, surface_, &present);
                if ((families[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
                    physical_device_ = candidate;
                    queue_family_ = index;
                    return true;
                }
            }
        }
        return false;
    }

    bool device_extension_available(const char* requested) const {
        std::uint32_t count = 0;
        if (vkEnumerateDeviceExtensionProperties(physical_device_, nullptr, &count, nullptr) !=
            VK_SUCCESS) return false;
        std::vector<VkExtensionProperties> extensions(count);
        if (vkEnumerateDeviceExtensionProperties(physical_device_, nullptr, &count,
                                                 extensions.data()) != VK_SUCCESS) return false;
        for (const auto& extension : extensions) {
            if (std::strcmp(extension.extensionName, requested) == 0) return true;
        }
        return false;
    }

    bool supports_max_depth_resolve() const {
        VkPhysicalDeviceDepthStencilResolveProperties resolve{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_STENCIL_RESOLVE_PROPERTIES};
        VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        properties.pNext = &resolve;
        vkGetPhysicalDeviceProperties2(physical_device_, &properties);
        return (resolve.supportedDepthResolveModes & VK_RESOLVE_MODE_MAX_BIT) != 0;
    }

    bool create_swapchain() {
        VkSurfaceCapabilitiesKHR capabilities{};
        if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device_, surface_, &capabilities) != VK_SUCCESS) return false;
        std::uint32_t format_count = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, surface_, &format_count, nullptr);
        std::vector<VkSurfaceFormatKHR> formats(format_count);
        vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, surface_, &format_count, formats.data());
        if (formats.empty()) return false;
        surface_format_ = formats[0];
        for (const auto& format : formats) {
            if (format.format == VK_FORMAT_B8G8R8A8_UNORM &&
                format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                surface_format_ = format;
                break;
            }
        }
        if (capabilities.currentExtent.width != UINT32_MAX) {
            extent_ = capabilities.currentExtent;
        } else {
            extent_.width = std::clamp(width_, capabilities.minImageExtent.width,
                                       capabilities.maxImageExtent.width);
            extent_.height = std::clamp(height_, capabilities.minImageExtent.height,
                                        capabilities.maxImageExtent.height);
        }
        std::uint32_t image_count = capabilities.minImageCount + 1;
        if (capabilities.maxImageCount > 0) image_count = std::min(image_count, capabilities.maxImageCount);
        VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        info.surface = surface_;
        info.minImageCount = image_count;
        info.imageFormat = surface_format_.format;
        info.imageColorSpace = surface_format_.colorSpace;
        info.imageExtent = extent_;
        info.imageArrayLayers = 1;
        info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.preTransform = capabilities.currentTransform;
        info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        info.clipped = VK_TRUE;
        if (vkCreateSwapchainKHR(device_, &info, nullptr, &swapchain_) != VK_SUCCESS) return false;
        std::uint32_t actual_count = 0;
        vkGetSwapchainImagesKHR(device_, swapchain_, &actual_count, nullptr);
        swapchain_images_.resize(actual_count);
        vkGetSwapchainImagesKHR(device_, swapchain_, &actual_count, swapchain_images_.data());
        swapchain_views_.resize(actual_count);
        for (std::size_t index = 0; index < swapchain_images_.size(); ++index) {
            VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            view.image = swapchain_images_[index];
            view.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view.format = surface_format_.format;
            view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            view.subresourceRange.levelCount = 1;
            view.subresourceRange.layerCount = 1;
            if (vkCreateImageView(device_, &view, nullptr, &swapchain_views_[index]) != VK_SUCCESS) return false;
        }
        return true;
    }

    bool create_render_pass2() {
        if (!create_render_pass2_) return false;
        // Attachment 0 = multisample color, 1 = presentable color resolve, 2 = multisample
        // reversed-Z depth, 3 = single-sample MAX depth resolve exposed to the water shader.
        VkAttachmentDescription2 attachments[4]{};
        for (auto& attachment : attachments) attachment.sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
        attachments[0].format = surface_format_.format;
        attachments[0].samples = msaa_samples_;
        attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachments[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attachments[1].format = surface_format_.format;
        attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
        attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachments[1].finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        attachments[2].format = kDepthFormat;
        attachments[2].samples = msaa_samples_;
        attachments[2].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachments[2].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[2].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[2].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[2].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachments[2].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        attachments[3].format = kDepthFormat;
        attachments[3].samples = VK_SAMPLE_COUNT_1_BIT;
        attachments[3].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[3].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachments[3].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[3].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[3].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachments[3].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkAttachmentReference2 color{VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2};
        color.attachment = 0;
        color.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        VkAttachmentReference2 resolve{VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2};
        resolve.attachment = 1;
        resolve.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        VkAttachmentReference2 depth{VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2};
        depth.attachment = 2;
        depth.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        VkAttachmentReference2 depth_read{VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2};
        depth_read.attachment = 2;
        depth_read.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        VkAttachmentReference2 depth_resolve{VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2};
        depth_resolve.attachment = 3;
        depth_resolve.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        VkAttachmentReference2 depth_resolve_read{VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2};
        depth_resolve_read.attachment = 3;
        depth_resolve_read.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        depth_resolve_read.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;  // required for input attachments

        VkSubpassDescriptionDepthStencilResolve depth_resolve_info{
            VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE};
        depth_resolve_info.depthResolveMode = VK_RESOLVE_MODE_MAX_BIT;
        depth_resolve_info.stencilResolveMode = VK_RESOLVE_MODE_NONE;
        depth_resolve_info.pDepthStencilResolveAttachment = &depth_resolve;
        VkSubpassDescription2 subpasses[2]{};
        subpasses[0].sType = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2;
        subpasses[0].pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpasses[0].colorAttachmentCount = 1;
        subpasses[0].pColorAttachments = &color;
        subpasses[0].pDepthStencilAttachment = &depth;
        subpasses[0].pNext = &depth_resolve_info;
        subpasses[1].sType = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2;
        subpasses[1].pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpasses[1].inputAttachmentCount = 1;
        subpasses[1].pInputAttachments = &depth_resolve_read;
        subpasses[1].colorAttachmentCount = 1;
        subpasses[1].pColorAttachments = &color;
        subpasses[1].pResolveAttachments = &resolve;
        subpasses[1].pDepthStencilAttachment = &depth_read;

        VkSubpassDependency2 dependencies[2]{};
        dependencies[0].sType = VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2;
        dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
        dependencies[0].dstSubpass = 0;
        dependencies[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                         VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        dependencies[1].sType = VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2;
        dependencies[1].srcSubpass = 0;
        dependencies[1].dstSubpass = 1;
        dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        dependencies[1].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                         VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        dependencies[1].dstAccessMask = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT |
                                         VK_ACCESS_SHADER_READ_BIT |
                                         VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                                         VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                         VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;

        VkRenderPassCreateInfo2 info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2};
        info.attachmentCount = 4;
        info.pAttachments = attachments;
        info.subpassCount = 2;
        info.pSubpasses = subpasses;
        info.dependencyCount = 2;
        info.pDependencies = dependencies;
        if (create_render_pass2_(device_, &info, nullptr, &render_pass_) != VK_SUCCESS) return false;
        render_pass2_active_ = true;
        return true;
    }

    bool create_render_pass() {
        const bool msaa = msaa_samples_ > VK_SAMPLE_COUNT_1_BIT;
        if (msaa && depth_resolve_supported_) {
            if (create_render_pass2()) return true;
            depth_resolve_supported_ = false;
        }
        render_pass2_active_ = false;
        // MSAA uses two subpasses: subpass 0 fills the multisample color/depth attachments,
        // then subpass 1 draws water and resolves color into the presentable image. Keeping the
        // resolve at the end of the water subpass is the prerequisite for a later depth resolve
        // attachment without exposing an unresolved depth image to a shader.
        // Single-sample frames retain the compact one-subpass layout.
        const std::uint32_t depth_index = msaa ? 2u : 1u;
        VkAttachmentDescription attachments[3]{};
        attachments[0].format = surface_format_.format;
        attachments[0].samples = msaa ? msaa_samples_ : VK_SAMPLE_COUNT_1_BIT;
        attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachments[0].storeOp = msaa ? VK_ATTACHMENT_STORE_OP_DONT_CARE
                                      : VK_ATTACHMENT_STORE_OP_STORE;
        attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachments[0].finalLayout = msaa ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                                          : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        if (msaa) {
            attachments[1].format = surface_format_.format;
            attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
            attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            attachments[1].finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        }
        attachments[depth_index].format = kDepthFormat;
        attachments[depth_index].samples = msaa ? msaa_samples_ : VK_SAMPLE_COUNT_1_BIT;
        attachments[depth_index].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachments[depth_index].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[depth_index].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[depth_index].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[depth_index].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachments[depth_index].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        VkAttachmentReference color_ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkAttachmentReference resolve_ref{1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkAttachmentReference depth_ref{depth_index,
                                        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &color_ref;
        subpass.pDepthStencilAttachment = &depth_ref;
        VkSubpassDependency dependency{};
        dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
        dependency.dstSubpass = 0;
        dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                  VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                  VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                   VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        VkSubpassDescription subpasses[2]{};
        std::array<VkSubpassDependency, 2> dependencies{};
        std::uint32_t subpass_count = 1;
        std::uint32_t dependency_count = 1;
        if (!msaa) {
            subpass.pResolveAttachments = nullptr;
            subpasses[0] = subpass;
            dependencies[0] = dependency;
        } else {
            // Opaque pass has no resolve attachment. Water is rendered in subpass 1 and owns the
            // color resolve, so the multisample color buffer remains available to blend against.
            VkAttachmentReference water_depth_ref{depth_index,
                                                   VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
            subpasses[0] = subpass;
            subpasses[1] = subpass;
            subpasses[1].pResolveAttachments = &resolve_ref;
            subpasses[1].pDepthStencilAttachment = &water_depth_ref;
            dependencies[0] = dependency;
            dependencies[1].srcSubpass = 0;
            dependencies[1].dstSubpass = 1;
            dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
            dependencies[1].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
            dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                             VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            dependencies[1].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                                             VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                             VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
            subpass_count = 2;
            dependency_count = 2;
        }
        VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        info.attachmentCount = msaa ? 3u : 2u;
        info.pAttachments = attachments;
        info.subpassCount = subpass_count;
        info.pSubpasses = subpasses;
        info.dependencyCount = dependency_count;
        info.pDependencies = dependencies.data();
        return vkCreateRenderPass(device_, &info, nullptr, &render_pass_) == VK_SUCCESS;
    }

    // Optional HDR-compositor tuning overrides (defaults give the retail glow), same env names as
    // the D3D12 frontend: FABLE2NATIVE_HDR_EXPOSURE / _BLOOM_THRESHOLD / _BLOOM_INTENSITY.
    void read_hdr_options() {
        const auto env_float = [](const char* name, float& out) {
            char buffer[64];
            if (GetEnvironmentVariableA(name, buffer, sizeof(buffer)) > 0) {
                try {
                    out = std::stof(buffer);
                } catch (...) {
                }
            }
        };
        env_float("FABLE2NATIVE_HDR_EXPOSURE", hdr_exposure_);
        env_float("FABLE2NATIVE_BLOOM_THRESHOLD", hdr_bloom_threshold_);
        env_float("FABLE2NATIVE_BLOOM_INTENSITY", hdr_bloom_intensity_);
    }

    // Build the HDR render pass + scene target(s) + compositor. On any failure, tears the HDR state
    // back down and leaves hdr_enabled_ = false so the World renders directly into render_pass_.
    void setup_hdr() {
        hdr_enabled_ = false;
        if (!create_hdr_render_pass()) {
            if (hdr_render_pass_) vkDestroyRenderPass(device_, hdr_render_pass_, nullptr);
            hdr_render_pass_ = VK_NULL_HANDLE;
            return;
        }
        std::string tonemap_error;
        // The composite pipeline renders in the swap-chain render pass' first subpass, whose colour
        // attachment carries msaa_samples_ (resolved by the water/UI subpass just like the frontend).
        if (!tonemap_renderer_.initialise(physical_device_, device_, kHdrFormat, render_pass_,
                                          msaa_samples_, F2NATIVE_VULKAN_SHADER_DIR, tonemap_error)) {
            OutputDebugStringA(
                ("Fable2Native: Vulkan HDR compositor disabled: " + tonemap_error + "\n").c_str());
            vkDestroyRenderPass(device_, hdr_render_pass_, nullptr);
            hdr_render_pass_ = VK_NULL_HANDLE;
            return;
        }
        if (!create_hdr_targets()) {
            tonemap_renderer_.destroy();
            vkDestroyRenderPass(device_, hdr_render_pass_, nullptr);
            hdr_render_pass_ = VK_NULL_HANDLE;
            return;
        }
        tonemap_renderer_.ensure_targets(hdr_resolve_view_, extent_.width, extent_.height);
        hdr_enabled_ = true;
    }

    // Re-point the HDR scene target + compositor bloom targets after a swap-chain resize. Called from
    // recreate_swapchain() once the new framebuffers (hence depth image) exist.
    void refresh_hdr_targets() {
        if (!hdr_render_pass_ || !tonemap_renderer_.ready()) {
            hdr_enabled_ = false;
            return;
        }
        if (!create_hdr_targets()) {
            hdr_enabled_ = false;
            return;
        }
        tonemap_renderer_.ensure_targets(hdr_resolve_view_, extent_.width, extent_.height);
        hdr_enabled_ = true;
    }

    // The World-pass HDR render pass — structurally identical to render_pass_ (same MSAA subpass
    // layout, so world winding / depth / water blending are unchanged) but with the colour +
    // resolve attachments in kHdrFormat, and the single-sample colour the compositor samples ending
    // in SHADER_READ_ONLY_OPTIMAL instead of PRESENT_SRC. Depth attachments are unchanged. Mirrors
    // both the render-pass2 (MSAA depth-resolve) and the legacy paths so behaviour matches.
    bool create_hdr_render_pass2() {
        if (!create_render_pass2_) return false;
        VkAttachmentDescription2 attachments[4]{};
        for (auto& attachment : attachments) attachment.sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
        attachments[0].format = kHdrFormat;
        attachments[0].samples = msaa_samples_;
        attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachments[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attachments[1].format = kHdrFormat;
        attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
        attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachments[1].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        attachments[2].format = kDepthFormat;
        attachments[2].samples = msaa_samples_;
        attachments[2].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachments[2].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[2].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[2].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[2].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachments[2].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        attachments[3].format = kDepthFormat;
        attachments[3].samples = VK_SAMPLE_COUNT_1_BIT;
        attachments[3].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[3].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachments[3].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[3].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[3].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachments[3].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkAttachmentReference2 color{VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2};
        color.attachment = 0;
        color.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        VkAttachmentReference2 resolve{VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2};
        resolve.attachment = 1;
        resolve.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        VkAttachmentReference2 depth{VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2};
        depth.attachment = 2;
        depth.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        VkAttachmentReference2 depth_read{VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2};
        depth_read.attachment = 2;
        depth_read.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        VkAttachmentReference2 depth_resolve{VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2};
        depth_resolve.attachment = 3;
        depth_resolve.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        VkAttachmentReference2 depth_resolve_read{VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2};
        depth_resolve_read.attachment = 3;
        depth_resolve_read.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        depth_resolve_read.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;  // required for input attachments

        VkSubpassDescriptionDepthStencilResolve depth_resolve_info{
            VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE};
        depth_resolve_info.depthResolveMode = VK_RESOLVE_MODE_MAX_BIT;
        depth_resolve_info.stencilResolveMode = VK_RESOLVE_MODE_NONE;
        depth_resolve_info.pDepthStencilResolveAttachment = &depth_resolve;
        VkSubpassDescription2 subpasses[2]{};
        subpasses[0].sType = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2;
        subpasses[0].pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpasses[0].colorAttachmentCount = 1;
        subpasses[0].pColorAttachments = &color;
        subpasses[0].pDepthStencilAttachment = &depth;
        subpasses[0].pNext = &depth_resolve_info;
        subpasses[1].sType = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2;
        subpasses[1].pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpasses[1].inputAttachmentCount = 1;
        subpasses[1].pInputAttachments = &depth_resolve_read;
        subpasses[1].colorAttachmentCount = 1;
        subpasses[1].pColorAttachments = &color;
        subpasses[1].pResolveAttachments = &resolve;
        subpasses[1].pDepthStencilAttachment = &depth_read;

        VkSubpassDependency2 dependencies[3]{};
        dependencies[0].sType = VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2;
        dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
        dependencies[0].dstSubpass = 0;
        dependencies[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                         VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        dependencies[1].sType = VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2;
        dependencies[1].srcSubpass = 0;
        dependencies[1].dstSubpass = 1;
        dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        dependencies[1].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                         VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        dependencies[1].dstAccessMask = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT |
                                         VK_ACCESS_SHADER_READ_BIT |
                                         VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                                         VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                         VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
        // The resolved HDR colour is sampled by the compositor after this render pass.
        dependencies[2].sType = VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2;
        dependencies[2].srcSubpass = 1;
        dependencies[2].dstSubpass = VK_SUBPASS_EXTERNAL;
        dependencies[2].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependencies[2].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        dependencies[2].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dependencies[2].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        VkRenderPassCreateInfo2 info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2};
        info.attachmentCount = 4;
        info.pAttachments = attachments;
        info.subpassCount = 2;
        info.pSubpasses = subpasses;
        info.dependencyCount = 3;
        info.pDependencies = dependencies;
        return create_render_pass2_(device_, &info, nullptr, &hdr_render_pass_) == VK_SUCCESS;
    }

    bool create_hdr_render_pass() {
        const bool msaa = msaa_samples_ > VK_SAMPLE_COUNT_1_BIT;
        // Reuse the render-pass2 (depth-resolve) layout when the swap-chain pass is on that path, so
        // the water shader's sampled depth resolve remains available for the HDR world too.
        if (msaa && render_pass2_active_ && depth_resolve_view_) {
            if (create_hdr_render_pass2()) return true;
        }
        const std::uint32_t depth_index = msaa ? 2u : 1u;
        VkAttachmentDescription attachments[3]{};
        attachments[0].format = kHdrFormat;
        attachments[0].samples = msaa ? msaa_samples_ : VK_SAMPLE_COUNT_1_BIT;
        attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachments[0].storeOp = msaa ? VK_ATTACHMENT_STORE_OP_DONT_CARE
                                      : VK_ATTACHMENT_STORE_OP_STORE;
        attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachments[0].finalLayout = msaa ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                                          : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        if (msaa) {
            attachments[1].format = kHdrFormat;
            attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
            attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            attachments[1].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }
        attachments[depth_index].format = kDepthFormat;
        attachments[depth_index].samples = msaa ? msaa_samples_ : VK_SAMPLE_COUNT_1_BIT;
        attachments[depth_index].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachments[depth_index].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[depth_index].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[depth_index].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[depth_index].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachments[depth_index].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        VkAttachmentReference color_ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkAttachmentReference resolve_ref{1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkAttachmentReference depth_ref{depth_index,
                                        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &color_ref;
        subpass.pDepthStencilAttachment = &depth_ref;
        VkSubpassDescription subpasses[2]{};
        std::array<VkSubpassDependency, 3> dependencies{};
        std::uint32_t subpass_count = 1;
        std::uint32_t dependency_count = 2;
        VkSubpassDependency in_dep{};
        in_dep.srcSubpass = VK_SUBPASS_EXTERNAL;
        in_dep.dstSubpass = 0;
        in_dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                              VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        in_dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                              VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        in_dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        if (!msaa) {
            subpasses[0] = subpass;
            dependencies[0] = in_dep;
            // Single-sample HDR colour (attachment 0) is sampled by the compositor afterwards.
            dependencies[1].srcSubpass = 0;
            dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
            dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        } else {
            VkAttachmentReference water_depth_ref{depth_index,
                                                   VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
            subpasses[0] = subpass;
            subpasses[1] = subpass;
            subpasses[1].pResolveAttachments = &resolve_ref;
            subpasses[1].pDepthStencilAttachment = &water_depth_ref;
            dependencies[0] = in_dep;
            dependencies[1].srcSubpass = 0;
            dependencies[1].dstSubpass = 1;
            dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
            dependencies[1].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
            dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                             VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            dependencies[1].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                                             VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                             VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
            // The resolved HDR colour (attachment 1) is sampled by the compositor afterwards.
            dependencies[2].srcSubpass = 1;
            dependencies[2].dstSubpass = VK_SUBPASS_EXTERNAL;
            dependencies[2].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            dependencies[2].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            dependencies[2].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            dependencies[2].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            subpass_count = 2;
            dependency_count = 3;
        }
        VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        info.attachmentCount = msaa ? 3u : 2u;
        info.pAttachments = attachments;
        info.subpassCount = subpass_count;
        info.pSubpasses = subpasses;
        info.dependencyCount = dependency_count;
        info.pDependencies = dependencies.data();
        return vkCreateRenderPass(device_, &info, nullptr, &hdr_render_pass_) == VK_SUCCESS;
    }

    // Allocate a device-local image + view. Helper for the HDR colour/resolve targets.
    bool create_image(VkFormat format, VkSampleCountFlagBits samples, VkImageUsageFlags usage,
                      VkImageAspectFlags aspect, VkImage& image, VkDeviceMemory& memory,
                      VkImageView& view) {
        VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        image_info.imageType = VK_IMAGE_TYPE_2D;
        image_info.format = format;
        image_info.extent = {extent_.width, extent_.height, 1};
        image_info.mipLevels = 1;
        image_info.arrayLayers = 1;
        image_info.samples = samples;
        image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
        image_info.usage = usage;
        image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateImage(device_, &image_info, nullptr, &image) != VK_SUCCESS) return false;
        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(device_, image, &requirements);
        VkPhysicalDeviceMemoryProperties memory_properties{};
        vkGetPhysicalDeviceMemoryProperties(physical_device_, &memory_properties);
        std::uint32_t memory_type = 0;
        bool found = false;
        for (std::uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i) {
            if ((requirements.memoryTypeBits & (1u << i)) &&
                (memory_properties.memoryTypes[i].propertyFlags &
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
                memory_type = i;
                found = true;
                break;
            }
        }
        if (!found) return false;
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = memory_type;
        if (vkAllocateMemory(device_, &allocation, nullptr, &memory) != VK_SUCCESS ||
            vkBindImageMemory(device_, image, memory, 0) != VK_SUCCESS) return false;
        VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view_info.image = image;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = format;
        view_info.subresourceRange.aspectMask = aspect;
        view_info.subresourceRange.levelCount = 1;
        view_info.subresourceRange.layerCount = 1;
        return vkCreateImageView(device_, &view_info, nullptr, &view) == VK_SUCCESS;
    }

    void destroy_hdr_targets() {
        if (hdr_framebuffer_) vkDestroyFramebuffer(device_, hdr_framebuffer_, nullptr);
        if (hdr_color_view_) vkDestroyImageView(device_, hdr_color_view_, nullptr);
        if (hdr_color_image_) vkDestroyImage(device_, hdr_color_image_, nullptr);
        if (hdr_color_memory_) vkFreeMemory(device_, hdr_color_memory_, nullptr);
        if (hdr_resolve_view_) vkDestroyImageView(device_, hdr_resolve_view_, nullptr);
        if (hdr_resolve_image_) vkDestroyImage(device_, hdr_resolve_image_, nullptr);
        if (hdr_resolve_memory_) vkFreeMemory(device_, hdr_resolve_memory_, nullptr);
        hdr_framebuffer_ = VK_NULL_HANDLE;
        hdr_color_view_ = VK_NULL_HANDLE;
        hdr_color_image_ = VK_NULL_HANDLE;
        hdr_color_memory_ = VK_NULL_HANDLE;
        hdr_resolve_view_ = VK_NULL_HANDLE;
        hdr_resolve_image_ = VK_NULL_HANDLE;
        hdr_resolve_memory_ = VK_NULL_HANDLE;
    }

    // (Re)create the HDR colour target(s) + framebuffer at the current extent. Requires
    // hdr_render_pass_ + depth_image_ already created (create_framebuffers made the depth image).
    // Returns false only on a hard allocation failure; the caller then disables HDR.
    bool create_hdr_targets() {
        destroy_hdr_targets();
        const bool msaa = msaa_samples_ > VK_SAMPLE_COUNT_1_BIT;
        // Single-sample resolve image the compositor samples (SAMPLED); at 1x it IS the colour image.
        if (!create_image(kHdrFormat, VK_SAMPLE_COUNT_1_BIT,
                          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                          VK_IMAGE_ASPECT_COLOR_BIT, hdr_resolve_image_, hdr_resolve_memory_,
                          hdr_resolve_view_)) {
            destroy_hdr_targets();
            return false;
        }
        if (msaa) {
            if (!create_image(kHdrFormat, msaa_samples_, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                              VK_IMAGE_ASPECT_COLOR_BIT, hdr_color_image_, hdr_color_memory_,
                              hdr_color_view_)) {
                destroy_hdr_targets();
                return false;
            }
        }
        // Attachment order matches create_hdr_render_pass[2]: [0]=colour, [1]=resolve, [2]=MSAA
        // depth, [3]=single-sample depth resolve (render-pass2 path only). At 1x: [0]=colour(=resolve),
        // [1]=depth.
        const bool hdr_rp2 = msaa && render_pass2_active_ && depth_resolve_view_;
        VkImageView attachments[4] = {
            msaa ? hdr_color_view_ : hdr_resolve_view_,   // [0] colour
            msaa ? hdr_resolve_view_ : depth_view_,        // [1] resolve (MSAA) or depth (1x)
            depth_view_,                                   // [2] depth (MSAA)
            depth_resolve_view_,                           // [3] resolved depth (render-pass2)
        };
        VkFramebufferCreateInfo info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        info.renderPass = hdr_render_pass_;
        info.attachmentCount = hdr_rp2 ? 4u : (msaa ? 3u : 2u);
        info.pAttachments = attachments;
        info.width = extent_.width;
        info.height = extent_.height;
        info.layers = 1;
        if (vkCreateFramebuffer(device_, &info, nullptr, &hdr_framebuffer_) != VK_SUCCESS) {
            destroy_hdr_targets();
            return false;
        }
        return true;
    }

    // AA index -> a device-supported MSAA sample count.
    VkSampleCountFlagBits choose_sample_count(int aa_index) const {
        std::uint32_t desired = 1;
        if (aa_index == 1) desired = 2;
        else if (aa_index == 2) desired = 4;
        else if (aa_index == 3) desired = 8;
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(physical_device_, &props);
        const VkSampleCountFlags supported = props.limits.framebufferColorSampleCounts;
        for (std::uint32_t s = desired; s > 1; s /= 2) {
            const auto bit = static_cast<VkSampleCountFlagBits>(s);
            if (supported & bit) return bit;
        }
        return VK_SAMPLE_COUNT_1_BIT;
    }

    // (Re)create the multisampled color image the MSAA render pass renders into (resolved to the
    // swapchain image). Released at 1x. Sized to the current swapchain extent.
    bool create_msaa_image() {
        destroy_msaa_image();
        if (msaa_samples_ <= VK_SAMPLE_COUNT_1_BIT) return true;
        VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        image_info.imageType = VK_IMAGE_TYPE_2D;
        image_info.format = surface_format_.format;
        image_info.extent = {extent_.width, extent_.height, 1};
        image_info.mipLevels = 1;
        image_info.arrayLayers = 1;
        image_info.samples = msaa_samples_;
        image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
        image_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
        image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateImage(device_, &image_info, nullptr, &msaa_image_) != VK_SUCCESS) return false;
        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(device_, msaa_image_, &requirements);
        VkPhysicalDeviceMemoryProperties memory_properties{};
        vkGetPhysicalDeviceMemoryProperties(physical_device_, &memory_properties);
        std::uint32_t memory_type = 0;
        bool found = false;
        for (std::uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i) {
            if ((requirements.memoryTypeBits & (1u << i)) &&
                (memory_properties.memoryTypes[i].propertyFlags &
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
                memory_type = i;
                found = true;
                break;
            }
        }
        if (!found) { destroy_msaa_image(); return false; }
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = memory_type;
        if (vkAllocateMemory(device_, &allocation, nullptr, &msaa_memory_) != VK_SUCCESS ||
            vkBindImageMemory(device_, msaa_image_, msaa_memory_, 0) != VK_SUCCESS) {
            destroy_msaa_image();
            return false;
        }
        VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view_info.image = msaa_image_;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = surface_format_.format;
        view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view_info.subresourceRange.levelCount = 1;
        view_info.subresourceRange.layerCount = 1;
        if (vkCreateImageView(device_, &view_info, nullptr, &msaa_view_) != VK_SUCCESS) {
            destroy_msaa_image();
            return false;
        }
        return true;
    }

    void destroy_msaa_image() {
        if (msaa_view_) vkDestroyImageView(device_, msaa_view_, nullptr);
        if (msaa_image_) vkDestroyImage(device_, msaa_image_, nullptr);
        if (msaa_memory_) vkFreeMemory(device_, msaa_memory_, nullptr);
        msaa_view_ = VK_NULL_HANDLE;
        msaa_image_ = VK_NULL_HANDLE;
        msaa_memory_ = VK_NULL_HANDLE;
    }

    // World depth buffer. Single image shared across framebuffers (like msaa_image_); its
    // sample count must match the colour attachment (msaa) for a valid render pass.
    bool create_depth_image() {
        destroy_depth_image();
        VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        image_info.imageType = VK_IMAGE_TYPE_2D;
        image_info.format = kDepthFormat;
        image_info.extent = {extent_.width, extent_.height, 1};
        image_info.mipLevels = 1;
        image_info.arrayLayers = 1;
        image_info.samples = msaa_samples_ > VK_SAMPLE_COUNT_1_BIT ? msaa_samples_
                                                                   : VK_SAMPLE_COUNT_1_BIT;
        image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
        image_info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateImage(device_, &image_info, nullptr, &depth_image_) != VK_SUCCESS) return false;
        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(device_, depth_image_, &requirements);
        VkPhysicalDeviceMemoryProperties memory_properties{};
        vkGetPhysicalDeviceMemoryProperties(physical_device_, &memory_properties);
        std::uint32_t memory_type = 0;
        bool found = false;
        for (std::uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i) {
            if ((requirements.memoryTypeBits & (1u << i)) &&
                (memory_properties.memoryTypes[i].propertyFlags &
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
                memory_type = i;
                found = true;
                break;
            }
        }
        if (!found) { destroy_depth_image(); return false; }
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = memory_type;
        if (vkAllocateMemory(device_, &allocation, nullptr, &depth_memory_) != VK_SUCCESS ||
            vkBindImageMemory(device_, depth_image_, depth_memory_, 0) != VK_SUCCESS) {
            destroy_depth_image();
            return false;
        }
        VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view_info.image = depth_image_;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = kDepthFormat;
        view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        view_info.subresourceRange.levelCount = 1;
        view_info.subresourceRange.layerCount = 1;
        if (vkCreateImageView(device_, &view_info, nullptr, &depth_view_) != VK_SUCCESS) {
            destroy_depth_image();
            return false;
        }
        return true;
    }

    void destroy_depth_image() {
        destroy_depth_resolve_image();
        if (depth_view_) vkDestroyImageView(device_, depth_view_, nullptr);
        if (depth_image_) vkDestroyImage(device_, depth_image_, nullptr);
        if (depth_memory_) vkFreeMemory(device_, depth_memory_, nullptr);
        depth_view_ = VK_NULL_HANDLE;
        depth_image_ = VK_NULL_HANDLE;
        depth_memory_ = VK_NULL_HANDLE;
    }

    void destroy_depth_resolve_image() {
        if (depth_resolve_view_) vkDestroyImageView(device_, depth_resolve_view_, nullptr);
        if (depth_resolve_image_) vkDestroyImage(device_, depth_resolve_image_, nullptr);
        if (depth_resolve_memory_) vkFreeMemory(device_, depth_resolve_memory_, nullptr);
        depth_resolve_view_ = VK_NULL_HANDLE;
        depth_resolve_image_ = VK_NULL_HANDLE;
        depth_resolve_memory_ = VK_NULL_HANDLE;
    }

    bool create_depth_resolve_image() {
        if (!depth_resolve_supported_ || msaa_samples_ <= VK_SAMPLE_COUNT_1_BIT) return true;
        VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        image_info.imageType = VK_IMAGE_TYPE_2D;
        image_info.format = kDepthFormat;
        image_info.extent = {extent_.width, extent_.height, 1};
        image_info.mipLevels = 1;
        image_info.arrayLayers = 1;
        image_info.samples = VK_SAMPLE_COUNT_1_BIT;
        image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
        image_info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                           VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
        image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateImage(device_, &image_info, nullptr, &depth_resolve_image_) != VK_SUCCESS) {
            depth_resolve_supported_ = false;
            return true;
        }
        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(device_, depth_resolve_image_, &requirements);
        VkPhysicalDeviceMemoryProperties memory_properties{};
        vkGetPhysicalDeviceMemoryProperties(physical_device_, &memory_properties);
        std::uint32_t memory_type = 0;
        bool found = false;
        for (std::uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i) {
            if ((requirements.memoryTypeBits & (1u << i)) &&
                (memory_properties.memoryTypes[i].propertyFlags &
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
                memory_type = i;
                found = true;
                break;
            }
        }
        if (!found) {
            destroy_depth_resolve_image();
            depth_resolve_supported_ = false;
            return true;
        }
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = memory_type;
        if (vkAllocateMemory(device_, &allocation, nullptr, &depth_resolve_memory_) != VK_SUCCESS ||
            vkBindImageMemory(device_, depth_resolve_image_, depth_resolve_memory_, 0) != VK_SUCCESS) {
            destroy_depth_resolve_image();
            depth_resolve_supported_ = false;
            return true;
        }
        VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view_info.image = depth_resolve_image_;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = kDepthFormat;
        view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        view_info.subresourceRange.levelCount = 1;
        view_info.subresourceRange.layerCount = 1;
        if (vkCreateImageView(device_, &view_info, nullptr, &depth_resolve_view_) != VK_SUCCESS) {
            destroy_depth_resolve_image();
            depth_resolve_supported_ = false;
            return true;
        }
        return true;
    }

    bool create_framebuffers() {
        const bool msaa = msaa_samples_ > VK_SAMPLE_COUNT_1_BIT;
        if (!create_msaa_image() || !create_depth_image() || !create_depth_resolve_image()) return false;
        if (render_pass2_active_ && !depth_resolve_view_) {
            // The resolve image is optional. If allocation failed after render-pass2 creation,
            // rebuild the legacy two-subpass pass so the remaining attachments still match.
            vkDestroyRenderPass(device_, render_pass_, nullptr);
            render_pass_ = VK_NULL_HANDLE;
            render_pass2_active_ = false;
            depth_resolve_supported_ = false;
            if (!create_render_pass()) return false;
        }
        framebuffers_.resize(swapchain_views_.size());
        for (std::size_t index = 0; index < swapchain_views_.size(); ++index) {
            // Order must match the render pass: [0]=color, [1]=color resolve, [2]=MSAA depth,
            // [3]=single-sample depth resolve when the optional render-pass2 path is active.
            const VkImageView attachments[4] = {
                msaa ? msaa_view_ : swapchain_views_[index],   // [0] color
                msaa ? swapchain_views_[index] : depth_view_,  // [1] resolve (MSAA) or depth
                depth_view_,                                   // [2] depth (MSAA only)
                depth_resolve_view_,                            // [3] resolved depth (optional)
            };
            VkFramebufferCreateInfo info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            info.renderPass = render_pass_;
            info.attachmentCount = depth_resolve_view_ ? 4u : (msaa ? 3u : 2u);
            info.pAttachments = attachments;
            info.width = extent_.width;
            info.height = extent_.height;
            info.layers = 1;
            if (vkCreateFramebuffer(device_, &info, nullptr, &framebuffers_[index]) != VK_SUCCESS) return false;
        }
        return true;
    }

    bool create_native_ui() {
        // Pool for the UI renderer's per-(primary,detail) descriptor sets (2 combined image samplers
        // each). Sized for all UI textures + font + video pairs actually used across the frontend.
        VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 512};
        VkDescriptorPoolCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        info.maxSets = 256;
        info.poolSizeCount = 1;
        info.pPoolSizes = &pool_size;
        if (vkCreateDescriptorPool(device_, &info, nullptr, &ui_descriptor_pool_) != VK_SUCCESS)
            return false;

        std::string error;
        if (!native_ui_renderer_.initialise(physical_device_, device_, render_pass_,
                                            static_cast<std::uint32_t>(swapchain_images_.size()),
                                            msaa_samples_,
                                            std::filesystem::path(F2NATIVE_VULKAN_SHADER_DIR),
                                            error)) {
            if (window_)
                MessageBoxA(window_, error.c_str(), "Fable II Native - Vulkan UI",
                            MB_OK | MB_ICONERROR);
            return false;
        }

        // Bridge logical UI assets to the shared scene builder's backend-neutral TextureIds; the app
        // owns the Vulkan textures and resolves ids -> descriptor sets at draw time.
        f2::FrontendSceneBuilder::TextureAccess access;
        access.id = [this](f2::NativeUiAsset asset) -> f2::render::TextureId {
            const auto slot = ui_slot(asset);
            return ui_textures_[slot].is_ready() ? static_cast<f2::render::TextureId>(slot + 1)
                                                 : f2::render::kInvalidTexture;
        };
        access.shader_ready = [this](f2::NativeUiAsset asset) {
            return ui_textures_[ui_slot(asset)].is_ready();
        };
        access.font_id = [this]() -> f2::render::TextureId {
            return font_texture_.is_ready() ? kFontTextureId : f2::render::kInvalidTexture;
        };
        access.font_ready = [this]() { return font_texture_.is_ready(); };
        scene_builder_.emplace(native_font_, ui_assets_, std::move(access));
        return true;
    }

    // Resolve a scene TextureId to its Vulkan (view, sampler). Invalid id -> not-ready texture.
    const f2::NativeVulkanVideoTexture* texture_for_id(f2::render::TextureId id) const {
        if (id == f2::render::kInvalidTexture) return nullptr;
        if (id == kFontTextureId) return &font_texture_;
        if (id == kVideoTextureId) return &video_texture_;
        const auto slot = static_cast<std::size_t>(id - 1);
        return slot < ui_textures_.size() ? &ui_textures_[slot] : nullptr;
    }

    // Descriptor set (binding0=primary, binding1=detail) for a scene quad, cached by id pair.
    VkDescriptorSet descriptor_set_for(f2::render::TextureId primary, f2::render::TextureId detail) {
        const auto* primary_texture = texture_for_id(primary);
        if (!primary_texture || !primary_texture->is_ready()) return VK_NULL_HANDLE;
        const auto* detail_texture = texture_for_id(detail);
        if (!detail_texture || !detail_texture->is_ready()) detail_texture = primary_texture;
        const std::uint64_t key = (static_cast<std::uint64_t>(primary) << 32) | detail;
        if (const auto it = descriptor_cache_.find(key); it != descriptor_cache_.end())
            return it->second;

        VkDescriptorSetLayout layout = native_ui_renderer_.descriptor_set_layout();
        VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        alloc.descriptorPool = ui_descriptor_pool_;
        alloc.descriptorSetCount = 1;
        alloc.pSetLayouts = &layout;
        VkDescriptorSet set = VK_NULL_HANDLE;
        if (vkAllocateDescriptorSets(device_, &alloc, &set) != VK_SUCCESS) return VK_NULL_HANDLE;
        const VkDescriptorImageInfo images[2] = {
            {primary_texture->sampler(), primary_texture->view(),
             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {detail_texture->sampler(), detail_texture->view(),
             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
        VkWriteDescriptorSet writes[2]{};
        for (std::uint32_t i = 0; i < 2; ++i) {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = set;
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[i].pImageInfo = &images[i];
        }
        vkUpdateDescriptorSets(device_, 2, writes, 0, nullptr);
        descriptor_cache_.emplace(key, set);
        return set;
    }

    // Cached descriptor sets reference texture views; drop them when a texture is (re)created.
    void reset_descriptor_cache() {
        if (ui_descriptor_pool_ != VK_NULL_HANDLE) vkResetDescriptorPool(device_, ui_descriptor_pool_, 0);
        descriptor_cache_.clear();
    }

    bool fail(const char* prefix, VkResult result) {
        const std::string message = std::string(prefix) + vulkan_result_name(result);
        if (window_) MessageBoxA(window_, message.c_str(), "Fable II Native - Vulkan error", MB_OK | MB_ICONERROR);
        return false;
    }

    void handle_input() {
        using Action = f2::NativeInputAction;
        const auto play_sound = [&](f2::NativeFrontendSound sound) {
            if (game_.frontend.sound_enabled()) audio_.play(sound);
        };
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

    // Keep Vulkan's inspection camera identical to the D3D12 frontend: WASD moves on the view
    // plane, Q/E move vertically, arrows look, and Shift boosts. This is deliberately independent
    // of menu bindings so it remains useful while inspecting cooked geometry in World state.
    void update_free_camera(double delta) {
        if (game_.frontend.state() != f2::FrontendState::World) {
            world_cam_initialised_ = false;
            world_renderer_.clear_free_camera();
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
            game_.camera.yaw = 0.6f;
            game_.camera.pitch = -0.32f;
            const float cp = std::cos(game_.camera.pitch);
            const std::array<float, 3> fwd{cp * std::sin(game_.camera.yaw),
                                           std::sin(game_.camera.pitch),
                                           cp * std::cos(game_.camera.yaw)};
            const float dist = hero_view ? 8.0f : radius * 2.4f;
            game_.camera.position = {center[0] - fwd[0] * dist,
                                     center[1] - fwd[1] * dist,
                                     center[2] - fwd[2] * dist};
            world_cam_initialised_ = true;
        }
        const bool focused = GetForegroundWindow() == window_;
        const auto down = [&](int vk) {
            return focused && (GetAsyncKeyState(vk) & 0x8000) != 0;
        };
        const float dt = static_cast<float>(delta);
        const float look = 1.6f * dt;
        if (down(VK_LEFT)) game_.camera.yaw -= look;
        if (down(VK_RIGHT)) game_.camera.yaw += look;
        if (down(VK_UP)) game_.camera.pitch += look;
        if (down(VK_DOWN)) game_.camera.pitch -= look;
        game_.camera.pitch = std::clamp(game_.camera.pitch, -1.55f, 1.55f);
        const float cp = std::cos(game_.camera.pitch);
        const std::array<float, 3> fwd{cp * std::sin(game_.camera.yaw),
                                       std::sin(game_.camera.pitch),
                                       cp * std::cos(game_.camera.yaw)};
        const std::array<float, 3> world_up{0.0f, 1.0f, 0.0f};
        std::array<float, 3> right{fwd[1] * world_up[2] - fwd[2] * world_up[1],
                                   fwd[2] * world_up[0] - fwd[0] * world_up[2],
                                   fwd[0] * world_up[1] - fwd[1] * world_up[0]};
        const float right_length = std::sqrt(right[0] * right[0] + right[1] * right[1] +
                                             right[2] * right[2]);
        if (right_length > 1e-4f) {
            for (auto& component : right) component /= right_length;
        }
        const float boost = down(VK_SHIFT) ? 4.0f : 1.0f;
        const float speed = std::max(radius * 0.35f, 2.0f) * boost * dt;
        auto& position = game_.camera.position;
        const auto move = [&](const std::array<float, 3>& direction, float amount) {
            for (std::size_t index = 0; index < 3; ++index) position[index] += direction[index] * amount;
        };
        if (down('W')) move(fwd, speed);
        if (down('S')) move(fwd, -speed);
        if (down('D')) move(right, speed);
        if (down('A')) move(right, -speed);
        if (down('E')) move(world_up, speed);
        if (down('Q')) move(world_up, -speed);
        world_renderer_.set_free_camera(position, game_.camera.yaw, game_.camera.pitch);
    }

    // Match the D3D12 inspection control: IJKL moves hero draw ranges, U/O adjusts height,
    // and Shift boosts. The static world buffers remain untouched.
    void update_character_controller(double delta) {
        if (game_.frontend.state() != f2::FrontendState::World || !scene_has_hero()) {
            character_offset_ = {0.0f, 0.0f, 0.0f};
            character_motion_phase_ = 0.0f;
            character_motion_strength_ = 0.0f;
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

    // Apply the Options "Resolution" setting by resizing the window; the WM_SIZE handler flags a
    // framebuffer resize and draw() recreates the swapchain (mirrors the D3D12 frontend).
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
            if (video_texture_.is_ready()) {
                vkDeviceWaitIdle(device_);
                video_texture_.destroy();
                reset_descriptor_cache();
                uploaded_video_serial_ = 0;
                video_next_frame_time_ = 0.0;
            }
            return;
        }
        const auto* clip = state == f2::FrontendState::IntroVideo
                               ? game_.frontend.intro_videos().current_clip()
                               : game_.frontend.attract_videos().current_clip();
        if (!clip) return;
        const auto path = video_root_ / clip->asset;
        if (path != active_video_path_) {
            audio_.stop_video_audio();
            video_decoder_.close();
            active_video_path_ = path;
            video_error_.clear();
            video_next_frame_time_ = 0.0;
            if (!video_decoder_.open(path, video_error_)) return;
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


    bool ensure_video_texture() {
        if (video_frame_.serial == 0) return false;
        if (video_texture_.is_ready() && !video_texture_.matches(video_frame_)) {
            vkDeviceWaitIdle(device_);
            video_texture_.destroy();
            reset_descriptor_cache();
        }
        if (!video_texture_.is_ready()) {
            if (!video_texture_.initialise(physical_device_, device_, command_pool_, queue_,
                                           video_frame_, video_error_)) return false;
            reset_descriptor_cache();  // new view -> invalidate cached sets
            uploaded_video_serial_ = video_frame_.serial;
            return true;
        }
        if (video_frame_.serial != uploaded_video_serial_) {
            if (!video_texture_.update(video_frame_, video_error_)) return false;  // same view, pixels only
            uploaded_video_serial_ = video_frame_.serial;
        }
        return true;
    }

    // Upload one UI asset into its slot texture (fixed size, so this inits once and never re-inits).
    bool ensure_ui_texture(f2::NativeUiAsset asset) {
        const auto* source = ui_assets_.texture(asset);
        if (!source) return false;
        auto& texture = ui_textures_[ui_slot(asset)];
        if (texture.is_ready() && texture.width() == source->width &&
            texture.height() == source->height) return true;
        if (texture.is_ready()) {
            vkDeviceWaitIdle(device_);
            texture.destroy();
            reset_descriptor_cache();
        }
        f2::NativeVideoFrame frame;
        frame.width = source->width;
        frame.height = source->height;
        frame.serial = 1;
        frame.rgba8 = source->rgba8;
        std::string error;
        if (!texture.initialise(physical_device_, device_, command_pool_, queue_, frame, error)) {
            video_error_ = std::move(error);
            return false;
        }
        reset_descriptor_cache();
        return true;
    }

    // Rasterize the native font once and upload its atlas (mirrors the D3D12 frontend).
    bool ensure_font_texture() {
        if (font_texture_.is_ready()) return true;
        if (!native_font_.ready()) {
            std::string error;
            bool loaded = false;
            if (!ui_assets_.title_font_path().empty())
                loaded = native_font_.load(ui_assets_.title_font_path(), 48.0f, error);
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
        f2::NativeVideoFrame frame;
        frame.width = native_font_.atlas_width();
        frame.height = native_font_.atlas_height();
        frame.serial = 1;
        frame.rgba8 = native_font_.atlas_rgba8();
        std::string error;
        if (!font_texture_.initialise(physical_device_, device_, command_pool_, queue_, frame, error))
            return false;
        reset_descriptor_cache();
        return true;
    }

    void ensure_ui_textures() {
        ensure_font_texture();
        for (const auto asset :
             {f2::NativeUiAsset::TitleBackground, f2::NativeUiAsset::MainBackground,
              f2::NativeUiAsset::Logo, f2::NativeUiAsset::Accept, f2::NativeUiAsset::Back,
              f2::NativeUiAsset::MenuSurface, f2::NativeUiAsset::Sparkle1, f2::NativeUiAsset::Sparkle2,
              f2::NativeUiAsset::Sparkle3, f2::NativeUiAsset::Sparkle4, f2::NativeUiAsset::Sparkle5,
              f2::NativeUiAsset::Sparkle6, f2::NativeUiAsset::Sparkle7, f2::NativeUiAsset::Sparkle8,
              f2::NativeUiAsset::AmbientAtlas, f2::NativeUiAsset::AmbientBaseline,
              f2::NativeUiAsset::FrameElements, f2::NativeUiAsset::AbilityElements,
              f2::NativeUiAsset::MenuFrameOverlay, f2::NativeUiAsset::Frames04,
              f2::NativeUiAsset::CardBoy, f2::NativeUiAsset::CardGirl, f2::NativeUiAsset::CardBack,
              f2::NativeUiAsset::SideRailAtlas, f2::NativeUiAsset::MenuFrameLeftUpper,
              f2::NativeUiAsset::MenuFrameLeftLower, f2::NativeUiAsset::MenuFrameRightUpper,
              f2::NativeUiAsset::MenuFrameRightLower, f2::NativeUiAsset::FramesPage,
              f2::NativeUiAsset::FramesPageTexture, f2::NativeUiAsset::SliderFrame,
              f2::NativeUiAsset::Motifs, f2::NativeUiAsset::CalibrationImage,
              f2::NativeUiAsset::GoldCoin, f2::NativeUiAsset::LogoFlare}) {
            ensure_ui_texture(asset);
        }
    }

    void update_ambient_detail_frame() {
        if (game_.frontend.state() != f2::FrontendState::Title) return;
        auto& atlas = ui_textures_[ui_slot(f2::NativeUiAsset::AmbientAtlas)];
        const auto frame_count = ui_assets_.ambient_detail_frame_count();
        const auto* main = ui_assets_.texture(f2::NativeUiAsset::AmbientAtlas);
        if (frame_count == 0 || !main || !atlas.is_ready()) return;
        const auto elapsed = std::max(0.0, game_.frontend.state_time() - 5.90);
        const auto frame_index = static_cast<std::size_t>(std::floor(elapsed * 60.0)) % frame_count;
        if (frame_index == ambient_detail_frame_uploaded_) return;
        const auto* detail = ui_assets_.ambient_detail_frame(frame_index);
        f2::NativeTexture composed;
        if (!detail || !f2::compose_native_ambient_atlas(*main, *detail, composed)) return;
        f2::NativeVideoFrame frame;
        frame.width = composed.width;
        frame.height = composed.height;
        frame.serial = static_cast<std::uint64_t>(frame_index + 1);
        frame.rgba8 = std::move(composed.rgba8);
        std::string error;
        if (!atlas.update(frame, error)) {
            video_error_ = std::move(error);
            return;
        }
        ambient_detail_frame_uploaded_ = frame_index;
    }

    // Build the current frame's UI scene through the shared, backend-neutral FrontendSceneBuilder,
    // then draw it with the native Vulkan UI renderer (ImGui-free; identical scene to the D3D12 path).
    void build_scene(f2::render::UiDrawList& scene) {
        if (!scene_builder_) return;
        const auto state = game_.frontend.state();
        const float w = static_cast<float>(extent_.width);
        const float h = static_cast<float>(extent_.height);
        if (state == f2::FrontendState::MainMenu || state == f2::FrontendState::ChooseCard ||
            state == f2::FrontendState::Options) {
            scene_builder_->build_main_menu(scene, w, h, game_, input_.using_controller_prompts());
        } else if (state == f2::FrontendState::Title) {
            scene_builder_->build_title(scene, w, h, game_.frontend.state_time(),
                                        input_.prompt(f2::NativeInputAction::Accept),
                                        input_.using_controller_prompts());
        } else if (state == f2::FrontendState::IntroVideo ||
                   state == f2::FrontendState::AttractVideo) {
            scene_builder_->build_video(scene, w, h, kVideoTextureId);
        } else if (state == f2::FrontendState::Loading) {
            scene_builder_->build_loading(scene, w, h);
        }
        // Optional on-screen FPS counter (Video options toggle), over the font-ready frontend states.
        if (game_.frontend.fps_display_enabled() &&
            (state == f2::FrontendState::Title || state == f2::FrontendState::MainMenu ||
             state == f2::FrontendState::ChooseCard || state == f2::FrontendState::Options)) {
            scene_builder_->build_fps_overlay(scene, w, h, current_fps_);
        }
    }

    void build_world_overlay(f2::render::UiDrawList& scene) {
        if (!scene_builder_) return;
        scene_builder_->build_world_overlay(scene, static_cast<float>(extent_.width),
                                            static_cast<float>(extent_.height), scene_has_hero(),
                                            character_offset_, character_motion_strength_ > 0.5f);
    }

    void draw() {
        if (game_.frontend.state() == f2::FrontendState::IntroVideo ||
            game_.frontend.state() == f2::FrontendState::AttractVideo) ensure_video_texture();
        if (game_.frontend.state() == f2::FrontendState::Title ||
            game_.frontend.state() == f2::FrontendState::MainMenu ||
            game_.frontend.state() == f2::FrontendState::ChooseCard ||
            game_.frontend.state() == f2::FrontendState::Options) ensure_ui_textures();
        else if (game_.frontend.state() == f2::FrontendState::World) ensure_font_texture();
        update_ambient_detail_frame();
        if (framebuffer_resized_) recreate_swapchain();
        vkWaitForFences(device_, 1, &in_flight_[current_frame_], VK_TRUE, UINT64_MAX);
        std::uint32_t image_index = 0;
        VkResult result = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX,
                                                image_available_[current_frame_], VK_NULL_HANDLE, &image_index);
        if (result == VK_ERROR_OUT_OF_DATE_KHR) {
            recreate_swapchain();
            return;
        }
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) return;
        vkResetFences(device_, 1, &in_flight_[current_frame_]);
        vkResetCommandBuffer(command_buffers_[image_index], 0);
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        if (vkBeginCommandBuffer(command_buffers_[image_index], &begin) != VK_SUCCESS) return;
        VkClearValue clear{};
        if (game_.frontend.state() == f2::FrontendState::Title) {
            // BLACK behind the title wordmark before the panorama fades in (matches D3D12; the
            // Vulkan clear previously used sky_color, which showed as blue here).
            clear.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
        } else if (game_.frontend.state() == f2::FrontendState::World) {
            clear.color = {{game_.scene.sky_color[0], game_.scene.sky_color[1],
                            game_.scene.sky_color[2], 1.0f}};
        } else {
            clear.color = {{0.015f, 0.02f, 0.035f, 1.0f}};
        }
        // Attachments: [0]=color, [1]=(resolve MSAA / depth no-MSAA), [2]=depth (MSAA). Give a
        // clear value per attachment; the depth slot clears to 1.0 (far).
        const bool msaa_clear = msaa_samples_ > VK_SAMPLE_COUNT_1_BIT;
        VkClearValue depth_clear{};
        depth_clear.depthStencil = {0.0f, 0};  // reversed-Z far value
        VkClearValue clears[3] = {clear, msaa_clear ? clear : depth_clear, depth_clear};
        // World HDR path: the World passes render into the HDR scene target (hdr_render_pass_ /
        // hdr_framebuffer_) exactly as they used to render into render_pass_ (same subpass layout);
        // the compositor then resolves to the swap-chain image, and the World UI overlay draws in the
        // swap-chain pass afterwards. Non-World states, and the fallback when HDR is unavailable, keep
        // the original single-pass path into render_pass_.
        const bool use_hdr = game_.frontend.state() == f2::FrontendState::World && hdr_enabled_;
        VkRenderPassBeginInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        pass.renderPass = use_hdr ? hdr_render_pass_ : render_pass_;
        pass.framebuffer = use_hdr ? hdr_framebuffer_ : framebuffers_[image_index];
        pass.renderArea.extent = extent_;
        pass.clearValueCount = msaa_clear ? 3u : 2u;
        pass.pClearValues = clears;
        vkCmdBeginRenderPass(command_buffers_[image_index], &pass, VK_SUBPASS_CONTENTS_INLINE);
        if (game_.frontend.state() == f2::FrontendState::World) {
            if (sky_renderer_.ready()) {
                const f2::SkyCamera sky_camera = world_renderer_.compute_camera(
                    extent_.width, extent_.height, game_.elapsed_seconds);
                sky_renderer_.render(command_buffers_[image_index], extent_.width, extent_.height,
                                     game_.scene, sky_camera);
            }
            // Cloud layers over the sky, still behind the world (same opaque subpass).
            if (cloud_renderer_.ready() && !game_.scene.clouds.empty()) {
                const f2::SkyCamera cam = world_renderer_.compute_camera(
                    extent_.width, extent_.height, game_.elapsed_seconds);
                const auto vp = world_renderer_.compute_view_projection(
                    extent_.width, extent_.height, game_.elapsed_seconds);
                cloud_renderer_.render(command_buffers_[image_index], extent_.width, extent_.height,
                                       game_.scene, vp, cam.position, cam.forward,
                                       game_.elapsed_seconds);
            }
            // Celestial billboards (night moon + glare) over the clouds, behind the world.
            if (billboard_renderer_.ready() && game_.scene.has_moon) {
                const f2::SkyCamera cam = world_renderer_.compute_camera(
                    extent_.width, extent_.height, game_.elapsed_seconds);
                billboard_renderer_.render(command_buffers_[image_index], extent_.width,
                                           extent_.height, game_.scene, cam);
            }
            // Procedural night stars, drawn last of the sky passes (retail order), behind the world.
            if (stars_renderer_.ready() && game_.scene.star_brightness > 0.0f) {
                const f2::SkyCamera cam = world_renderer_.compute_camera(
                    extent_.width, extent_.height, game_.elapsed_seconds);
                stars_renderer_.render(command_buffers_[image_index], extent_.width, extent_.height,
                                       game_.scene, cam, game_.elapsed_seconds);
            }
            // World-overlay UI: baked against the swap-chain render_pass_, so in the HDR path it is
            // deferred to the swap-chain pass after compositing (mirrors D3D12); in the fallback
            // path it draws in the (swap-chain) world pass exactly as before.
            const auto draw_world_overlay = [&]() {
                f2::render::UiDrawList overlay;
                build_world_overlay(overlay);
                if (overlay.empty()) return;
                native_ui_renderer_.render(
                    command_buffers_[image_index], current_frame_, extent_.width, extent_.height,
                    overlay.quads(),
                    [this](f2::render::TextureId primary, f2::render::TextureId detail) {
                        return descriptor_set_for(primary, detail);
                    });
            };
            if (msaa_clear) {
                world_renderer_.render_opaque(command_buffers_[image_index], extent_.width,
                                              extent_.height, game_.elapsed_seconds);
                if (!use_hdr) draw_world_overlay();
                vkCmdNextSubpass(command_buffers_[image_index], VK_SUBPASS_CONTENTS_INLINE);
                world_renderer_.render_water(command_buffers_[image_index], extent_.width,
                                             extent_.height, game_.elapsed_seconds);
            } else {
                world_renderer_.render(command_buffers_[image_index], extent_.width,
                                       extent_.height, game_.elapsed_seconds);
                if (!use_hdr) draw_world_overlay();
            }
            if (use_hdr) {
                // End the HDR world pass, run the bloom chain (its own passes), then composite the
                // HDR scene down to the swap-chain image and draw the World UI overlay on top.
                vkCmdEndRenderPass(command_buffers_[image_index]);
                tonemap_renderer_.render_bloom(command_buffers_[image_index], hdr_bloom_threshold_,
                                               hdr_bloom_intensity_);
                VkClearValue swap_clears[3] = {clear, msaa_clear ? clear : depth_clear, depth_clear};
                VkRenderPassBeginInfo swap_pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
                swap_pass.renderPass = render_pass_;
                swap_pass.framebuffer = framebuffers_[image_index];
                swap_pass.renderArea.extent = extent_;
                swap_pass.clearValueCount = msaa_clear ? 3u : 2u;
                swap_pass.pClearValues = swap_clears;
                vkCmdBeginRenderPass(command_buffers_[image_index], &swap_pass,
                                     VK_SUBPASS_CONTENTS_INLINE);
                tonemap_renderer_.composite(command_buffers_[image_index], extent_.width,
                                            extent_.height, hdr_exposure_, hdr_bloom_intensity_);
                draw_world_overlay();
                // MSAA swap-chain pass resolves in subpass 1; advance so the composited output reaches
                // the swap-chain image (no water draw here — same as the frontend UI path).
                if (msaa_clear) {
                    vkCmdNextSubpass(command_buffers_[image_index], VK_SUBPASS_CONTENTS_INLINE);
                }
            }
        } else {
            f2::render::UiDrawList scene;
            build_scene(scene);
            if (!scene.empty()) {
                native_ui_renderer_.render(
                    command_buffers_[image_index], current_frame_, extent_.width, extent_.height,
                    scene.quads(), [this](f2::render::TextureId primary, f2::render::TextureId detail) {
                        return descriptor_set_for(primary, detail);
                    });
            }
            // MSAA resolves color only from subpass 1. Frontend UI has no water draw, but still
            // advances through the resolve subpass so its multisample output reaches the swapchain.
            if (msaa_clear) {
                vkCmdNextSubpass(command_buffers_[image_index], VK_SUBPASS_CONTENTS_INLINE);
            }
        }
        vkCmdEndRenderPass(command_buffers_[image_index]);
        if (vkEndCommandBuffer(command_buffers_[image_index]) != VK_SUCCESS) return;
        VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &image_available_[current_frame_];
        submit.pWaitDstStageMask = &wait_stage;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command_buffers_[image_index];
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &render_finished_[current_frame_];
        if (vkQueueSubmit(queue_, 1, &submit, in_flight_[current_frame_]) != VK_SUCCESS) return;
        VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &render_finished_[current_frame_];
        present.swapchainCount = 1;
        present.pSwapchains = &swapchain_;
        present.pImageIndices = &image_index;
        result = vkQueuePresentKHR(queue_, &present);
        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR || framebuffer_resized_) {
            framebuffer_resized_ = false;
            recreate_swapchain();
        }
        current_frame_ = (current_frame_ + 1) % kFrameCount;
    }

    void cleanup_swapchain() {
        for (const auto framebuffer : framebuffers_) vkDestroyFramebuffer(device_, framebuffer, nullptr);
        framebuffers_.clear();
        for (const auto view : swapchain_views_) vkDestroyImageView(device_, view, nullptr);
        swapchain_views_.clear();
        if (swapchain_) vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }

    void recreate_swapchain() {
        if (!device_) return;
        vkDeviceWaitIdle(device_);
        cleanup_swapchain();
        if (!create_swapchain() || !create_framebuffers()) PostMessageA(window_, WM_CLOSE, 0, 0);
        // The HDR scene target(s) + compositor bloom targets are extent-sized; rebuild them so the
        // World keeps compositing after a resize. On failure HDR disables and the World falls back.
        if (hdr_render_pass_) refresh_hdr_targets();
        framebuffer_resized_ = false;
    }

    void shutdown() {
        if (!instance_) {
            if (video_runtime_started_) {
                f2::stop_native_video_runtime();
                video_runtime_started_ = false;
            }
            return;
        }
        if (device_) vkDeviceWaitIdle(device_);
        native_ui_renderer_.destroy();
        for (auto& texture : ui_textures_) texture.destroy();
        font_texture_.destroy();
        video_texture_.destroy();
        video_decoder_.close();
        stars_renderer_.destroy();
        billboard_renderer_.destroy();
        cloud_renderer_.destroy();
        sky_renderer_.destroy();
        world_renderer_.destroy();
        tonemap_renderer_.destroy();
        destroy_hdr_targets();
        if (hdr_render_pass_) vkDestroyRenderPass(device_, hdr_render_pass_, nullptr);
        hdr_render_pass_ = VK_NULL_HANDLE;
        destroy_msaa_image();
        destroy_depth_image();
        if (ui_descriptor_pool_) vkDestroyDescriptorPool(device_, ui_descriptor_pool_, nullptr);
        if (device_) {
            cleanup_swapchain();
            if (render_pass_) vkDestroyRenderPass(device_, render_pass_, nullptr);
            if (command_pool_) vkDestroyCommandPool(device_, command_pool_, nullptr);
            for (std::uint32_t index = 0; index < kFrameCount; ++index) {
                if (image_available_[index]) vkDestroySemaphore(device_, image_available_[index], nullptr);
                if (render_finished_[index]) vkDestroySemaphore(device_, render_finished_[index], nullptr);
                if (in_flight_[index]) vkDestroyFence(device_, in_flight_[index], nullptr);
            }
            vkDestroyDevice(device_, nullptr);
        }
        if (surface_) vkDestroySurfaceKHR(instance_, surface_, nullptr);
        vkDestroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
        if (window_) DestroyWindow(window_);
        if (com_initialized_) CoUninitialize();
        if (video_runtime_started_) {
            f2::stop_native_video_runtime();
            video_runtime_started_ = false;
        }
        window_ = nullptr;
    }

    HWND window_ = nullptr;
    UINT width_ = 1280;
    UINT height_ = 720;
    double current_fps_ = 0.0;  // smoothed FPS for the optional on-screen counter
    int last_resolution_index_ = -1;  // tracks the applied Options "Resolution" value
    VkSampleCountFlagBits msaa_samples_ = VK_SAMPLE_COUNT_1_BIT;  // Options "Anti-Aliasing"
    VkImage msaa_image_ = VK_NULL_HANDLE;  // multisampled color target resolved to the swapchain image
    VkDeviceMemory msaa_memory_ = VK_NULL_HANDLE;
    VkImageView msaa_view_ = VK_NULL_HANDLE;
    static constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;  // world depth buffer
    VkImage depth_image_ = VK_NULL_HANDLE;
    VkDeviceMemory depth_memory_ = VK_NULL_HANDLE;
    VkImageView depth_view_ = VK_NULL_HANDLE;
    VkImage depth_resolve_image_ = VK_NULL_HANDLE;
    VkDeviceMemory depth_resolve_memory_ = VK_NULL_HANDLE;
    VkImageView depth_resolve_view_ = VK_NULL_HANDLE;
    bool depth_resolve_supported_ = false;
    bool render_pass2_active_ = false;
    PFN_vkCreateRenderPass2 create_render_pass2_ = nullptr;
    bool framebuffer_resized_ = false;
    bool com_initialized_ = false;
    bool video_runtime_started_ = false;
    std::optional<f2::GameSource> source_;
    std::filesystem::path video_root_;
    std::filesystem::path ui_root_;
    f2::NativeUiAssets ui_assets_;
    f2::NativeFrontendAudio audio_;
    std::filesystem::path active_video_path_;
    f2::NativeVideoDecoder video_decoder_;
    f2::NativeVideoFrame video_frame_;
    f2::NativeVulkanVideoTexture video_texture_;
    // Slot-indexed UI textures (same ui_slot mapping as D3D12) + the native font atlas, drawn by the
    // native Vulkan UI renderer from the shared scene builder. Descriptor sets are cached per (primary,
    // detail) TextureId pair, allocated from ui_descriptor_pool_.
    std::array<f2::NativeVulkanVideoTexture, kUiTextureCount> ui_textures_;
    f2::NativeFont native_font_;
    f2::NativeVulkanVideoTexture font_texture_;
    f2::NativeVulkanUiRenderer native_ui_renderer_;
    std::optional<f2::FrontendSceneBuilder> scene_builder_;
    VkDescriptorPool ui_descriptor_pool_ = VK_NULL_HANDLE;
    std::unordered_map<std::uint64_t, VkDescriptorSet> descriptor_cache_;
    std::size_t ambient_detail_frame_uploaded_ = std::numeric_limits<std::size_t>::max();
    std::uint64_t uploaded_video_serial_ = 0;
    double video_next_frame_time_ = 0.0;
    std::string video_error_;
    f2::NativeVulkanWorldRenderer world_renderer_;
    f2::NativeVulkanSkyRenderer sky_renderer_;
    f2::NativeVulkanCloudRenderer cloud_renderer_;
    f2::NativeVulkanSkyBillboardRenderer billboard_renderer_;
    f2::NativeVulkanSkyStarsRenderer stars_renderer_;
    // HDR compositor: the World passes render into an RGBA16F scene target through hdr_render_pass_,
    // then tonemap_renderer_ resolves it (tonemap/exposure + bloom) to the LDR swap-chain image in
    // render_pass_. Mirrors the D3D12 path (rendering_pipeline.txt §D.3). When any HDR resource is
    // unavailable, the World branch falls back to the old direct-to-swapchain render_pass_ path.
    static constexpr VkFormat kHdrFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    f2::NativeVulkanTonemapRenderer tonemap_renderer_;
    bool hdr_enabled_ = false;  // HDR render pass + scene target + compositor all created
    VkRenderPass hdr_render_pass_ = VK_NULL_HANDLE;  // World-pass HDR pass (mirrors render_pass_)
    VkFramebuffer hdr_framebuffer_ = VK_NULL_HANDLE;
    VkImage hdr_color_image_ = VK_NULL_HANDLE;   // MSAA HDR colour (msaa) or single-sample HDR colour
    VkDeviceMemory hdr_color_memory_ = VK_NULL_HANDLE;
    VkImageView hdr_color_view_ = VK_NULL_HANDLE;
    VkImage hdr_resolve_image_ = VK_NULL_HANDLE;  // single-sample HDR resolve the compositor samples
    VkDeviceMemory hdr_resolve_memory_ = VK_NULL_HANDLE;
    VkImageView hdr_resolve_view_ = VK_NULL_HANDLE;  // the sampled scene view (== color view at 1x)
    float hdr_exposure_ = 1.0f;           // compositor exposure (1.0 == the old direct clamp)
    float hdr_bloom_threshold_ = 0.62f;   // HDR level above which bloom is extracted
    float hdr_bloom_intensity_ = 0.90f;   // bloom add strength (0 == byte-identical no-bloom)
    bool world_cam_initialised_ = false;
    std::array<float, 3> character_offset_{0.0f, 0.0f, 0.0f};
    float character_motion_phase_ = 0.0f;
    float character_motion_strength_ = 0.0f;
    f2::NativeGame game_;
    f2::NativeInputRouter input_;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    std::uint32_t queue_family_ = 0;
    VkCommandPool command_pool_ = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkSurfaceFormatKHR surface_format_{};
    VkExtent2D extent_{};
    std::vector<VkImage> swapchain_images_;
    std::vector<VkImageView> swapchain_views_;
    std::vector<VkFramebuffer> framebuffers_;
    VkRenderPass render_pass_ = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> command_buffers_;
    std::array<VkSemaphore, kFrameCount> image_available_{};
    std::array<VkSemaphore, kFrameCount> render_finished_{};
    std::array<VkFence, kFrameCount> in_flight_{};
    std::uint32_t current_frame_ = 0;
};

}  // namespace

int f2::run_vulkan_frontend(HINSTANCE instance) {
    FrontendApp app;
    return app.initialise(instance) ? app.run() : 1;
}
