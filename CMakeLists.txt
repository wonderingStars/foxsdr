# cascade — a modular SDR receiver
#
# SPDX-License-Identifier: MIT
#
# Dependency licences are deliberately permissive so the whole tree can be
# relicensed or shipped closed later without renegotiation:
#   Dear ImGui  MIT     GLFW  Zlib     PortAudio  MIT-like
#   nlohmann/json  MIT  pffft  FFTPACK/BSD-style  SoapySDR  BSL-1.0
# Note the absence of FFTW (GPL) and of GNU Radio VOLK (LGPL). The `volk`
# package present in this vcpkg tree is the *Vulkan* loader, an unrelated
# project that happens to share the name.
#
# All linked libraries except SoapySDR are VENDORED under third_party/ at
# pinned revisions (see third_party/THIRD_PARTY.md for exact tags/commits,
# archive hashes, and per-library notes) and built by this file — upstream or
# vcpkg changes can no longer alter this build. SoapySDR alone stays external
# (vcpkg, via the toolchain file): it is the hardware ABI boundary, and the
# runtime-loaded vendor modules (SoapyUHD etc.) must match the *system*
# SoapySDR ABI — vendoring it would break exactly what it exists to provide.

cmake_minimum_required(VERSION 3.20)
project(cascade VERSION 0.30.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ---------------------------------------------------------------------------
# External dependencies (deliberately minimal — see header comment)
# ---------------------------------------------------------------------------

find_package(OpenGL REQUIRED)
# Hardware sources (P4+). Target verified from SoapySDRExport.cmake: plain
# `SoapySDR`, shared. Vendor support (B200 via SoapyUHD, RTL dongles, etc.)
# arrives through SoapySDR's runtime-loaded modules, keeping this tree MIT.
# NOT vendored, by design — the ABI-boundary exception documented above and
# in third_party/THIRD_PARTY.md.
find_package(SoapySDR CONFIG REQUIRED)

# ---------------------------------------------------------------------------
# Vendored dependencies — third_party/, pinned; see THIRD_PARTY.md
# ---------------------------------------------------------------------------

set(CASCADE_TP "${CMAKE_CURRENT_SOURCE_DIR}/third_party")

# GLFW 3.4 (static; docs/tests/examples/install off)
set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL        OFF CACHE BOOL "" FORCE)
add_subdirectory("${CASCADE_TP}/glfw" EXCLUDE_FROM_ALL)

# PortAudio 19.7 (static). Windows host APIs: WASAPI + WMME only — ASIO needs
# the proprietary Steinberg SDK, DirectSound and WDM/KS are deliberately off
# (WDM/KS is also the one host API whose source vcpkg had to patch; disabling
# it lets the pristine upstream tree build unmodified).
set(PA_BUILD_SHARED          OFF CACHE BOOL "" FORCE)
set(PA_BUILD_STATIC          ON  CACHE BOOL "" FORCE)
set(PA_USE_ASIO              OFF CACHE BOOL "" FORCE)
set(PA_USE_DS                OFF CACHE BOOL "" FORCE)
set(PA_USE_WDMKS             OFF CACHE BOOL "" FORCE)
set(PA_USE_WDMKS_DEVICE_INFO OFF CACHE BOOL "" FORCE)
set(PA_USE_WMME              ON  CACHE BOOL "" FORCE)
set(PA_USE_WASAPI            ON  CACHE BOOL "" FORCE)
set(PA_DISABLE_INSTALL       ON  CACHE BOOL "" FORCE)
# PortAudio's cmake_minimum_required(2.8) predates CMake 4's policy floor;
# give the subdirectory a 3.5 floor (build-system accommodation only — no
# vendored source is modified).
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
add_subdirectory("${CASCADE_TP}/portaudio" EXCLUDE_FROM_ALL)
unset(CMAKE_POLICY_VERSION_MINIMUM)

# Dear ImGui 1.92.8: same translation units the vcpkg port compiled (core +
# imgui_stdlib + the GLFW and OpenGL3 backends). Include dirs exported so
# <imgui.h>, <imgui_impl_glfw.h>, <imgui_impl_opengl3.h> resolve as before.
add_library(imgui STATIC
    "${CASCADE_TP}/imgui/imgui.cpp"
    "${CASCADE_TP}/imgui/imgui_demo.cpp"
    "${CASCADE_TP}/imgui/imgui_draw.cpp"
    "${CASCADE_TP}/imgui/imgui_tables.cpp"
    "${CASCADE_TP}/imgui/imgui_widgets.cpp"
    "${CASCADE_TP}/imgui/misc/cpp/imgui_stdlib.cpp"
    "${CASCADE_TP}/imgui/backends/imgui_impl_glfw.cpp"
    "${CASCADE_TP}/imgui/backends/imgui_impl_opengl3.cpp")
