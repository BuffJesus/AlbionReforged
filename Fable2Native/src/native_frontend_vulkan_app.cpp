#define VK_USE_PLATFORM_WIN32_KHR

#include "f2/native_game.h"
#include "f2/native_audio.h"
#include "f2/native_frontend_entry.h"
#include "f2/native_install.h"
#include "f2/native_input.h"
#include "f2/native_logo_effects.h"
#include "f2/native_ui.h"
#include "f2/native_video_decoder.h"
#include "f2/native_vulkan_video_texture.h"
#include "f2/native_vulkan_world_renderer.h"

#include "imgui.h"
#include "imgui_impl_vulkan.h"
#include "imgui_impl_win32.h"

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
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND window, UINT message,
                                               WPARAM wparam, LPARAM lparam);

namespace {

constexpr std::uint32_t kFrameCount = 2;

// Recovered from the retail ExpandableMenuFormatter LuaQ bytecode.  The
// visible slot is DisplayIndex + 4 - CurrentHighlightIdx; slots 1..3 and
// 10..12 form the curved, fading ends of the rail.
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
        if (!create_vulkan()) return false;
        std::string renderer_error;
        if (!world_renderer_.initialise(physical_device_, device_, command_pool_, queue_,
                                        render_pass_, surface_format_.format,
                                        source_ ? source_->data_root : std::filesystem::path{},
                                        F2NATIVE_VULKAN_SHADER_DIR, game_.scene,
                                        renderer_error)) {
            MessageBoxA(window_, renderer_error.c_str(), "Fable II Native - Vulkan renderer failed",
                        MB_OK | MB_ICONERROR);
            return false;
        }

        ShowWindow(window_, SW_SHOWDEFAULT);
        UpdateWindow(window_);
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        imgui_context_ = true;
        ImGui::StyleColorsDark();
        if (!ui_assets_.title_font_path().empty()) {
            const auto font_path = ui_assets_.title_font_path().string();
            if (auto* font = ImGui::GetIO().Fonts->AddFontFromFileTTF(font_path.c_str(), 26.0f)) {
                ImGui::GetIO().FontDefault = font;
            }
        }
        ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        ImGui_ImplWin32_Init(window_);

        if (!create_imgui()) return false;
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
        if (app && app->imgui_context_ &&
            ImGui_ImplWin32_WndProcHandler(window, message, wparam, lparam)) return 1;
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
        const char* device_extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        device_info.queueCreateInfoCount = 1;
        device_info.pQueueCreateInfos = &queue_info;
        device_info.enabledExtensionCount = 1;
        device_info.ppEnabledExtensionNames = device_extensions;
        result = vkCreateDevice(physical_device_, &device_info, nullptr, &device_);
        if (result != VK_SUCCESS) return fail("Vulkan device creation failed: ", result);
        vkGetDeviceQueue(device_, queue_family_, 0, &queue_);

        VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool_info.queueFamilyIndex = queue_family_;
        if (vkCreateCommandPool(device_, &pool_info, nullptr, &command_pool_) != VK_SUCCESS) {
            return false;
        }
        if (!create_swapchain() || !create_render_pass() || !create_framebuffers()) return false;

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

    bool create_render_pass() {
        VkAttachmentDescription color{};
        color.format = surface_format_.format;
        color.samples = VK_SAMPLE_COUNT_1_BIT;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &reference;
        VkSubpassDependency dependency{};
        dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
        dependency.dstSubpass = 0;
        dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        info.attachmentCount = 1;
        info.pAttachments = &color;
        info.subpassCount = 1;
        info.pSubpasses = &subpass;
        info.dependencyCount = 1;
        info.pDependencies = &dependency;
        return vkCreateRenderPass(device_, &info, nullptr, &render_pass_) == VK_SUCCESS;
    }

    bool create_framebuffers() {
        framebuffers_.resize(swapchain_views_.size());
        for (std::size_t index = 0; index < swapchain_views_.size(); ++index) {
            VkFramebufferCreateInfo info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            info.renderPass = render_pass_;
            info.attachmentCount = 1;
            info.pAttachments = &swapchain_views_[index];
            info.width = extent_.width;
            info.height = extent_.height;
            info.layers = 1;
            if (vkCreateFramebuffer(device_, &info, nullptr, &framebuffers_[index]) != VK_SUCCESS) return false;
        }
        return true;
    }

