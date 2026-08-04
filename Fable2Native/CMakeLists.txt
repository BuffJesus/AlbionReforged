cmake_minimum_required(VERSION 3.25)
project(Fable2Native LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(f2native_core STATIC
    src/native_scene.cpp
    src/native_texture.cpp
    src/native_game.cpp
    src/native_frontend.cpp
    src/native_install.cpp
    src/native_input.cpp
    src/native_ui.cpp
    src/native_iso.cpp
    src/native_video.cpp
)

target_include_directories(f2native_core PUBLIC include)

if(WIN32)
    target_link_libraries(f2native_core PUBLIC xinput9_1_0)
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

    add_executable(f2native_frontend WIN32
        src/native_frontend_app.cpp
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
        user32)
    target_link_libraries(f2native_frontend PRIVATE xinput9_1_0)
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

        add_executable(f2native_frontend_vulkan WIN32
            src/native_frontend_vulkan_app.cpp
            src/native_vulkan_video_texture.cpp
            src/native_vulkan_world_renderer.cpp)
        target_link_libraries(f2native_frontend_vulkan PRIVATE
            f2native_core
            f2native_video
            f2native_imgui
            Vulkan::Vulkan
            ole32
            shell32
            user32)
        target_link_libraries(f2native_frontend_vulkan PRIVATE xinput9_1_0)
        target_include_directories(f2native_frontend_vulkan PRIVATE
            "${F2NATIVE_IMGUI_SOURCE}"
            "${F2NATIVE_IMGUI_SOURCE}/backends")
        file(TO_CMAKE_PATH "${F2NATIVE_VULKAN_SHADER_OUTPUT_DIR}"
             F2NATIVE_VULKAN_SHADER_DIR_DEFINE)
        target_compile_definitions(f2native_frontend_vulkan PRIVATE
            WIN32_LEAN_AND_MEAN
            NOMINMAX
            F2NATIVE_VULKAN_SHADER_DIR="${F2NATIVE_VULKAN_SHADER_DIR_DEFINE}")
        add_dependencies(f2native_frontend_vulkan f2native_vulkan_shaders)
    endif()
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(f2native_core_tests tests/native_core_tests.cpp)
    target_link_libraries(f2native_core_tests PRIVATE f2native_core)
    add_test(NAME f2native_core_tests COMMAND f2native_core_tests)
endif()