target_include_directories(imgui SYSTEM PUBLIC
    "${CASCADE_TP}/imgui"
    "${CASCADE_TP}/imgui/backends"
    "${CASCADE_TP}/imgui/misc/cpp")
target_link_libraries(imgui PUBLIC glfw OpenGL::GL)

# pffft v1.0.0: mirrors the vcpkg port's own CMakeLists — pffft.c only,
# _USE_MATH_DEFINES on MSVC, no /arch flag (pffft auto-selects SSE via _M_X64
# on MSVC x64, where SSE2 is implicit). The include root is third_party/ so
# the existing <pffft/pffft.h> include spelling keeps working.
add_library(cascade_pffft STATIC "${CASCADE_TP}/pffft/pffft.c")
if(MSVC)
    target_compile_definitions(cascade_pffft PRIVATE _USE_MATH_DEFINES)
endif()
target_include_directories(cascade_pffft SYSTEM PUBLIC "${CASCADE_TP}")

# nlohmann/json 3.12.0 single header (config persistence, P5+).
add_library(cascade_json INTERFACE)
target_include_directories(cascade_json SYSTEM INTERFACE
    "${CASCADE_TP}/nlohmann_json/include")

# cpp-httplib — the HTTP/1.1 server behind web server mode. Header-only, so an
# INTERFACE target carrying the include root plus the Winsock import library the
# header's socket code needs. SYSTEM so /W4 does not police 747 KB of upstream.
#
# TLS IS DELIBERATELY NOT ENABLED. CPPHTTPLIB_OPENSSL_SUPPORT would pull in
# OpenSSL, which is neither vendored here nor small, and would put a second TLS
# stack in a product that already reaches the network through WinHTTP (which
# uses the OS trust store). The server therefore speaks plain HTTP, which is
# why the bind policy treats loopback and LAN differently and why remote access
# is documented as "terminate TLS in front of it" rather than "open the port".
add_library(cascade_httplib INTERFACE)
target_include_directories(cascade_httplib SYSTEM INTERFACE
    "${CASCADE_TP}/cpp_httplib/include")
if(WIN32)
    target_link_libraries(cascade_httplib INTERFACE ws2_32)
endif()

# ---------------------------------------------------------------------------
# Warnings — /W4 applies to OUR code only (cascade_flags is linked by
# cascade_lib, never by the vendored targets, which build at their defaults;
# vendored headers are SYSTEM includes, so they stay warning-silent in our
# translation units too).
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
file(GLOB CASCADE_NET    CONFIGURE_DEPENDS src/net/*.cpp)

add_library(cascade_lib STATIC
    ${CASCADE_CORE} ${CASCADE_DSP} ${CASCADE_GUI} ${CASCADE_SOURCE} ${CASCADE_SINK}
    ${CASCADE_NET})

target_include_directories(cascade_lib PUBLIC src)
# Vendored targets are listed BEFORE SoapySDR so third_party/ include dirs
# always precede vcpkg's shared installed-include root (which still contains
# same-named headers for other consumers of that vcpkg tree).
target_link_libraries(cascade_lib PUBLIC
    cascade_flags
    imgui
    glfw
    OpenGL::GL
    portaudio_static
    cascade_json
    cascade_pffft
    cascade_httplib
    SoapySDR)

# ---------------------------------------------------------------------------
# Application
# ---------------------------------------------------------------------------

# Windows executable icon (Explorer, taskbar, Alt-Tab, Add/Remove Programs).
# The .rc must be a SOURCE of the EXECUTABLE target - rc.exe compiles it and
# the linker embeds the RT_GROUP_ICON - and only of the executable: cascade_lib
# and the test binaries neither need it nor should carry it. The runtime window
# icon is a separate mechanism set in AppWindow::run; see
# resources/icon/README.md.
if(WIN32)
    enable_language(RC)
    add_executable(cascade src/main.cpp resources/icon/foxsdr.rc)
else()
    add_executable(cascade src/main.cpp)
endif()
target_link_libraries(cascade PRIVATE cascade_lib)

# Delay-load SoapySDR so a missing or broken hardware runtime cannot stop the
# app from starting. Without this the Windows loader kills the process before
# main() with an unactionable "SoapySDR.dll was not found" modal; with it, the
# app opens normally and SoapySource::runtimeAvailable() reports the problem
# in the Source panel while the generator and IQ-file sources keep working.
# Only the shipped executable needs it — the test binaries always run beside
# a freshly deployed DLL.
if(MSVC)
    target_link_options(cascade PRIVATE "/DELAYLOAD:SoapySDR.dll")
    target_link_libraries(cascade PRIVATE delayimp)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

enable_testing()
add_subdirectory(tests)