    bool create_imgui() {
        VkDescriptorPoolSize pool_sizes[] = {
            {VK_DESCRIPTOR_TYPE_SAMPLER, 1000},
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000},
            {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1000},
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1000},
            {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1000},
            {VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1000},
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1000},
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1000},
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1000},
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1000},
            {VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1000},
        };
        VkDescriptorPoolCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        info.maxSets = 1000 * static_cast<std::uint32_t>(std::size(pool_sizes));
        info.poolSizeCount = static_cast<std::uint32_t>(std::size(pool_sizes));
        info.pPoolSizes = pool_sizes;
        if (vkCreateDescriptorPool(device_, &info, nullptr, &imgui_descriptor_pool_) != VK_SUCCESS) return false;

        ImGui_ImplVulkan_InitInfo init_info{};
        init_info.Instance = instance_;
        init_info.PhysicalDevice = physical_device_;
        init_info.Device = device_;
        init_info.QueueFamily = queue_family_;
        init_info.Queue = queue_;
        init_info.DescriptorPool = imgui_descriptor_pool_;
        init_info.RenderPass = render_pass_;
        init_info.MinImageCount = kFrameCount;
        init_info.ImageCount = static_cast<std::uint32_t>(swapchain_images_.size());
        init_info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
        return ImGui_ImplVulkan_Init(&init_info);
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

    void update_video() {
        const auto state = game_.frontend.state();
        const bool video_state = state == f2::FrontendState::IntroVideo ||
                                  state == f2::FrontendState::AttractVideo;
        if (!video_state || video_root_.empty()) {
            video_decoder_.close();
            active_video_path_.clear();
            video_frame_ = {};
            video_next_frame_time_ = 0.0;
            if (video_texture_.is_ready() || video_descriptor_set_ != VK_NULL_HANDLE) {
                vkDeviceWaitIdle(device_);
                if (video_descriptor_set_ != VK_NULL_HANDLE) {
                    ImGui_ImplVulkan_RemoveTexture(video_descriptor_set_);
                    video_descriptor_set_ = VK_NULL_HANDLE;
                }
                video_texture_.destroy();
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
            video_decoder_.close();
            active_video_path_ = path;
            video_error_.clear();
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

    bool ensure_video_texture() {
        if (video_frame_.serial == 0) return false;
        if (video_texture_.is_ready() && !video_texture_.matches(video_frame_)) {
            vkDeviceWaitIdle(device_);
            if (video_descriptor_set_ != VK_NULL_HANDLE) {
                ImGui_ImplVulkan_RemoveTexture(video_descriptor_set_);
                video_descriptor_set_ = VK_NULL_HANDLE;
            }
            video_texture_.destroy();
        }
        if (!video_texture_.is_ready()) {
            if (!video_texture_.initialise(physical_device_, device_, command_pool_, queue_,
                                           video_frame_, video_error_)) return false;
            video_descriptor_set_ = ImGui_ImplVulkan_AddTexture(
                video_texture_.sampler(), video_texture_.view(),
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            if (video_descriptor_set_ == VK_NULL_HANDLE) {
                video_error_ = "Vulkan could not create the ImGui video descriptor.";
                video_texture_.destroy();
                return false;
            }
            uploaded_video_serial_ = video_frame_.serial;
            return true;
        }
        if (video_frame_.serial != uploaded_video_serial_) {
            if (!video_texture_.update(video_frame_, video_error_)) return false;
            uploaded_video_serial_ = video_frame_.serial;
        }
        return video_descriptor_set_ != VK_NULL_HANDLE;
    }

    bool ensure_ui_texture(f2::NativeUiAsset asset, f2::NativeVulkanVideoTexture& texture,
                           VkDescriptorSet& descriptor_set) {
        const auto* source = ui_assets_.texture(asset);
        if (!source) return false;
        if (texture.is_ready() && texture.width() == source->width &&
            texture.height() == source->height && descriptor_set != VK_NULL_HANDLE) return true;
        if (descriptor_set != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(device_);
            ImGui_ImplVulkan_RemoveTexture(descriptor_set);
            descriptor_set = VK_NULL_HANDLE;
        }
        texture.destroy();
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
        descriptor_set = ImGui_ImplVulkan_AddTexture(
            texture.sampler(), texture.view(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        if (descriptor_set == VK_NULL_HANDLE) {
            texture.destroy();
            video_error_ = "Vulkan could not create the ImGui UI descriptor.";
            return false;
        }
        return true;
    }

    void ensure_ui_textures() {
        ensure_ui_texture(f2::NativeUiAsset::TitleBackground, ui_background_texture_,
                          ui_background_descriptor_set_);
        ensure_ui_texture(f2::NativeUiAsset::MainBackground, ui_main_background_texture_,
                          ui_main_background_descriptor_set_);
        ensure_ui_texture(f2::NativeUiAsset::Logo, ui_logo_texture_, ui_logo_descriptor_set_);
        ensure_ui_texture(f2::NativeUiAsset::Accept, ui_accept_texture_,
                          ui_accept_descriptor_set_);
        ensure_ui_texture(f2::NativeUiAsset::Back, ui_back_texture_, ui_back_descriptor_set_);
        ensure_ui_texture(f2::NativeUiAsset::MenuSurface, ui_menu_surface_texture_,
                          ui_menu_surface_descriptor_set_);
        ensure_ui_texture(f2::NativeUiAsset::Sparkle1, ui_sparkle_textures_[0],
                          ui_sparkle_descriptor_sets_[0]);
        ensure_ui_texture(f2::NativeUiAsset::Sparkle2, ui_sparkle_textures_[1],
                          ui_sparkle_descriptor_sets_[1]);
        ensure_ui_texture(f2::NativeUiAsset::Sparkle3, ui_sparkle_textures_[2],
                          ui_sparkle_descriptor_sets_[2]);
        ensure_ui_texture(f2::NativeUiAsset::Sparkle4, ui_sparkle_textures_[3],
                          ui_sparkle_descriptor_sets_[3]);
        ensure_ui_texture(f2::NativeUiAsset::Sparkle5, ui_sparkle_textures_[4],
                          ui_sparkle_descriptor_sets_[4]);
        ensure_ui_texture(f2::NativeUiAsset::Sparkle6, ui_sparkle_textures_[5],
                          ui_sparkle_descriptor_sets_[5]);
        ensure_ui_texture(f2::NativeUiAsset::Sparkle7, ui_sparkle_textures_[6],
                          ui_sparkle_descriptor_sets_[6]);
        ensure_ui_texture(f2::NativeUiAsset::Sparkle8, ui_sparkle_textures_[7],
                          ui_sparkle_descriptor_sets_[7]);
        ensure_ui_texture(f2::NativeUiAsset::AmbientAtlas, ui_ambient_atlas_texture_,
                          ui_ambient_atlas_descriptor_set_);
        ensure_ui_texture(f2::NativeUiAsset::AmbientBaseline, ui_ambient_baseline_texture_,
                          ui_ambient_baseline_descriptor_set_);
        ensure_ui_texture(f2::NativeUiAsset::FrameElements, ui_frame_elements_texture_,
                          ui_frame_elements_descriptor_set_);
        ensure_ui_texture(f2::NativeUiAsset::AbilityElements, ui_ability_elements_texture_,
                          ui_ability_elements_descriptor_set_);
        ensure_ui_texture(f2::NativeUiAsset::MenuFrameOverlay, ui_menu_frame_texture_,
                          ui_menu_frame_descriptor_set_);
        ensure_ui_texture(f2::NativeUiAsset::MenuFrameLeftUpper, ui_menu_frame_left_upper_texture_,
                          ui_menu_frame_left_upper_descriptor_set_);
        ensure_ui_texture(f2::NativeUiAsset::MenuFrameLeftLower, ui_menu_frame_left_lower_texture_,
                          ui_menu_frame_left_lower_descriptor_set_);
        ensure_ui_texture(f2::NativeUiAsset::MenuFrameRightUpper, ui_menu_frame_right_upper_texture_,
                          ui_menu_frame_right_upper_descriptor_set_);
        ensure_ui_texture(f2::NativeUiAsset::MenuFrameRightLower, ui_menu_frame_right_lower_texture_,
                          ui_menu_frame_right_lower_descriptor_set_);
        ensure_ui_texture(f2::NativeUiAsset::Frames04, ui_frames_04_texture_,
                          ui_frames_04_descriptor_set_);
        ensure_ui_texture(f2::NativeUiAsset::CardBoy, ui_card_boy_texture_,
                          ui_card_boy_descriptor_set_);
        ensure_ui_texture(f2::NativeUiAsset::CardGirl, ui_card_girl_texture_,
                          ui_card_girl_descriptor_set_);
        ensure_ui_texture(f2::NativeUiAsset::SideRailAtlas, ui_side_rail_texture_,
                          ui_side_rail_descriptor_set_);
    }

    void update_ambient_detail_frame() {
        if (game_.frontend.state() != f2::FrontendState::Title) return;
        const auto frame_count = ui_assets_.ambient_detail_frame_count();
        const auto* main = ui_assets_.texture(f2::NativeUiAsset::AmbientAtlas);
        if (frame_count == 0 || !main || !ui_ambient_atlas_texture_.is_ready()) return;
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
        if (!ui_ambient_atlas_texture_.update(frame, error)) {
            video_error_ = std::move(error);
            return;
        }
        ambient_detail_frame_uploaded_ = frame_index;
    }

    void draw_ui() {
        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        const auto state = game_.frontend.state();
        if (state == f2::FrontendState::Boot || state == f2::FrontendState::IntroVideo ||
            state == f2::FrontendState::AttractVideo) {
            ImGui::SetNextWindowPos(ImVec2(0, 0));
            ImGui::SetNextWindowSize(ImVec2(static_cast<float>(width_), static_cast<float>(height_)));
            ImGui::Begin("##intro", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoSavedSettings);
            if ((state == f2::FrontendState::IntroVideo ||
                 state == f2::FrontendState::AttractVideo) &&
                video_descriptor_set_ != VK_NULL_HANDLE) {
                const float scale = std::max(width_ / static_cast<float>(video_frame_.width),
                                             height_ / static_cast<float>(video_frame_.height));
                const ImVec2 image_size(video_frame_.width * scale, video_frame_.height * scale);
                ImGui::SetCursorPos(ImVec2((width_ - image_size.x) * 0.5f,
                                           (height_ - image_size.y) * 0.5f));
                ImGui::Image(reinterpret_cast<ImTextureID>(video_descriptor_set_), image_size);
            }
            ImGui::End();
        } else if (state == f2::FrontendState::Title) {
            ImGui::SetNextWindowPos(ImVec2(0, 0));
            ImGui::SetNextWindowSize(ImVec2(static_cast<float>(width_), static_cast<float>(height_)));
            ImGui::Begin("##title", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoSavedSettings);
            auto* draw_list = ImGui::GetWindowDrawList();
            const float title_time = static_cast<float>(game_.frontend.state_time());
            const auto alpha = [](float value) {
                return static_cast<int>(std::clamp(value, 0.0f, 1.0f) * 255.0f);
            };
            draw_list->AddRectFilled(ImVec2(0, 0), ImVec2(width_, height_), IM_COL32(0, 0, 0, 255));
            if (ui_background_descriptor_set_ != VK_NULL_HANDLE) {
                const auto* background = ui_assets_.texture(f2::NativeUiAsset::TitleBackground);
                const float scale = height_ / static_cast<float>(background->height);
                const float image_width = background->width * scale;
                const float offset = std::fmod(title_time * 29.0f, image_width);
                const int image_alpha = alpha((title_time - 5.90f) / 1.10f);
                for (float x = -offset; x < static_cast<float>(width_); x += image_width) {
                    draw_list->AddImage(reinterpret_cast<ImTextureID>(ui_background_descriptor_set_),
                                        ImVec2(x, 0), ImVec2(x + image_width, height_),
                                        ImVec2(0, 0), ImVec2(1, 1),
                                        IM_COL32(255, 255, 255, image_alpha));
                }
            }
            const int logo_alpha = alpha((title_time - 0.86f) / 0.75f);
            if (ui_logo_descriptor_set_ != VK_NULL_HANDLE) {
                const auto* logo = ui_assets_.texture(f2::NativeUiAsset::Logo);
                const float logo_scale = width_ / (logo->width * 1.8f);
                const float logo_width = logo->width * logo_scale;
                const float logo_height = logo->height * logo_scale;
                const float logo_x = (width_ - logo_width) * 0.5f;
                const float logo_y = height_ * 0.47f - logo_height * 0.5f;
                std::array<ImTextureID, 8> sparkle_textures{};
                for (std::size_t index = 0; index < sparkle_textures.size(); ++index) {
                    if (ui_sparkle_descriptor_sets_[index] != VK_NULL_HANDLE) {
                        sparkle_textures[index] = reinterpret_cast<ImTextureID>(
                            ui_sparkle_descriptor_sets_[index]);
                    }
                }
                if (ui_ambient_baseline_descriptor_set_ != VK_NULL_HANDLE && title_time >= 5.90f) {
                    draw_list->AddImage(
                        reinterpret_cast<ImTextureID>(ui_ambient_baseline_descriptor_set_),
                        ImVec2(0, 0), ImVec2(static_cast<float>(width_), static_cast<float>(height_)),
                        ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, 255));
                }
                const bool atlas_visuals = ui_ambient_atlas_descriptor_set_ != VK_NULL_HANDLE &&
                                           title_time >= 5.90f + 2.0f / 60.0f;
                if (atlas_visuals) {
                    logo_sparkles_.draw_captured_atlas_slices(
                        draw_list, reinterpret_cast<ImTextureID>(ui_ambient_atlas_descriptor_set_),
                        static_cast<float>(width_), static_cast<float>(height_), title_time, 1.0f);
                } else if (title_time < 5.90f ||
                           ui_ambient_baseline_descriptor_set_ == VK_NULL_HANDLE) {
                    logo_sparkles_.draw_ambient(
                        draw_list, reinterpret_cast<ImTextureID>(ui_logo_descriptor_set_), logo_x,
                        logo_y, logo_width, logo_height, title_time, 1.0f);
                }
                if (!atlas_visuals) {
                    logo_sparkles_.draw(draw_list, *logo, sparkle_textures,
                                        logo_x, logo_y, logo_width, logo_height, title_time,
                                        logo_alpha / 255.0f);
                }
                draw_list->AddImage(reinterpret_cast<ImTextureID>(ui_logo_descriptor_set_),
                                    ImVec2(logo_x, logo_y),
                                    ImVec2(logo_x + logo_width, logo_y + logo_height),
                                    ImVec2(0, 0), ImVec2(1, 1),
                                    IM_COL32(255, 255, 255, logo_alpha));
            } else {
                draw_list->AddText(ImGui::GetFont(), 92.0f,
                                   ImVec2(width_ * 0.24f, height_ * 0.32f),
                                   IM_COL32(255, 255, 255, logo_alpha), "FABLE II");
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
            const bool use_accept_glyph = input_.using_controller_prompts() &&
                                          ui_accept_descriptor_set_ != VK_NULL_HANDLE &&
                                          prompt == "A";
            if (use_accept_glyph) {
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
                draw_list->AddImage(reinterpret_cast<ImTextureID>(ui_accept_descriptor_set_),
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
            ImGui::Begin(state == f2::FrontendState::MainMenu ? "##main_menu" : "##options", nullptr,
                         ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoSavedSettings);
            auto* draw_list = ImGui::GetWindowDrawList();
            const auto alpha = [](float value) {
                return static_cast<int>(std::clamp(value, 0.0f, 1.0f) * 255.0f);
            };
            const auto background_descriptor = ui_main_background_descriptor_set_ != VK_NULL_HANDLE
                ? ui_main_background_descriptor_set_ : ui_background_descriptor_set_;
            if (background_descriptor != VK_NULL_HANDLE) {
                const auto* background = ui_assets_.texture(
                    ui_main_background_descriptor_set_ != VK_NULL_HANDLE
                        ? f2::NativeUiAsset::MainBackground : f2::NativeUiAsset::TitleBackground);
                const float scale = height_ / static_cast<float>(background->height);
                const float image_width = background->width * scale;
                const float pan_scale = width_ / 1280.0f;
                const float offset = std::fmod(width_ * 0.78125f +
                                                   static_cast<float>(game_.frontend.state_time()) *
                                                       29.0f * pan_scale,
                                               image_width);
                for (float x = -offset; x < static_cast<float>(width_); x += image_width) {
                    draw_list->AddImage(reinterpret_cast<ImTextureID>(background_descriptor),
                                        ImVec2(x, 0), ImVec2(x + image_width, height_));
                }
            } else {
                draw_list->AddRectFilled(ImVec2(0, 0), ImVec2(width_, height_), IM_COL32(8, 12, 22, 255));
                draw_list->AddRectFilled(ImVec2(0, 0), ImVec2(width_ * 0.46f, height_),
                                         IM_COL32(14, 24, 39, 255));
            }
            if (state == f2::FrontendState::MainMenu || state == f2::FrontendState::ChooseCard) {
                const auto* menu_frame = ui_assets_.texture(f2::NativeUiAsset::MenuFrameOverlay);
                const auto* left_upper = ui_assets_.texture(f2::NativeUiAsset::MenuFrameLeftUpper);
                const auto* left_lower = ui_assets_.texture(f2::NativeUiAsset::MenuFrameLeftLower);
                const auto* right_upper = ui_assets_.texture(f2::NativeUiAsset::MenuFrameRightUpper);
                const auto* right_lower = ui_assets_.texture(f2::NativeUiAsset::MenuFrameRightLower);
                const bool has_exact_menu_frame = left_upper && left_lower && right_upper &&
                                                   right_lower &&
                                                   ui_menu_frame_left_upper_descriptor_set_ != VK_NULL_HANDLE &&
                                                   ui_menu_frame_left_lower_descriptor_set_ != VK_NULL_HANDLE &&
                                                   ui_menu_frame_right_upper_descriptor_set_ != VK_NULL_HANDLE &&
                                                   ui_menu_frame_right_lower_descriptor_set_ != VK_NULL_HANDLE;
                const bool has_menu_frame = has_exact_menu_frame ||
                                             (menu_frame != nullptr &&
                                              ui_menu_frame_descriptor_set_ != VK_NULL_HANDLE);
                const auto* frame_elements = ui_assets_.texture(f2::NativeUiAsset::FrameElements);
                const bool has_frame_elements = frame_elements != nullptr &&
                                                 ui_frame_elements_descriptor_set_ != VK_NULL_HANDLE;
                const float menu_unit_scale = width_ / 1280.0f;
                // Projected from the retail sprite shader at 1280x720:
                // outer rows settle at x=177..627 and begin at y=160.
                const float row_x = 177.0f * menu_unit_scale;
                const float row_y = 160.0f * menu_unit_scale;
                const float row_width = 450.0f * menu_unit_scale;
                const float row_height = 68.0f * menu_unit_scale;
                const float slice_width = 52.0f * menu_unit_scale;
                const auto add_three_slice = [&](VkDescriptorSet descriptor,
                                                 const ImVec2& min,
                                                 const ImVec2& max,
                                                 float v0, float v1,
                                                 ImU32 color) {
                    if (descriptor == VK_NULL_HANDLE) return;
                    const auto texture = reinterpret_cast<ImTextureID>(descriptor);
                    const float center_min = min.x + slice_width;
                    const float center_max = max.x - slice_width;
                    draw_list->AddImage(texture, min,
                                        ImVec2(center_min, max.y),
                                        ImVec2(0.0f, v0),
                                        ImVec2(0.125f, v1), color);
                    draw_list->AddImage(texture,
                                        ImVec2(center_min, min.y),
                                        ImVec2(center_max, max.y),
                                        ImVec2(0.125f, v0),
                                        ImVec2(0.813f, v1), color);
                    draw_list->AddImage(texture,
                                        ImVec2(center_max, min.y), max,
                                         ImVec2(0.820f, v0),
                                         ImVec2(0.945f, v1), color);
                };
                const auto add_native_text = [&](std::string_view text, float x, float y,
                                                 float size, ImU32 color) {
                    auto* font = ImGui::GetFont();
                    if (!font || !font->ContainerAtlas || font->ContainerAtlas->TexID == 0 ||
                        font->FontSize <= 0.0f) {
                        return;
                    }
                    const auto texture = static_cast<ImTextureID>(font->ContainerAtlas->TexID);
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
                            draw_list->AddImage(texture,
                                                ImVec2(cursor + glyph->X0 * scale,
                                                       y + glyph->Y0 * scale),
                                                ImVec2(cursor + glyph->X1 * scale,
                                                       y + glyph->Y1 * scale),
                                                ImVec2(glyph->U0, glyph->V0),
                                                ImVec2(glyph->U1, glyph->V1), color);
                        }
                        cursor += glyph->AdvanceX * scale;
                    }
                };
                const bool native_menu_text = ImGui::GetIO().Fonts->TexID != 0;
                const auto* ability_elements = ui_assets_.texture(f2::NativeUiAsset::AbilityElements);
                const bool has_ability_elements = ability_elements != nullptr &&
                                                   ui_ability_elements_descriptor_set_ != VK_NULL_HANDLE;
                const std::size_t selected_index = game_.frontend.selected_item();
                const std::size_t previous_selected_index = game_.frontend.previous_selected_item();
                const float selection_t = std::clamp(
                    static_cast<float>(game_.frontend.selection_time() / 0.15), 0.0f, 1.0f);
                const bool selection_animating = game_.frontend.selection_animating();
                bool selected_prompt_ready = false;
                ImVec2 selected_prompt_min;
                ImVec2 selected_prompt_max;
                ImU32 selected_prompt_color = 0;
                for (std::size_t index = 0; index < game_.frontend.menu_items().size(); ++index) {
                    const RetailMenuSlot* target_slot = retail_menu_slot(index, selected_index);
                    const RetailMenuSlot* source_slot = retail_menu_slot(index, previous_selected_index);
                    if (!target_slot || (selection_animating && !source_slot)) continue;
                    const auto& item = game_.frontend.menu_items()[index];
                    const bool selected = index == selected_index;
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
                    if (has_ability_elements) {
                        const ImVec2 inner_min(row_min.x - 1.5f * menu_unit_scale,
                                               row_min.y + 2.0f * menu_unit_scale);
                        const ImVec2 inner_max(row_max.x + 1.5f * menu_unit_scale,
                                               row_max.y - 1.5f * menu_unit_scale);
                        add_three_slice(ui_ability_elements_descriptor_set_,
                                        inner_min, inner_max, 0.0f,
                                        84.0f / 512.0f, row_color);
                    } else if (ui_menu_surface_descriptor_set_ == VK_NULL_HANDLE) {
                        draw_list->AddRectFilled(row_min, row_max,
                                                 selected ? IM_COL32(60, 43, 27,
                                                                     alpha(slot_alpha))
                                                           : IM_COL32(19, 16, 15,
                                                                      alpha(slot_alpha)),
                                                 24.0f);
                    }
                    if (!has_ability_elements) {
                        draw_list->AddRectFilled(row_min, row_max,
                                                 selected ? IM_COL32(60, 43, 27,
                                                                     alpha(slot_alpha * 0.35f))
                                                           : IM_COL32(19, 16, 15,
                                                                      alpha(slot_alpha * 0.25f)),
                                                 24.0f);
                        draw_list->AddRect(row_min, row_max,
                                           selected ? IM_COL32(223, 166, 91,
                                                               alpha(slot_alpha))
                                                    : IM_COL32(139, 91, 48,
                                                               alpha(slot_alpha)),
                                           24.0f, 0, selected ? 3.0f : 2.0f);
                    }
                    if (has_frame_elements) {
                        add_three_slice(ui_frame_elements_descriptor_set_,
                                        row_min, row_max, 4.0f / 512.0f,
                                        84.0f / 512.0f, row_color);
                    }
                    // The serialized side panel occludes the first ~30px of
                    // the row; keep the text inside the surviving client area.
                    add_native_text(item.label,
                                    row_min.x + 92.0f * slot_scale,
                                    row_min.y + 18.0f * slot_scale,
                                    26.0f * menu_unit_scale * slot_scale,
                                    IM_COL32(255, 224, 128, alpha(slot_alpha)));
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
                                           26.0f * menu_unit_scale * slot_scale,
                                           ImVec2(row_min.x + 92.0f * slot_scale,
                                                  row_min.y + 18.0f * slot_scale),
                                           IM_COL32(255, 224, 128, alpha(slot_alpha)),
                                           item.label.c_str());
                    }
                    if (selected && input_.using_controller_prompts() && has_frame_elements) {
                        // Retail's selected A is a paired frames-elements pass,
                        // not a frames_04 disk behind a separately inset glyph.
                        selected_prompt_ready = true;
                        selected_prompt_min = ImVec2(row_min.x + 20.0f * menu_unit_scale,
                                                     row_min.y + 15.0f * menu_unit_scale);
                        selected_prompt_max = ImVec2(selected_prompt_min.x + 46.0f * menu_unit_scale,
                                                     selected_prompt_min.y + 46.0f * menu_unit_scale);
                        selected_prompt_color = row_color;
                    } else if (selected) {
                        draw_list->AddText(ImGui::GetFont(), 16.0f,
                                           ImVec2(row_min.x - 70.0f, row_min.y + 18.0f),
                                           IM_COL32(240, 220, 160, 255),
                                           input_.prompt(f2::NativeInputAction::Accept).c_str());
                    }
                }
                // Retail sequence order is rows, then the side panels, then
                // the selected prompt. The panel's curved inner edge therefore
                // occludes the row ends instead of sitting underneath them.
                if (has_exact_menu_frame) {
                    draw_list->AddImage(reinterpret_cast<ImTextureID>(ui_menu_frame_left_upper_descriptor_set_),
                                        ImVec2(-3.05f * menu_unit_scale, -3.95f * menu_unit_scale),
                                        ImVec2(271.93f * menu_unit_scale, 513.67f * menu_unit_scale),
                                        ImVec2(0, 0), ImVec2(0.531f, 1.0f),
                                        IM_COL32(255, 255, 255, 255));
                    draw_list->AddImage(reinterpret_cast<ImTextureID>(ui_menu_frame_left_lower_descriptor_set_),
                                        ImVec2(-3.05f * menu_unit_scale, 513.67f * menu_unit_scale),
                                        ImVec2(271.93f * menu_unit_scale, 723.95f * menu_unit_scale),
                                        ImVec2(0, 0), ImVec2(0.531f, 0.406f),
                                        IM_COL32(255, 255, 255, 255));
                    draw_list->AddImage(reinterpret_cast<ImTextureID>(ui_menu_frame_right_upper_descriptor_set_),
                                        ImVec2(1007.06f * menu_unit_scale, -4.95f * menu_unit_scale),
                                        ImVec2(1283.56f * menu_unit_scale, 515.53f * menu_unit_scale),
                                        ImVec2(0, 0), ImVec2(0.531f, 1.0f),
                                        IM_COL32(255, 255, 255, 255));
                    draw_list->AddImage(reinterpret_cast<ImTextureID>(ui_menu_frame_right_lower_descriptor_set_),
                                        ImVec2(1007.06f * menu_unit_scale, 515.53f * menu_unit_scale),
                                        ImVec2(1283.56f * menu_unit_scale, 726.98f * menu_unit_scale),
                                        ImVec2(0, 0), ImVec2(0.531f, 0.406f),
                                        IM_COL32(255, 255, 255, 255));
                } else if (has_menu_frame) {
                    draw_list->AddImage(reinterpret_cast<ImTextureID>(ui_menu_frame_descriptor_set_),
                                        ImVec2(0, 0), ImVec2(width_, height_),
                                        ImVec2(0, 0), ImVec2(1, 1),
                                        IM_COL32(255, 255, 255, 255));
                }
                if (!has_menu_frame && ui_menu_surface_descriptor_set_ != VK_NULL_HANDLE) {
                    draw_list->AddImage(reinterpret_cast<ImTextureID>(ui_menu_surface_descriptor_set_),
                                        ImVec2(0, 0), ImVec2(width_ * 0.16f, height_),
                                        ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, 245));
                    draw_list->AddImage(reinterpret_cast<ImTextureID>(ui_menu_surface_descriptor_set_),
                                        ImVec2(width_ * 0.84f, 0), ImVec2(width_, height_),
                                        ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, 245));
                }
                if (!has_menu_frame && ui_side_rail_descriptor_set_ != VK_NULL_HANDLE) {
                    const auto side_rail = reinterpret_cast<ImTextureID>(
                        ui_side_rail_descriptor_set_);
                    draw_list->AddImage(side_rail,
                                        ImVec2(width_ * 0.145f, 0),
                                        ImVec2(width_ * 0.172f, height_),
                                        ImVec2(0, 0), ImVec2(74.0f / 256.0f, 720.0f / 1024.0f),
                                        IM_COL32(205, 145, 88, 245));
                    draw_list->AddImage(side_rail,
                                        ImVec2(width_ * 0.828f, 0),
                                        ImVec2(width_ * 0.855f, height_),
                                        ImVec2(74.0f / 256.0f, 0),
                                        ImVec2(150.0f / 256.0f, 720.0f / 1024.0f),
                                        IM_COL32(205, 145, 88, 245));
                } else if (!has_menu_frame && has_frame_elements) {
                    const auto frame_texture = reinterpret_cast<ImTextureID>(
                        ui_frame_elements_descriptor_set_);
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
                    // MenuHighlight.rim_and_red is a frames-elements crop that
                    // contains the metallic bezel and a red center. Retail
                    // covers that center with the following green_top /
                    // green_top_translucent passes (151305/151306), producing
                    // the selected A. Its transparent margins are part of the
                    // serialized component and position the bezel around the
                    // glyph.
                    const auto frame_texture = reinterpret_cast<ImTextureID>(
                        ui_frame_elements_descriptor_set_);
                    const float source_prompt_width = 0.448f;
                    const float source_rim_width = 1.024f;
                    const float source_prompt_left_in_rim = 0.328f;
                    // Draw 151304 bounds are z=.544..2.656 (height 2.112),
                    // while draws 151305/151306 start at z=1.388. Preserve
                    // that recorded bezel height and place its top relative
                    // to the green prompt's top; shrinking the crop to the UV
                    // height makes the ring too small even when centered.
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
                        ImVec2(0.0f, 0.171f), ImVec2(0.25f, 0.687f),
                        selected_prompt_color);
                    draw_list->AddImage(frame_texture, selected_prompt_min, selected_prompt_max,
                                        ImVec2(8.0f / 512.0f, 369.0f / 512.0f),
                                        ImVec2(64.0f / 512.0f, 425.0f / 512.0f),
                                        selected_prompt_color);
                    draw_list->AddImage(frame_texture,
                                        ImVec2(selected_prompt_min.x,
                                               selected_prompt_min.y - 0.4f * menu_unit_scale),
                                        ImVec2(selected_prompt_max.x,
                                               selected_prompt_max.y - 0.4f * menu_unit_scale),
                                        ImVec2(8.0f / 512.0f, 369.0f / 512.0f),
                                        ImVec2(64.0f / 512.0f, 425.0f / 512.0f),
                                        selected_prompt_color);
                }
                if (state == f2::FrontendState::ChooseCard) {
                    draw_list->AddRectFilled(ImVec2(0, 0), ImVec2(width_, height_),
                                             IM_COL32(0, 0, 0, 76));
                    const float fade = std::clamp(
                        (static_cast<float>(game_.frontend.state_time()) - 0.04f) / 0.34f,
                        0.0f, 1.0f);
                    const ImU32 card_color = IM_COL32(255, 255, 255,
                                                      static_cast<int>(fade * 255.0f));
                    const float card_scale = width_ / 1280.0f;
                    const auto add_card = [&](VkDescriptorSet descriptor, float x0,
                                              float width, float y_offset, float angle) {
                        if (descriptor == VK_NULL_HANDLE) return;
                        const float scale = width_ / 1280.0f;
                        const float card_height = 398.0f * scale;
                        const float left = x0 * scale;
                        const float top = height_ * 0.502f + y_offset * scale - card_height * 0.5f;
                        const float right = left + width * scale;
                        const float bottom = top + card_height;
                        const float cx = (left + right) * 0.5f;
                        const float cy = (top + bottom) * 0.5f;
                        const float c = std::cos(angle);
                        const float s = std::sin(angle);
                        const auto rotate = [&](float x, float y) {
                            const float dx = x - cx;
                            const float dy = y - cy;
                            return ImVec2(cx + dx * c - dy * s, cy + dx * s + dy * c);
                        };
                        const auto p0 = rotate(left, top);
                        const auto p1 = rotate(right, top);
                        const auto p2 = rotate(right, bottom);
                        const auto p3 = rotate(left, bottom);
                        draw_list->AddImageQuad(reinterpret_cast<ImTextureID>(descriptor),
                                                p0, p1, p2, p3, ImVec2(0, 0), ImVec2(1, 0),
                                                ImVec2(1, 1), ImVec2(0, 1), card_color);
                    };
                    add_card(ui_card_boy_descriptor_set_, 349.0f, 410.0f, -12.0f, -0.14f);
                    add_card(ui_card_girl_descriptor_set_, 654.0f, 250.0f, -3.0f, 0.105f);

                    // Match the measured card draw rectangles for pointer
                    // selection. The controller owns the choice so this path
                    // has the same state semantics as D3D12 and the pad.
                    const auto add_card_hitbox = [&](const char* id, bool girl, float x0,
                                                     float draw_width, float y_offset) {
                        const float card_height = 398.0f * card_scale;
                        const float left = x0 * card_scale;
                        const float top = height_ * 0.502f + y_offset * card_scale -
                                          card_height * 0.5f;
                        ImGui::SetCursorScreenPos(ImVec2(left, top));
                        if (ImGui::InvisibleButton(
                                id, ImVec2(draw_width * card_scale, card_height))) {
                            game_.frontend.select_card(girl);
                        }
                    };
                    add_card_hitbox("##choose_card_boy", false, 349.0f, 410.0f, -12.0f);
                    add_card_hitbox("##choose_card_girl", true, 654.0f, 250.0f, -3.0f);
                }
            } else {
                const auto& options = game_.frontend.options_items();
                const float unit = width_ / 1280.0f;
                const float panel_x = width_ * 0.19f;
                const float panel_y = height_ * 0.10f;
                const float panel_w = width_ * 0.62f;
                const float panel_h = height_ * 0.76f;
                draw_list->AddRectFilled(ImVec2(panel_x, panel_y),
                                         ImVec2(panel_x + panel_w, panel_y + panel_h),
                                         IM_COL32(8, 10, 15, 220), 12.0f * unit);
                draw_list->AddRect(ImVec2(panel_x, panel_y),
                                   ImVec2(panel_x + panel_w, panel_y + panel_h),
                                   IM_COL32(184, 128, 70, 220), 12.0f * unit, 0,
                                   2.0f * unit);
                draw_list->AddText(ImGui::GetFont(), 27.0f * unit,
                                   ImVec2(panel_x + 34.0f * unit, panel_y + 24.0f * unit),
                                   IM_COL32(255, 226, 143, 255), "OPTIONS");
                const float row_x = panel_x + 30.0f * unit;
                const float row_y = panel_y + 82.0f * unit;
                const float row_w = panel_w * 0.43f;
                const float row_h = 54.0f * unit;
                for (std::size_t index = 0; index < options.size(); ++index) {
                    const bool selected = index == game_.frontend.selected_item();
                    const ImVec2 min(row_x, row_y + index * (row_h + 8.0f * unit));
                    const ImVec2 max(min.x + row_w, min.y + row_h);
                    draw_list->AddRectFilled(min, max,
                                             selected ? IM_COL32(83, 58, 35, 245)
                                                      : IM_COL32(31, 27, 24, 220),
                                             8.0f * unit);
                    draw_list->AddRect(min, max,
                                       selected ? IM_COL32(238, 183, 96, 255)
                                                : IM_COL32(126, 86, 47, 220),
                                       8.0f * unit, 0, selected ? 2.0f : 1.0f);
                    draw_list->AddText(ImGui::GetFont(), 20.0f * unit,
                                       ImVec2(min.x + 18.0f * unit, min.y + 14.0f * unit),
                                       selected ? IM_COL32(255, 226, 143, 255)
                                                : IM_COL32(226, 195, 125, 255),
                                       options[index].label.c_str());
                    ImGui::SetCursorScreenPos(min);
                    if (ImGui::InvisibleButton(("##option_" + options[index].id).c_str(),
                                               ImVec2(row_w, row_h))) {
                        game_.frontend.select_menu_item(options[index].id);
                        game_.frontend.dispatch(f2::FrontendAction::Accept);
                    }
                    if (ImGui::IsItemHovered() && !input_.using_controller_prompts()) {
                        game_.frontend.select_menu_item(options[index].id);
                    }
                }
                const auto& selected_id = options.empty() ||
                                                   game_.frontend.selected_item() >= options.size()
                                               ? std::string_view{}
                                               : std::string_view(options[game_.frontend.selected_item()].id);
                const float detail_x = panel_x + panel_w * 0.52f;
                const float detail_y = panel_y + 94.0f * unit;
                const auto draw_detail = [&](const char* title, const char* description,
                                             const char* value) {
                    draw_list->AddText(ImGui::GetFont(), 23.0f * unit,
                                       ImVec2(detail_x, detail_y),
                                       IM_COL32(255, 226, 143, 255), title);
                    draw_list->AddText(ImGui::GetFont(), 16.0f * unit,
                                       ImVec2(detail_x, detail_y + 45.0f * unit),
                                       IM_COL32(219, 203, 170, 255), description);
                    draw_list->AddText(ImGui::GetFont(), 18.0f * unit,
                                       ImVec2(detail_x, detail_y + 94.0f * unit),
                                       IM_COL32(244, 190, 103, 255), value);
                };
                if (selected_id == "sound") {
                    draw_detail("SOUND", "Menu music and interface sounds",
                                game_.frontend.sound_enabled() ? "Status: ON" : "Status: OFF");
                } else if (selected_id == "video") {
                    draw_detail("VIDEO", "Presentation settings for this native build",
                                "Windowed / VSync");
                } else if (selected_id == "key_bindings") {
                    draw_detail("KEY BINDINGS", "Current input bindings",
                                ("Accept " + input_.prompt(f2::NativeInputAction::Accept) +
                                 "   Back " + input_.prompt(f2::NativeInputAction::Back)).c_str());
                    draw_list->AddText(ImGui::GetFont(), 16.0f * unit,
                                       ImVec2(detail_x, detail_y + 126.0f * unit),
                                       IM_COL32(219, 203, 170, 255),
                                       ("Move " + input_.prompt(f2::NativeInputAction::Up) +
                                        " / " + input_.prompt(f2::NativeInputAction::Down)).c_str());
                } else if (selected_id == "renderer") {
                    draw_detail("RENDERER", "Active native presentation backend",
                                "Vulkan native renderer");
                }
                draw_list->AddText(ImGui::GetFont(), 16.0f * unit,
                                   ImVec2(panel_x + 34.0f * unit, panel_y + panel_h - 38.0f * unit),
                                   IM_COL32(232, 215, 178, 255),
                                   (input_.prompt(f2::NativeInputAction::Back) + "  BACK").c_str());
            }
            ImGui::End();
        } else if (state == f2::FrontendState::Loading) {
            ImGui::SetNextWindowPos(ImVec2(width_ * 0.5f, height_ * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
            ImGui::Begin("##loading", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDecoration);
            ImGui::TextUnformatted("Loading native world...");
            ImGui::End();
        } else if (state == f2::FrontendState::World) {
            ImGui::SetNextWindowPos(ImVec2(16, 16));
            ImGui::Begin("Native world", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
            ImGui::TextUnformatted("Native Vulkan world renderer active.");
            ImGui::Text("Meshes: %zu  Instances: %zu", game_.scene.meshes.size(), game_.scene.instances.size());
            if (source_) ImGui::Text("Source: %s", source_->root.string().c_str());
            if (!game_.scene.materials.empty() && !game_.scene.materials[0].albedo.empty()) {
                ImGui::Text("Albedo: %s", game_.scene.materials[0].albedo.c_str());
            }
            ImGui::TextUnformatted("Geometry comes from a user-owned F2SCENE package when supplied.");
            ImGui::End();
        }
        ImGui::Render();
    }

    void draw() {
        if (game_.frontend.state() == f2::FrontendState::IntroVideo ||
            game_.frontend.state() == f2::FrontendState::AttractVideo) ensure_video_texture();
        if (game_.frontend.state() == f2::FrontendState::Title ||
            game_.frontend.state() == f2::FrontendState::MainMenu ||
            game_.frontend.state() == f2::FrontendState::ChooseCard ||
            game_.frontend.state() == f2::FrontendState::Options) ensure_ui_textures();
        update_ambient_detail_frame();
        draw_ui();
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
        clear.color = {{game_.scene.sky_color[0], game_.scene.sky_color[1], game_.scene.sky_color[2], 1.0f}};
        VkRenderPassBeginInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        pass.renderPass = render_pass_;
        pass.framebuffer = framebuffers_[image_index];
        pass.renderArea.extent = extent_;
        pass.clearValueCount = 1;
        pass.pClearValues = &clear;
        vkCmdBeginRenderPass(command_buffers_[image_index], &pass, VK_SUBPASS_CONTENTS_INLINE);
        if (game_.frontend.state() == f2::FrontendState::World) {
            world_renderer_.render(command_buffers_[image_index], extent_.width, extent_.height,
                                   game_.elapsed_seconds);
        }
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), command_buffers_[image_index]);
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
        if (imgui_context_) {
            if (video_descriptor_set_ != VK_NULL_HANDLE) {
                ImGui_ImplVulkan_RemoveTexture(video_descriptor_set_);
                video_descriptor_set_ = VK_NULL_HANDLE;
            }
            if (ui_background_descriptor_set_ != VK_NULL_HANDLE) {
                ImGui_ImplVulkan_RemoveTexture(ui_background_descriptor_set_);
                ui_background_descriptor_set_ = VK_NULL_HANDLE;
            }
            if (ui_main_background_descriptor_set_ != VK_NULL_HANDLE) {
                ImGui_ImplVulkan_RemoveTexture(ui_main_background_descriptor_set_);
                ui_main_background_descriptor_set_ = VK_NULL_HANDLE;
            }
            if (ui_logo_descriptor_set_ != VK_NULL_HANDLE) {
                ImGui_ImplVulkan_RemoveTexture(ui_logo_descriptor_set_);
                ui_logo_descriptor_set_ = VK_NULL_HANDLE;
            }
            if (ui_accept_descriptor_set_ != VK_NULL_HANDLE) {
                ImGui_ImplVulkan_RemoveTexture(ui_accept_descriptor_set_);
                ui_accept_descriptor_set_ = VK_NULL_HANDLE;
            }
            if (ui_back_descriptor_set_ != VK_NULL_HANDLE) {
                ImGui_ImplVulkan_RemoveTexture(ui_back_descriptor_set_);
                ui_back_descriptor_set_ = VK_NULL_HANDLE;
            }
            if (ui_menu_surface_descriptor_set_ != VK_NULL_HANDLE) {
                ImGui_ImplVulkan_RemoveTexture(ui_menu_surface_descriptor_set_);
                ui_menu_surface_descriptor_set_ = VK_NULL_HANDLE;
            }
            for (auto& descriptor_set : ui_sparkle_descriptor_sets_) {
                if (descriptor_set != VK_NULL_HANDLE) {
                    ImGui_ImplVulkan_RemoveTexture(descriptor_set);
                    descriptor_set = VK_NULL_HANDLE;
                }
            }
            if (ui_ambient_atlas_descriptor_set_ != VK_NULL_HANDLE) {
                ImGui_ImplVulkan_RemoveTexture(ui_ambient_atlas_descriptor_set_);
                ui_ambient_atlas_descriptor_set_ = VK_NULL_HANDLE;
            }
            if (ui_ambient_baseline_descriptor_set_ != VK_NULL_HANDLE) {
                ImGui_ImplVulkan_RemoveTexture(ui_ambient_baseline_descriptor_set_);
                ui_ambient_baseline_descriptor_set_ = VK_NULL_HANDLE;
            }
            if (ui_frame_elements_descriptor_set_ != VK_NULL_HANDLE) {
                ImGui_ImplVulkan_RemoveTexture(ui_frame_elements_descriptor_set_);
                ui_frame_elements_descriptor_set_ = VK_NULL_HANDLE;
            }
            if (ui_ability_elements_descriptor_set_ != VK_NULL_HANDLE) {
                ImGui_ImplVulkan_RemoveTexture(ui_ability_elements_descriptor_set_);
                ui_ability_elements_descriptor_set_ = VK_NULL_HANDLE;
            }
            if (ui_menu_frame_descriptor_set_ != VK_NULL_HANDLE) {
                ImGui_ImplVulkan_RemoveTexture(ui_menu_frame_descriptor_set_);
                ui_menu_frame_descriptor_set_ = VK_NULL_HANDLE;
            }
            if (ui_menu_frame_left_upper_descriptor_set_ != VK_NULL_HANDLE) {
                ImGui_ImplVulkan_RemoveTexture(ui_menu_frame_left_upper_descriptor_set_);
                ui_menu_frame_left_upper_descriptor_set_ = VK_NULL_HANDLE;
            }
            if (ui_menu_frame_left_lower_descriptor_set_ != VK_NULL_HANDLE) {
                ImGui_ImplVulkan_RemoveTexture(ui_menu_frame_left_lower_descriptor_set_);
                ui_menu_frame_left_lower_descriptor_set_ = VK_NULL_HANDLE;
            }
            if (ui_menu_frame_right_upper_descriptor_set_ != VK_NULL_HANDLE) {
                ImGui_ImplVulkan_RemoveTexture(ui_menu_frame_right_upper_descriptor_set_);
                ui_menu_frame_right_upper_descriptor_set_ = VK_NULL_HANDLE;
            }
            if (ui_menu_frame_right_lower_descriptor_set_ != VK_NULL_HANDLE) {
                ImGui_ImplVulkan_RemoveTexture(ui_menu_frame_right_lower_descriptor_set_);
                ui_menu_frame_right_lower_descriptor_set_ = VK_NULL_HANDLE;
            }
            if (ui_frames_04_descriptor_set_ != VK_NULL_HANDLE) {
                ImGui_ImplVulkan_RemoveTexture(ui_frames_04_descriptor_set_);
                ui_frames_04_descriptor_set_ = VK_NULL_HANDLE;
            }
            if (ui_card_boy_descriptor_set_ != VK_NULL_HANDLE) {
                ImGui_ImplVulkan_RemoveTexture(ui_card_boy_descriptor_set_);
                ui_card_boy_descriptor_set_ = VK_NULL_HANDLE;
            }
            if (ui_card_girl_descriptor_set_ != VK_NULL_HANDLE) {
                ImGui_ImplVulkan_RemoveTexture(ui_card_girl_descriptor_set_);
                ui_card_girl_descriptor_set_ = VK_NULL_HANDLE;
            }
            if (ui_side_rail_descriptor_set_ != VK_NULL_HANDLE) {
                ImGui_ImplVulkan_RemoveTexture(ui_side_rail_descriptor_set_);
                ui_side_rail_descriptor_set_ = VK_NULL_HANDLE;
            }
            ImGui_ImplVulkan_Shutdown();
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext();
            imgui_context_ = false;
        }
        ui_background_texture_.destroy();
        ui_main_background_texture_.destroy();
        ui_logo_texture_.destroy();
        ui_accept_texture_.destroy();
        ui_back_texture_.destroy();
        ui_menu_surface_texture_.destroy();
        for (auto& texture : ui_sparkle_textures_) texture.destroy();
        ui_ambient_atlas_texture_.destroy();
        ui_ambient_baseline_texture_.destroy();
        ui_frame_elements_texture_.destroy();
        ui_ability_elements_texture_.destroy();
        ui_menu_frame_texture_.destroy();
        ui_menu_frame_left_upper_texture_.destroy();
        ui_menu_frame_left_lower_texture_.destroy();
        ui_menu_frame_right_upper_texture_.destroy();
        ui_menu_frame_right_lower_texture_.destroy();
        ui_frames_04_texture_.destroy();
        ui_card_boy_texture_.destroy();
        ui_card_girl_texture_.destroy();
        ui_side_rail_texture_.destroy();
        video_texture_.destroy();
        video_decoder_.close();
        world_renderer_.destroy();
        if (imgui_descriptor_pool_) vkDestroyDescriptorPool(device_, imgui_descriptor_pool_, nullptr);
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
    bool framebuffer_resized_ = false;
    bool com_initialized_ = false;
    bool imgui_context_ = false;
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
    VkDescriptorSet video_descriptor_set_ = VK_NULL_HANDLE;
    f2::NativeVulkanVideoTexture ui_background_texture_;
    f2::NativeVulkanVideoTexture ui_main_background_texture_;
    f2::NativeVulkanVideoTexture ui_logo_texture_;
    f2::NativeVulkanVideoTexture ui_accept_texture_;
    f2::NativeVulkanVideoTexture ui_back_texture_;
    f2::NativeVulkanVideoTexture ui_menu_surface_texture_;
    std::array<f2::NativeVulkanVideoTexture, 8> ui_sparkle_textures_;
    f2::NativeVulkanVideoTexture ui_ambient_atlas_texture_;
    f2::NativeVulkanVideoTexture ui_ambient_baseline_texture_;
    f2::NativeVulkanVideoTexture ui_frame_elements_texture_;
    f2::NativeVulkanVideoTexture ui_ability_elements_texture_;
    f2::NativeVulkanVideoTexture ui_menu_frame_texture_;
    f2::NativeVulkanVideoTexture ui_menu_frame_left_upper_texture_;
    f2::NativeVulkanVideoTexture ui_menu_frame_left_lower_texture_;
    f2::NativeVulkanVideoTexture ui_menu_frame_right_upper_texture_;
    f2::NativeVulkanVideoTexture ui_menu_frame_right_lower_texture_;
    f2::NativeVulkanVideoTexture ui_frames_04_texture_;
    f2::NativeVulkanVideoTexture ui_card_boy_texture_;
    f2::NativeVulkanVideoTexture ui_card_girl_texture_;
    f2::NativeVulkanVideoTexture ui_side_rail_texture_;
    f2::NativeLogoSparkles logo_sparkles_;
    VkDescriptorSet ui_background_descriptor_set_ = VK_NULL_HANDLE;
    VkDescriptorSet ui_main_background_descriptor_set_ = VK_NULL_HANDLE;
    VkDescriptorSet ui_logo_descriptor_set_ = VK_NULL_HANDLE;
    VkDescriptorSet ui_accept_descriptor_set_ = VK_NULL_HANDLE;
    VkDescriptorSet ui_back_descriptor_set_ = VK_NULL_HANDLE;
    VkDescriptorSet ui_menu_surface_descriptor_set_ = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, 8> ui_sparkle_descriptor_sets_{};
    VkDescriptorSet ui_ambient_atlas_descriptor_set_ = VK_NULL_HANDLE;
    VkDescriptorSet ui_ambient_baseline_descriptor_set_ = VK_NULL_HANDLE;
    VkDescriptorSet ui_frame_elements_descriptor_set_ = VK_NULL_HANDLE;
    VkDescriptorSet ui_ability_elements_descriptor_set_ = VK_NULL_HANDLE;
    VkDescriptorSet ui_menu_frame_descriptor_set_ = VK_NULL_HANDLE;
    VkDescriptorSet ui_menu_frame_left_upper_descriptor_set_ = VK_NULL_HANDLE;
    VkDescriptorSet ui_menu_frame_left_lower_descriptor_set_ = VK_NULL_HANDLE;
    VkDescriptorSet ui_menu_frame_right_upper_descriptor_set_ = VK_NULL_HANDLE;
    VkDescriptorSet ui_menu_frame_right_lower_descriptor_set_ = VK_NULL_HANDLE;
    VkDescriptorSet ui_frames_04_descriptor_set_ = VK_NULL_HANDLE;
    VkDescriptorSet ui_card_boy_descriptor_set_ = VK_NULL_HANDLE;
    VkDescriptorSet ui_card_girl_descriptor_set_ = VK_NULL_HANDLE;
    VkDescriptorSet ui_side_rail_descriptor_set_ = VK_NULL_HANDLE;
    std::size_t ambient_detail_frame_uploaded_ = std::numeric_limits<std::size_t>::max();
    std::uint64_t uploaded_video_serial_ = 0;
    double video_next_frame_time_ = 0.0;
    std::string video_error_;
    f2::NativeVulkanWorldRenderer world_renderer_;
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
    VkDescriptorPool imgui_descriptor_pool_ = VK_NULL_HANDLE;
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
