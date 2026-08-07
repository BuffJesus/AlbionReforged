cmake_minimum_required(VERSION 3.25)
project(Fable2Native LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(F2NATIVE_ASSETBROWSER_SOURCE
    "${CMAKE_CURRENT_SOURCE_DIR}/../Fable2AssetBrowser/source/src"
    CACHE PATH "Local AssetBrowser source used by optional research cookers")

add_library(f2native_core STATIC
    src/native_scene.cpp
    src/native_texture.cpp
    src/native_game.cpp
    src/native_frontend.cpp
    src/native_install.cpp
    src/native_cook.cpp
    src/native_frontend_config.cpp
    src/native_input.cpp
    src/native_audio.cpp
    src/native_ui.cpp
    src/native_font.cpp
    src/native_iso.cpp
    src/native_video.cpp
)

target_include_directories(f2native_core PUBLIC include)
# Vendored public-domain stb_truetype for the native font atlas (ImGui-free UI text).
target_include_directories(f2native_core PRIVATE third_party)

# Dev builds locate the offline cookers (tools/cook_*.py) at the source tree; a shipped
# installer bundles them next to the executable (see default_tools_dir()).
target_compile_definitions(f2native_core PRIVATE
    F2NATIVE_TOOLS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tools")

if(WIN32)
    target_link_libraries(f2native_core PUBLIC xinput9_1_0 windowscodecs)
endif()

option(F2NATIVE_ENABLE_LOCAL_COOKERS
       "Build local research cookers; they never package game assets" ON)
if(F2NATIVE_ENABLE_LOCAL_COOKERS AND EXISTS
   "${F2NATIVE_ASSETBROWSER_SOURCE}/textures/LhTexCodec.cpp")
    add_executable(f2native_cook_lh_tex
        tools/cook_lh_tex.cpp
        "${F2NATIVE_ASSETBROWSER_SOURCE}/textures/LhTexCodec.cpp")
    target_include_directories(f2native_cook_lh_tex PRIVATE
        "${F2NATIVE_ASSETBROWSER_SOURCE}/textures")
    if(MSVC)
        target_compile_options(f2native_cook_lh_tex PRIVATE /W4 /permissive-)
    else()
        target_compile_options(f2native_cook_lh_tex PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endif()

if(WIN32)
    add_library(f2native_video STATIC src/native_video_decoder.cpp)
    target_include_directories(f2native_video PUBLIC include)
    target_link_libraries(f2native_video PUBLIC mfplat mfreadwrite mfuuid)
    add_executable(f2native_video_probe src/video_probe_main.cpp)
    target_link_libraries(f2native_video_probe PRIVATE f2native_video)
endif()

if(MSVC)
    target_compile_options(f2native_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(f2native_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(f2native_probe src/main.cpp)
target_link_libraries(f2native_probe PRIVATE f2native_core)

add_executable(f2native_installer src/installer_main.cpp)
target_link_libraries(f2native_installer PRIVATE f2native_core)

if(WIN32)
    option(F2NATIVE_ENABLE_VULKAN "Build the Vulkan native frontend" ON)
    if(F2NATIVE_ENABLE_VULKAN)
        find_package(Vulkan QUIET)
        find_program(F2NATIVE_GLSLC glslc
            HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin")
        if(NOT Vulkan_FOUND OR NOT F2NATIVE_GLSLC)
            message(WARNING "Vulkan SDK/glslc not found; Vulkan frontend will not be built")
            set(F2NATIVE_ENABLE_VULKAN OFF)
        endif()
    endif()

    set(F2NATIVE_IMGUI_SOURCE "" CACHE PATH "Dear ImGui source directory")
    if(NOT F2NATIVE_IMGUI_SOURCE AND EXISTS
       "${CMAKE_CURRENT_SOURCE_DIR}/../Fable2AssetBrowser/source/build/_deps/imgui-src")
        set(F2NATIVE_IMGUI_SOURCE
            "${CMAKE_CURRENT_SOURCE_DIR}/../Fable2AssetBrowser/source/build/_deps/imgui-src"
            CACHE PATH "Dear ImGui source directory" FORCE)
    endif()
    if(NOT F2NATIVE_IMGUI_SOURCE OR NOT EXISTS "${F2NATIVE_IMGUI_SOURCE}/imgui.cpp")
        include(FetchContent)
        FetchContent_Declare(imgui
            GIT_REPOSITORY https://github.com/ocornut/imgui.git
            GIT_TAG v1.91.5
            GIT_SHALLOW TRUE)
        FetchContent_MakeAvailable(imgui)
        set(F2NATIVE_IMGUI_SOURCE "${imgui_SOURCE_DIR}")
    endif()

    add_library(f2native_imgui STATIC
        "${F2NATIVE_IMGUI_SOURCE}/imgui.cpp"
        "${F2NATIVE_IMGUI_SOURCE}/imgui_draw.cpp"
        "${F2NATIVE_IMGUI_SOURCE}/imgui_tables.cpp"
        "${F2NATIVE_IMGUI_SOURCE}/imgui_widgets.cpp"
        "${F2NATIVE_IMGUI_SOURCE}/backends/imgui_impl_win32.cpp"
        "${F2NATIVE_IMGUI_SOURCE}/backends/imgui_impl_dx12.cpp")
    if(F2NATIVE_ENABLE_VULKAN)
        target_sources(f2native_imgui PRIVATE
            "${F2NATIVE_IMGUI_SOURCE}/backends/imgui_impl_vulkan.cpp")
        target_link_libraries(f2native_imgui PRIVATE Vulkan::Vulkan)
    endif()
    target_include_directories(f2native_imgui PUBLIC
        "${F2NATIVE_IMGUI_SOURCE}"
        "${F2NATIVE_IMGUI_SOURCE}/backends")

    # One executable hosts BOTH backends; the active backend is selected at launch (restart-applied)
    # by frontend_main.cpp via resolve_render_backend(). The Vulkan sources/libs are only compiled in
    # when the Vulkan SDK is available (F2NATIVE_HAS_VULKAN); otherwise the toggle resolves to D3D12.
    add_executable(f2native_frontend WIN32
        src/frontend_main.cpp
        src/native_frontend_app.cpp
        src/native_logo_effects.cpp
        src/native_ui_renderer.cpp
        src/native_world_renderer.cpp)
    target_link_libraries(f2native_frontend PRIVATE
        f2native_core
        f2native_video
        f2native_imgui
        d3d12
        d3dcompiler
        dxgi
        ole32
        shell32
        user32
        xinput9_1_0
        xaudio2)
    target_include_directories(f2native_frontend PRIVATE
        "${F2NATIVE_IMGUI_SOURCE}"
        "${F2NATIVE_IMGUI_SOURCE}/backends")
    target_compile_definitions(f2native_frontend PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX)

    if(F2NATIVE_ENABLE_VULKAN)
        set(F2NATIVE_VULKAN_SHADER_OUTPUT_DIR
            "${CMAKE_CURRENT_BINARY_DIR}/vulkan_shaders")
        set(F2NATIVE_VULKAN_SHADER_OUTPUTS
            "${F2NATIVE_VULKAN_SHADER_OUTPUT_DIR}/native_world.vert.spv"
            "${F2NATIVE_VULKAN_SHADER_OUTPUT_DIR}/native_world.frag.spv")
        add_custom_command(
            OUTPUT "${F2NATIVE_VULKAN_SHADER_OUTPUT_DIR}/native_world.vert.spv"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${F2NATIVE_VULKAN_SHADER_OUTPUT_DIR}"
            COMMAND ${F2NATIVE_GLSLC} -o
                    "${F2NATIVE_VULKAN_SHADER_OUTPUT_DIR}/native_world.vert.spv"
                    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/native_world.vert"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/native_world.vert"
            VERBATIM)
        add_custom_command(
            OUTPUT "${F2NATIVE_VULKAN_SHADER_OUTPUT_DIR}/native_world.frag.spv"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${F2NATIVE_VULKAN_SHADER_OUTPUT_DIR}"
            COMMAND ${F2NATIVE_GLSLC} -o
                    "${F2NATIVE_VULKAN_SHADER_OUTPUT_DIR}/native_world.frag.spv"
                    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/native_world.frag"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/native_world.frag"
            VERBATIM)
        add_custom_target(f2native_vulkan_shaders DEPENDS ${F2NATIVE_VULKAN_SHADER_OUTPUTS})

        file(TO_CMAKE_PATH "${F2NATIVE_VULKAN_SHADER_OUTPUT_DIR}"
             F2NATIVE_VULKAN_SHADER_DIR_DEFINE)
        target_sources(f2native_frontend PRIVATE
            src/native_frontend_vulkan_app.cpp
            src/native_vulkan_video_texture.cpp
            src/native_vulkan_world_renderer.cpp)
        target_link_libraries(f2native_frontend PRIVATE Vulkan::Vulkan)
        target_compile_definitions(f2native_frontend PRIVATE
            F2NATIVE_HAS_VULKAN
            F2NATIVE_VULKAN_SHADER_DIR="${F2NATIVE_VULKAN_SHADER_DIR_DEFINE}")
        add_dependencies(f2native_frontend f2native_vulkan_shaders)
    endif()
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(f2native_core_tests tests/native_core_tests.cpp)
    target_link_libraries(f2native_core_tests PRIVATE f2native_core)
    add_test(NAME f2native_core_tests COMMAND f2native_core_tests)
endif()
