#define VK_USE_PLATFORM_WIN32_KHR

#include "f2/native_game.h"
#include "f2/native_install.h"
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
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND window, UINT message,
                                               WPARAM wparam, LPARAM lparam);

namespace {

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

        WNDCLASSA window_class{};
        window_class.hInstance = instance;
        window_class.lpfnWndProc = &FrontendApp::window_proc;
        window_class.lpszClassName = "Fable2NativeVulkanFrontend";
        window_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
        if (!RegisterClassA(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            return false;
        }
        window_ = CreateWindowA(window_class.lpszClassName, "Fable II Native - Vulkan",
                                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
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
        application.pApplicationName = "Albion Reforged";
        application.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
        application.pEngineName = "Albion Reforged Native Runtime";
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
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true)) game_.frontend.dispatch(f2::FrontendAction::Up);
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true)) game_.frontend.dispatch(f2::FrontendAction::Down);
        if (ImGui::IsKeyPressed(ImGuiKey_Enter, true)) game_.frontend.dispatch(f2::FrontendAction::Accept);
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, true)) game_.frontend.dispatch(f2::FrontendAction::Back);
        if (ImGui::IsKeyPressed(ImGuiKey_Space, true)) game_.frontend.dispatch(f2::FrontendAction::Skip);
    }

    void draw_ui() {
        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        const auto state = game_.frontend.state();
        if (state == f2::FrontendState::Boot || state == f2::FrontendState::IntroVideo) {
            ImGui::SetNextWindowPos(ImVec2(0, 0));
            ImGui::SetNextWindowSize(ImVec2(static_cast<float>(width_), static_cast<float>(height_)));
            ImGui::Begin("##intro", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoSavedSettings);
            ImGui::SetCursorPos(ImVec2(width_ * 0.5f - 180.0f, height_ * 0.45f));
            if (state == f2::FrontendState::Boot) ImGui::TextUnformatted("FABLE II NATIVE");
            else if (const auto* clip = game_.frontend.intro_videos().current_clip()) {
                ImGui::Text("INTRO VIDEO: %s", clip->id.c_str());
                ImGui::Text("Asset: %s", clip->asset.string().c_str());
            } else ImGui::TextUnformatted("INTRO VIDEO PLAYER");
            ImGui::SetCursorPos(ImVec2(width_ * 0.5f - 120.0f, height_ * 0.55f));
            ImGui::TextUnformatted("Press Space to skip");
            ImGui::End();
        } else if (state == f2::FrontendState::Title) {
            ImGui::SetNextWindowPos(ImVec2(width_ * 0.5f, height_ * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
            ImGui::Begin("##title", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDecoration);
            ImGui::TextUnformatted("FABLE II");
            ImGui::TextUnformatted("Press Enter to start");
            ImGui::End();
        } else if (state == f2::FrontendState::MainMenu || state == f2::FrontendState::Options) {
            ImGui::SetNextWindowPos(ImVec2(width_ * 0.5f, height_ * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
            ImGui::Begin(state == f2::FrontendState::MainMenu ? "Main Menu" : "Options", nullptr,
                         ImGuiWindowFlags_AlwaysAutoResize);
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
            ImGui::TextUnformatted("Geometry comes from a user-owned F2SCENE package when supplied.");
            ImGui::End();
        }
        ImGui::Render();
    }

    void draw() {
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
        if (!instance_) return;
        if (device_) vkDeviceWaitIdle(device_);
        if (imgui_context_) {
            ImGui_ImplVulkan_Shutdown();
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext();
            imgui_context_ = false;
        }
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
        window_ = nullptr;
    }

    HWND window_ = nullptr;
    UINT width_ = 1280;
    UINT height_ = 720;
    bool framebuffer_resized_ = false;
    bool com_initialized_ = false;
    bool imgui_context_ = false;
    std::optional<f2::GameSource> source_;
    f2::NativeVulkanWorldRenderer world_renderer_;
    f2::NativeGame game_;
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

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int) {
    FrontendApp app;
    return app.initialise(instance) ? app.run() : 1;
}
