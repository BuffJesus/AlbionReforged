cmake_minimum_required(VERSION 3.25)
project(AlbionNativeServices LANGUAGES C CXX)

# Pin the codec with the active services; no dependency on the retired program.
set(MINIZ_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendor/miniz")
add_library(albion_miniz STATIC "${MINIZ_DIR}/miniz.c")
target_include_directories(albion_miniz PUBLIC "${MINIZ_DIR}")
target_compile_definitions(albion_miniz PRIVATE _CRT_SECURE_NO_WARNINGS)
add_library(f2data STATIC src/archive.cpp src/byte_source.cpp src/pcm_wave.cpp src/resource_index.cpp src/archive_handles.cpp src/archive_read_view.cpp)
target_include_directories(f2data PUBLIC include)
target_compile_features(f2data PUBLIC cxx_std_17)
target_link_libraries(f2data PRIVATE albion_miniz)
add_library(f2compat STATIC src/tu1_read_compare.cpp src/tu1_manifest.cpp src/tu1_name.cpp)
target_link_libraries(f2compat PUBLIC f2data)
if(MSVC)
  target_compile_options(f2data PRIVATE /W4 /WX)
  target_compile_options(f2compat PRIVATE /W4 /WX)
else()
  target_compile_options(f2data PRIVATE -Wall -Wextra -Werror)
  target_compile_options(f2compat PRIVATE -Wall -Wextra -Werror)
endif()

include(CTest)
if(BUILD_TESTING)
  add_executable(tu1_manifest_name_contract tests/tu1_manifest_name_contract.cpp)
  target_link_libraries(tu1_manifest_name_contract PRIVATE f2compat)
  add_test(NAME tu1_manifest_name_contract COMMAND tu1_manifest_name_contract)
  add_executable(tu1_name_probe tests/tu1_name_probe.cpp)
  target_link_libraries(tu1_name_probe PRIVATE f2compat)
  add_executable(tu1_directory_name_contract tests/tu1_directory_name_contract.cpp)
  target_link_libraries(tu1_directory_name_contract PRIVATE f2compat)
  add_test(NAME tu1_directory_name_contract COMMAND tu1_directory_name_contract)
  add_executable(tu1_table_capture_contract tests/tu1_table_capture_contract.cpp)
  target_link_libraries(tu1_table_capture_contract PRIVATE f2compat)
  add_test(NAME tu1_table_capture_contract COMMAND tu1_table_capture_contract)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  add_executable(tu1_manifest_probe tests/tu1_manifest_probe.cpp)
  add_test(NAME tu1_name_contract COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/tu1_name_contract.py"
    --probe "$<TARGET_FILE:tu1_name_probe>")
  target_link_libraries(tu1_manifest_probe PRIVATE f2compat)
  add_test(NAME tu1_manifest_contract COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/tu1_manifest_contract.py"
    --probe "$<TARGET_FILE:tu1_manifest_probe>")
  add_executable(byte_source_contract tests/byte_source_contract.cpp)
  target_link_libraries(byte_source_contract PRIVATE f2data)
  add_test(NAME byte_source_contract COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/byte_source_contract.py"
    --probe "$<TARGET_FILE:byte_source_contract>")
  if(WIN32)
    add_executable(tu1_capture_hook_contract tests/tu1_capture_hook_contract.cpp
      adapters/tu1_table_capture_hook.cpp)
    target_include_directories(tu1_capture_hook_contract PRIVATE tests/fake_rex include)
    target_compile_features(tu1_capture_hook_contract PRIVATE cxx_std_17)
    add_test(NAME tu1_capture_hook_contract COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/tu1_capture_hook_contract.py"
      --probe "$<TARGET_FILE:tu1_capture_hook_contract>")
  endif()
  add_test(NAME tu1_capture_file_contract COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/tu1_capture_file_contract.py")
  add_executable(archive_probe tests/archive_probe.cpp)
  target_link_libraries(archive_probe PRIVATE f2data)
  add_test(NAME archive_contract COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/archive_contract.py"
    --probe "$<TARGET_FILE:archive_probe>"
    --output "${CMAKE_CURRENT_BINARY_DIR}/contract-fixtures")
  add_test(NAME archive_ranges COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/archive_ranges.py"
    --probe "$<TARGET_FILE:archive_probe>"
    --output "${CMAKE_CURRENT_BINARY_DIR}/range-fixtures")
  add_executable(pcm_probe tests/pcm_probe.cpp)
  target_link_libraries(pcm_probe PRIVATE f2data)
  add_test(NAME pcm_contract COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/pcm_contract.py"
    --probe "$<TARGET_FILE:pcm_probe>"
    --output "${CMAKE_CURRENT_BINARY_DIR}/pcm-fixtures")
  add_executable(resource_index_probe tests/resource_index_probe.cpp)
  target_link_libraries(resource_index_probe PRIVATE f2data)
  add_test(NAME resource_index_contract COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/resource_index_contract.py"
    --probe "$<TARGET_FILE:resource_index_probe>"
    --output "${CMAKE_CURRENT_BINARY_DIR}/resource-index-fixtures")
  add_executable(archive_handles_contract tests/archive_handles_contract.cpp)
  target_link_libraries(archive_handles_contract PRIVATE f2data)
  add_test(NAME archive_handles_contract COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/archive_handles_contract.py"
    --probe "$<TARGET_FILE:archive_handles_contract>"
    --output "${CMAKE_CURRENT_BINARY_DIR}/archive-handle-fixtures")
  add_executable(archive_read_view_probe tests/archive_read_view_probe.cpp)
  target_link_libraries(archive_read_view_probe PRIVATE f2data)
  add_test(NAME archive_read_view_contract COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/archive_read_view_contract.py"
    --probe "$<TARGET_FILE:archive_read_view_probe>"
    --output "${CMAKE_CURRENT_BINARY_DIR}/archive-read-view-fixtures")
  add_executable(tu1_read_compare_contract tests/tu1_read_compare_contract.cpp)
  target_link_libraries(tu1_read_compare_contract PRIVATE f2compat)
  add_test(NAME tu1_read_compare_contract COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/tu1_read_compare_contract.py"
    --probe "$<TARGET_FILE:tu1_read_compare_contract>"
    --output "${CMAKE_CURRENT_BINARY_DIR}/tu1-read-compare-fixtures")
endif()
