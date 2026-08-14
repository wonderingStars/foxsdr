# cascade — a modular SDR receiver
#
# SPDX-License-Identifier: MIT
#
# Dependency licences are deliberately permissive so the whole tree can be
# relicensed or shipped closed later without renegotiation:
#   Dear ImGui  MIT     GLFW  Zlib     pffft  BSD-3     SoapySDR  BSL-1.0
# Note the absence of FFTW (GPL) and of GNU Radio VOLK (LGPL). The `volk`
# package present in this vcpkg tree is the *Vulkan* loader, an unrelated
# project that happens to share the name.

cmake_minimum_required(VERSION 3.20)
project(cascade VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------

find_package(glfw3 CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)
find_package(OpenGL REQUIRED)

# The pffft port has shipped under more than one target name; probe rather than
# assume, and fall back to a plain library search so a source build also works.
find_package(pffft CONFIG QUIET)
if(TARGET pffft::pffft)
    set(CASCADE_FFT_TARGET pffft::pffft)
elseif(TARGET PFFFT::PFFFT)
    set(CASCADE_FFT_TARGET PFFFT::PFFFT)
else()
    find_path(PFFFT_INCLUDE_DIR pffft.h REQUIRED)
    find_library(PFFFT_LIBRARY NAMES pffft REQUIRED)
    add_library(cascade_pffft INTERFACE)
    target_include_directories(cascade_pffft SYSTEM INTERFACE "${PFFFT_INCLUDE_DIR}")
    target_link_libraries(cascade_pffft INTERFACE "${PFFFT_LIBRARY}")
    set(CASCADE_FFT_TARGET cascade_pffft)
endif()
message(STATUS "cascade: FFT target = ${CASCADE_FFT_TARGET}")

# ---------------------------------------------------------------------------
# Warnings
# ---------------------------------------------------------------------------

add_library(cascade_flags INTERFACE)
if(MSVC)
    target_compile_options(cascade_flags INTERFACE /W4 /permissive- /Zc:__cplusplus /utf-8 /MP)
    target_compile_definitions(cascade_flags INTERFACE
        NOMINMAX WIN32_LEAN_AND_MEAN _USE_MATH_DEFINES _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(cascade_flags INTERFACE -Wall -Wextra)
endif()

# ---------------------------------------------------------------------------
# Core library — everything except main() and the tests
# ---------------------------------------------------------------------------

file(GLOB CASCADE_CORE   CONFIGURE_DEPENDS src/core/*.cpp)
file(GLOB CASCADE_DSP    CONFIGURE_DEPENDS src/dsp/*.cpp)
file(GLOB CASCADE_GUI    CONFIGURE_DEPENDS src/gui/*.cpp)
file(GLOB CASCADE_SOURCE CONFIGURE_DEPENDS src/source/*.cpp)
file(GLOB CASCADE_SINK   CONFIGURE_DEPENDS src/sink/*.cpp)

add_library(cascade_lib STATIC
    ${CASCADE_CORE} ${CASCADE_DSP} ${CASCADE_GUI} ${CASCADE_SOURCE} ${CASCADE_SINK})

target_include_directories(cascade_lib PUBLIC src)
target_link_libraries(cascade_lib PUBLIC
    cascade_flags
    glfw
    imgui::imgui
    OpenGL::GL
    ${CASCADE_FFT_TARGET})

# ---------------------------------------------------------------------------
# Application
# ---------------------------------------------------------------------------

add_executable(cascade src/main.cpp)
target_link_libraries(cascade PRIVATE cascade_lib)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

enable_testing()
add_subdirectory(tests)
