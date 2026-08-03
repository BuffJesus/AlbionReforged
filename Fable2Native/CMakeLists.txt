cmake_minimum_required(VERSION 3.25)
project(Fable2Native LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(f2native_core STATIC
    src/native_scene.cpp
    src/native_game.cpp
    src/native_frontend.cpp
    src/native_install.cpp
    src/native_iso.cpp
    src/native_video.cpp
)

target_include_directories(f2native_core PUBLIC include)

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
    target_include_directories(f2native_imgui PUBLIC
        "${F2NATIVE_IMGUI_SOURCE}"
        "${F2NATIVE_IMGUI_SOURCE}/backends")

    add_executable(f2native_frontend WIN32
        src/native_frontend_app.cpp
        src/native_world_renderer.cpp)
    target_link_libraries(f2native_frontend PRIVATE
        f2native_core
        f2native_imgui
        d3d12
        d3dcompiler
        dxgi
        ole32
        shell32
        user32)
    target_include_directories(f2native_frontend PRIVATE
        "${F2NATIVE_IMGUI_SOURCE}"
        "${F2NATIVE_IMGUI_SOURCE}/backends")
    target_compile_definitions(f2native_frontend PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX)
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(f2native_core_tests tests/native_core_tests.cpp)
    target_link_libraries(f2native_core_tests PRIVATE f2native_core)
    add_test(NAME f2native_core_tests COMMAND f2native_core_tests)
endif()
