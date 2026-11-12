# FoxSDR plugin SDK template: one decoder plugin that builds against nothing but plugin_abi.h.
#
#   cmake -S sdk -B build-sdk -A x64          (Windows, Visual Studio generator)
#   cmake --build build-sdk --config Release
#   ctest --test-dir build-sdk -C Release --output-on-failure
#
# The plugin and its test need exactly ONE file from FoxSDR: plugin_abi.h (MIT licence, the same file in the plugin
# repository's include/). It is found, in this order, in
#   1. -DFOXSDR_PLUGIN_ABI_DIR=<folder holding plugin_abi.h>
#   2. sdk/include/                      (copy the header there for a project of your own)
#   3. ../src/core/                      (this folder inside the FoxSDR source tree)
# Nothing is downloaded and nothing else of FoxSDR is linked: a plugin never links against the host.
#
# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.16)
project(foxsdr_plugin_sdk VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(FOXSDR_PLUGIN_ABI_DIR "" CACHE PATH "Folder that holds plugin_abi.h (default: sdk/include, then ../src/core)")
set(_abi_candidates "${FOXSDR_PLUGIN_ABI_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/include" "${CMAKE_CURRENT_SOURCE_DIR}/../src/core")
set(_abi_dir "")
foreach(_c IN LISTS _abi_candidates)
    if(NOT _c STREQUAL "" AND EXISTS "${_c}/plugin_abi.h")
        set(_abi_dir "${_c}")
        break()
    endif()
endforeach()
if(_abi_dir STREQUAL "")
    message(FATAL_ERROR "plugin_abi.h not found. Pass -DFOXSDR_PLUGIN_ABI_DIR=<folder with plugin_abi.h>, or copy "
            "plugin_abi.h into sdk/include/ (it is src/core/plugin_abi.h in the FoxSDR source tree).")
endif()
message(STATUS "plugin_abi.h from ${_abi_dir}")

# The plugin's own version, also the version in its file name.
set(PLUGIN_ID "heartbeat-decoder")
set(PLUGIN_VERSION "${PROJECT_VERSION}")

# File name the host's scanner expects: <id>-<version>-abi<ABI>-<os>-<arch>.<dll|so|dylib>
# (docs/PLUGIN-API.md, "When a plugin misbehaves": a name that declares ANOTHER ABI is refused before any of its code runs).
if(WIN32)
    set(_plat "win-x64")
elseif(APPLE)
    set(_plat "macos-${CMAKE_SYSTEM_PROCESSOR}")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    set(_plat "linux-arm64")
else()
    set(_plat "linux-x64")
endif()

# The code both the module and the test use.
add_library(heartbeat_core OBJECT src/heartbeat_decoder.cpp)
target_include_directories(heartbeat_core PRIVATE "${_abi_dir}")
target_compile_definitions(heartbeat_core PRIVATE CASCADE_PLUGIN_BUILD)
if(MSVC)
    target_compile_options(heartbeat_core PRIVATE /W4 /permissive- /EHsc)
else()
    target_compile_options(heartbeat_core PRIVATE -Wall -Wextra -fvisibility=hidden)
    set_target_properties(heartbeat_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

# The plugin: ONE shared module that exports cascade_plugin_query and nothing else.
add_library(heartbeat_decoder MODULE $<TARGET_OBJECTS:heartbeat_core>)
set_target_properties(heartbeat_decoder PROPERTIES
    PREFIX ""
    OUTPUT_NAME "${PLUGIN_ID}-${PLUGIN_VERSION}-abi3-${_plat}")
if(MSVC)
    # The vcruntime is linked statically so the module needs no redistributable beside it.
    set_property(TARGET heartbeat_decoder PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
    set_property(TARGET heartbeat_core PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
endif()

# The test: calls the exported entry point and the decoder table exactly as the host does.
enable_testing()
add_executable(heartbeat_test tests/heartbeat_test.cpp $<TARGET_OBJECTS:heartbeat_core>)
target_include_directories(heartbeat_test PRIVATE "${_abi_dir}")
if(MSVC)
    target_compile_options(heartbeat_test PRIVATE /W4 /permissive- /EHsc)
    set_property(TARGET heartbeat_test PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
else()
    target_compile_options(heartbeat_test PRIVATE -Wall -Wextra)
endif()
add_test(NAME heartbeat_test COMMAND heartbeat_test)
set_tests_properties(heartbeat_test PROPERTIES TIMEOUT 60)

# The plugin writes nothing to stdout, stderr or the host's settings; and the scan itself refuses a file that does.
add_test(NAME heartbeat_sinks
         COMMAND ${CMAKE_COMMAND} -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/src/heartbeat_decoder.cpp -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/scan_sinks.cmake)
add_test(NAME heartbeat_sinks_scan_refuses_a_bad_file
         COMMAND ${CMAKE_COMMAND} -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/sinks_bad.cpp.txt -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/scan_sinks.cmake)
set_tests_properties(heartbeat_sinks_scan_refuses_a_bad_file PROPERTIES WILL_FAIL TRUE)

# `cmake --install build-sdk --config Release --prefix <FoxSDR install dir>\plugins` copies the module where FoxSDR looks.
install(TARGETS heartbeat_decoder LIBRARY DESTINATION . RUNTIME DESTINATION .)
