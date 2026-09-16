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
project(cascade VERSION 0.98.0 LANGUAGES C CXX)

# The version string the BINARY reports, which is not always the project
# version. A nightly is built from this tree with a pre-release suffix naming
# the date and the commit it came from, and the About line, the usage report
# and the bug-report form must all say that rather than the release it is
# heading towards - otherwise a report names a version that does not exist and
# nobody can tell which build produced it.
#
# Set CASCADE_VERSION_STRING to override (tools/build-nightly.ps1 does).
# Left alone it is the project version, which is what a release build wants.
# NOT a cache variable, and that is the whole point. As a CACHE STRING it stuck
# at whatever it was first configured with, so bumping project() above left the
# binary still reporting the OLD version - a 0.57.0 installer packaging a binary
# that called itself 0.56.0, which is precisely what injecting the version was
# meant to make impossible. The app_version test could not see it either: it
# compares the binary against this variable, so both were wrong together.
#
# A -DCASCADE_VERSION_STRING=... on the command line still wins, because that
# creates a cache entry this if() sees; tools/build-nightly.ps1 relies on that.
if(NOT CASCADE_VERSION_STRING)
    set(CASCADE_VERSION_STRING "${PROJECT_VERSION}")
endif()
message(STATUS "cascade version string: ${CASCADE_VERSION_STRING}")

# Appended to CASCADE_VERSION_STRING at compile time (src/core/version.cpp).
# Empty by default, which is a normal desktop build and leaves the string
# unchanged. android/app/build.gradle passes "-android.<buildNumber>" here
# through externalNativeBuild arguments, so every report a sideloaded Android
# alpha sends is distinguishable from a desktop release of the same version -
# the site's version comparator treats "-<anything>" as a pre-release, which
# is what we want. A CACHE STRING (unlike CASCADE_VERSION_STRING above) is
# correct here: it is genuinely a build-time knob set once per configure
# (Gradle reconfigures Android builds itself), not a value this file derives
# from something that can move underneath a stale cache entry.
set(CASCADE_VERSION_SUFFIX "" CACHE STRING
    "Suffix appended to the reported version string (e.g. -android.1)")

# ---------------------------------------------------------------------------
# Android — ONE tree, and no early return
# ---------------------------------------------------------------------------
#
# -DCASCADE_ANDROID=ON is set by android/app/build.gradle's externalNativeBuild,
# which points Gradle at THIS file so the phone build and the desktop build come
# out of one source of truth rather than a second CMakeLists that drifts.
#
# THERE USED TO BE AN EARLY RETURN HERE, and removing it is the point of this
# slice. Gradle passed CASCADE_ANDROID_SHELL_ONLY as well and got a return
# straight into src/platform/android, where a placeholder target compiled just
# enough of src/dsp and two files of src/gui to draw a first screen; the
# portable-core cross-build read on through the whole file and produced a
# library that shell could not use. Both halves now come out of ONE configure:
# the file is read to the end, cascade_lib is built for Android with the
# desktop-only units excluded (see the CASCADE_ANDROID option below and every
# `if(CASCADE_ANDROID)` filter), src/gui is compiled INTO it, and
# src/platform/android at the very bottom links the lot into libfoxsdr.so.
# CASCADE_ANDROID_SHELL_ONLY is gone from this file and from build.gradle.
#
# What is still not built for Android is guarded where it lives rather than
# skipped wholesale: find_package(OpenGL)/(SoapySDR)/(OpenSSL) and libunwind
# are never looked for, GLFW and PortAudio are not built, the `cascade`
# executable, the Inno Setup version file and the symbol-archiving POST_BUILD
# steps do not exist, and the test tree takes a curated subset.

# Hand the version to the Inno Setup script, so the installer cannot be stamped
# with a number the binary does not carry. It used to be a literal in
# cascade.iss and drifted the moment this file was bumped: 0.57.0 source
# produced a "0.56.0" installer. Written at configure time; the .iss includes it
# unless a nightly overrides AppVersion outright.
file(WRITE "${CMAKE_CURRENT_SOURCE_DIR}/installer/generated-version.iss"
     "; Generated by CMake - do not edit, and do not commit a stale copy.
"
     "#define AppVersion \"${PROJECT_VERSION}\"
")

# The commit this binary was built from.
#
# A version alone does not identify a build. "0.61.0" is the release, the
# nightly heading towards it, and every rebuild in between; a crash report that
# names only the version cannot be matched to a source tree. The short SHA
# costs nothing and makes the report reproducible.
#
# GENERATED ON EVERY BUILD, not baked in at configure time. This was an
# execute_process here, and it was silently wrong: CMake re-configures when
# CMakeLists.txt changes, not when HEAD moves, so every build after the next
# commit stamped the PREVIOUS commit into the binary and into every report it
# wrote. See cmake/git-commit.cmake, which is run by the cascade_git_commit
# target below and rewrites the header only when the SHA actually changes.
find_package(Git QUIET)
set(CASCADE_COMMIT_HEADER_DIR "${CMAKE_BINARY_DIR}/generated")
set(CASCADE_COMMIT_HEADER "${CASCADE_COMMIT_HEADER_DIR}/cascade_git_commit.h")
file(MAKE_DIRECTORY "${CASCADE_COMMIT_HEADER_DIR}")
# Once at configure time as well, so the header exists before the first build
# of anything that includes it.
execute_process(COMMAND "${CMAKE_COMMAND}" "-DGIT=${GIT_EXECUTABLE}"
                        "-DTREE=${CMAKE_CURRENT_SOURCE_DIR}"
                        "-DOUT=${CASCADE_COMMIT_HEADER}"
                        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/git-commit.cmake")
add_custom_target(cascade_git_commit ALL
    COMMAND "${CMAKE_COMMAND}" "-DGIT=${GIT_EXECUTABLE}"
            "-DTREE=${CMAKE_CURRENT_SOURCE_DIR}"
            "-DOUT=${CASCADE_COMMIT_HEADER}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/git-commit.cmake"
    COMMENT "Reading HEAD for the commit stamped into this build"
    VERBATIM)

# Where the symbols for every build of this tree are kept. See
# tools/archive-symbols.ps1 and docs/DIAGNOSTICS.md - without the PDB that
# matches a shipped binary, every crash report against it is unreadable hex
# forever, and PDBs are not shipped to users.
set(CASCADE_SYMBOL_ARCHIVE "${CMAKE_CURRENT_SOURCE_DIR}/symbols")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# RELEASE BUILDS MUST PRODUCE PDBs. Without /Zi and /DEBUG the linker writes no
# PDB and no CodeView record, and a crash report from the field can never be
# read - that is the one failure in this whole diagnostics feature that cannot
# be repaired after the fact.
#
# /Zi is a debug-INFORMATION switch, not an optimisation switch: the generated
# code is byte-for-byte what /O2 produced without it. /DEBUG on its own would
# disable the linker's own optimisations, so /OPT:REF and /OPT:ICF are restored
# explicitly - this is exactly what CMake's own RelWithDebInfo does, applied to
# Release so that the configuration we actually ship is the one carrying
# symbols.
if(MSVC)
    string(APPEND CMAKE_C_FLAGS_RELEASE " /Zi")
    string(APPEND CMAKE_CXX_FLAGS_RELEASE " /Zi")
    string(APPEND CMAKE_EXE_LINKER_FLAGS_RELEASE " /DEBUG /OPT:REF /OPT:ICF")
    string(APPEND CMAKE_SHARED_LINKER_FLAGS_RELEASE " /DEBUG /OPT:REF /OPT:ICF")
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ---------------------------------------------------------------------------
# CASCADE_ANDROID — the portable-core subset for the Android port.
#
# ON automatically when configuring through the NDK's toolchain file
# (CMAKE_SYSTEM_NAME is "Android" in that case), so a plain
# -DCMAKE_TOOLCHAIN_FILE=.../android.toolchain.cmake configure needs no
# second flag. It can ALSO be set explicitly on a normal Linux x86-64
# configure with no NDK at all — that builds and tests the exact same
# exclusion set natively, which is how this subset is proven coherent
# without a device (see tests/CMakeLists.txt).
#
# Under it: no GLFW, no PortAudio, no SoapySDR (find_package does not even
# run — the ABI does not exist on Android), no OpenSSL (the NDK ships
# neither; see the ANDROID-TODO(auth-crypto)/(uploads-via-java) stubs in
# net/web_auth.cpp, core/plugin_repo.cpp, core/crash_upload.cpp,
# core/telemetry.cpp), no cascade executable, no tests glob, no
# symbol-archive post-build step. What remains — src/dsp, most of src/core,
# the native SDR drivers in src/source, src/usb, src/net minus TLS — builds
# into ONE static library, cascade_core (see the cascade_lib target below).
#
# This is a compile definition of cascade_lib as well as a CMake option: C++
# code guards the same exclusions with "#if defined(CASCADE_ANDROID)" rather
# than the compiler-supplied "__ANDROID__", precisely so the host-native
# validation configure exercises the identical code path a real NDK build
# takes, instead of a hand-maintained parallel one.
option(CASCADE_ANDROID "Build the portable-core subset for Android (no GUI, audio, SoapySDR, or OpenSSL)" OFF)
if(CMAKE_SYSTEM_NAME STREQUAL "Android")
    set(CASCADE_ANDROID ON CACHE BOOL "" FORCE)
endif()
# True only for a REAL NDK cross-compile (device or emulator ABI), never for
# the native Linux x86-64 validation configure above. tests/CMakeLists.txt
# uses this to tell "no device to run anything on, skip tests entirely" apart
# from "prove the subset on the host, build a curated test list".
if(CMAKE_SYSTEM_NAME STREQUAL "Android")
    set(CASCADE_ANDROID_CROSS_TARGET ON)
else()
    set(CASCADE_ANDROID_CROSS_TARGET OFF)
endif()
if(CASCADE_ANDROID)
    message(STATUS "cascade: CASCADE_ANDROID is ON (cross-target: ${CASCADE_ANDROID_CROSS_TARGET})")
endif()

# ---------------------------------------------------------------------------
# External dependencies (deliberately minimal — see header comment)
# ---------------------------------------------------------------------------

# libunwind, Linux only - the LOCAL unwinder src/core/crash_handler_posix.cpp
# and src/core/hang_watchdog.cpp use to walk a stack from a ucontext_t/live
# thread, the same role RtlVirtualUnwind plays on Windows. Not vendored: it is
# a system ABI-adjacent library the way SoapySDR and OpenSSL are, and pulling
# in our own copy would strand it on whatever DWARF unwind quirks the
# distribution's own libunwind has already been patched for.
#
# NOT under CASCADE_ANDROID: the NDK ships no libunwind local-unwind API, so
# crash_handler_posix.cpp uses clang's compiler-runtime _Unwind_Backtrace
# there instead (<unwind.h>, no separate library — see that file's header)
# and needs this package not at all; hang_watchdog.cpp's equivalent capture
# is still compiled out under CASCADE_ANDROID — see its own
# ANDROID-TODO(crash-capture) notes; that slice is unaffected by this one.
# Skipping the lookup, not just the link, matters here: this find_package is
# REQUIRED, and CASCADE_ANDROID's whole point (including the native Linux
# x86-64 validation build) is not needing libunwind at all.
if(NOT WIN32 AND NOT CASCADE_ANDROID)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBUNWIND REQUIRED IMPORTED_TARGET libunwind)
endif()

# Desktop OpenGL: the GUI's renderer (Dear ImGui's OpenGL3 backend). Not
# needed under CASCADE_ANDROID, where the whole GUI is excluded (see
# CASCADE_GUI below) — Android has no desktop GL, and the eventual on-device
# renderer is a separate slice's concern (GLES/Vulkan via the vendored
# imgui_impl_android backend, not yet wired to any target).
if(NOT CASCADE_ANDROID)
    find_package(OpenGL REQUIRED)
endif()
# Hardware sources (P4+). Target verified from SoapySDRExport.cmake: plain
# `SoapySDR`, shared. Vendor support (B200 via SoapyUHD, RTL dongles, etc.)
# arrives through SoapySDR's runtime-loaded modules, keeping this tree MIT.
# NOT vendored, by design — the ABI-boundary exception documented above and
# in third_party/THIRD_PARTY.md.
#
# NOT under CASCADE_ANDROID: SoapySDR's vendor-module ABI does not exist on
# Android, so find_package must not even be attempted (there is no vcpkg
# toolchain feeding it there) — every source that reaches it
# (soapy_source.cpp, soapy_modules.cpp, soapy_enum_proc.cpp,
# soapy_log_bridge.cpp, vendor_guard.cpp) is excluded below instead. The
# native drivers (rtlsdr, hackrf, airspy, ...) reach hardware through
# src/usb directly and need nothing here.
if(NOT CASCADE_ANDROID)
    find_package(SoapySDR CONFIG REQUIRED)
endif()

# OpenSSL, non-Windows non-Android only. Windows takes SHA-256/PBKDF2/entropy
# from CNG (bcrypt.lib), which is part of the OS and needs no package; every
# other desktop platform takes the same three primitives from OpenSSL.
# Apache-2.0, so it satisfies the permissive-only rule, and it is a system
# library rather than a vendored one for the same reason SoapySDR is: it is
# the platform's crypto, and shipping our own copy would strand it on the
# distribution's security updates.
#
# NOT under CASCADE_ANDROID: the NDK ships no OpenSSL at all (it is not part
# of the public NDK API on any recent version). net/web_auth.cpp's crypto,
# core/plugin_repo.cpp's httpsGet, core/crash_upload.cpp's and
# core/telemetry.cpp's POSIX posts, and cpp-httplib's SSL half are all
# stubbed or narrowed under CASCADE_ANDROID instead of linking this — see the
# ANDROID-TODO(auth-crypto) / ANDROID-TODO(uploads-via-java) comments in
# those files. HTTPS on Android is planned to go through Java's
# HttpsURLConnection over JNI in a later slice.
if(NOT WIN32 AND NOT CASCADE_ANDROID)
    find_package(OpenSSL REQUIRED)
endif()

# ---------------------------------------------------------------------------
# Vendored dependencies — third_party/, pinned; see THIRD_PARTY.md
# ---------------------------------------------------------------------------

set(CASCADE_TP "${CMAKE_CURRENT_SOURCE_DIR}/third_party")

# GLFW 3.4, PortAudio 19.7 and Dear ImGui's desktop (GLFW+OpenGL3) backend are
# all GUI/audio plumbing with no Android equivalent in this slice — the
# vendored imgui_impl_android backend exists in third_party/ (see
# THIRD_PARTY.md) but is not wired to any target yet, and PortAudio's
# Android counterpart is AAudio, a different agent's slice. None of the
# three are needed to build cascade_core, so none are built under
# CASCADE_ANDROID.
if(NOT CASCADE_ANDROID)
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

elseif(CASCADE_ANDROID_CROSS_TARGET)

# THE SAME LIBRARY, THE OTHER PLATFORM BACKEND. `imgui` is deliberately the
# same TARGET NAME as the desktop's: everything downstream (cascade_lib, the
# gui sources' `#include <imgui.h>`) then needs no Android branch at all, and
# the only difference is which two backends are compiled in.
#
#   imgui_impl_android replaces imgui_impl_glfw - it is what turns an
#   AInputEvent into ImGui's mouse and keys; there is no GLFW here.
#
#   imgui_impl_opengl3 is the SAME renderer backend as the desktop, compiled in
#   GLES mode. That is the whole reason the platform seam was drawn where it
#   was (see gui/platform_window.hpp): desktop GL and GLES ES3 share this
#   backend and differ only in the shader version string.
#
# IMGUI_IMPL_OPENGL_ES3 is PUBLIC and it is load-bearing in both directions:
# it is what makes imgui_impl_opengl3.cpp include <GLES3/gl3.h> instead of
# trying to load desktop GL entry points, and what makes it emit the
# "#version 300 es" shader prologue. Any translation unit of ours that includes
# imgui_impl_opengl3.h - gui/app_window.cpp does - must see the same define, or
# the two disagree about which GL they are talking to.
#
# imgui_demo.cpp is absent on purpose: a megabyte of example code nothing on
# the phone calls.
add_library(imgui STATIC
    "${CASCADE_TP}/imgui/imgui.cpp"
    "${CASCADE_TP}/imgui/imgui_draw.cpp"
    "${CASCADE_TP}/imgui/imgui_tables.cpp"
    "${CASCADE_TP}/imgui/imgui_widgets.cpp"
    "${CASCADE_TP}/imgui/misc/cpp/imgui_stdlib.cpp"
    "${CASCADE_TP}/imgui/backends/imgui_impl_android.cpp"
    "${CASCADE_TP}/imgui/backends/imgui_impl_opengl3.cpp")
target_include_directories(imgui SYSTEM PUBLIC
    "${CASCADE_TP}/imgui"
    "${CASCADE_TP}/imgui/backends"
    "${CASCADE_TP}/imgui/misc/cpp")
target_compile_definitions(imgui PUBLIC IMGUI_IMPL_OPENGL_ES3)
# imgui_impl_android.cpp needs <android/native_window.h> and friends; the
# renderer needs GLESv3. EGL is the platform window's rather than ImGui's, and
# comes along for anything that links this.
target_link_libraries(imgui PUBLIC android log GLESv3 EGL)

# android_native_app_glue: the NDK's own NativeActivity <-> native-thread glue.
# NDK-PROVIDED, not vendored - it ships inside the NDK and is compiled from
# there, so there is no copy in this repository to go stale and nothing to
# record in THIRD_PARTY.md. It is C and it is upstream, so it builds at its own
# defaults with none of our warning flags imposed on it.
#
# Built HERE rather than in src/platform/android because gui/
# platform_window_android.cpp - which is part of cascade_lib - includes
# <android_native_app_glue.h>: the platform window IS the thing that pumps the
# activity's looper.
set(CASCADE_GLUE_DIR "${ANDROID_NDK}/sources/android/native_app_glue")
if(NOT EXISTS "${CASCADE_GLUE_DIR}/android_native_app_glue.c")
    message(FATAL_ERROR "native_app_glue not found at ${CASCADE_GLUE_DIR}")
endif()
add_library(native_app_glue STATIC "${CASCADE_GLUE_DIR}/android_native_app_glue.c")
target_include_directories(native_app_glue PUBLIC "${CASCADE_GLUE_DIR}")
target_link_libraries(native_app_glue PUBLIC android log)

endif()  # NOT CASCADE_ANDROID / CASCADE_ANDROID_CROSS_TARGET

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
# THE SERVER SPEAKS PLAIN HTTP ON EVERY PLATFORM. That is why the bind policy
# treats loopback and LAN differently and why remote access is documented as
# "terminate TLS in front of it" rather than "open the port".
#
# The CLIENT is a separate matter. On Windows the plugin catalogue is fetched
# through WinHTTP, so this stays a plain header-only include. Elsewhere there
# is no WinHTTP, and plugin_repo.cpp compiles this header with
# CPPHTTPLIB_OPENSSL_SUPPORT to get an HTTPS client — which adds no new
# dependency, because OpenSSL is already linked there for SHA-256 and PBKDF2.
# Enabling it for the client does not enable it for the server: the server is
# constructed as a plain httplib::Server and never as an SSLServer.
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
# Linux-only: sigaction/libunwind/dl_iterate_phdr, none of which exist on
# Windows, so this is never handed to MSVC even though the glob above would
# otherwise pick it up automatically like every other src/core/*.cpp.
if(WIN32)
    list(REMOVE_ITEM CASCADE_CORE "${CMAKE_CURRENT_SOURCE_DIR}/src/core/crash_handler_posix.cpp")
endif()
if(CASCADE_ANDROID)
    # crash_handler_posix.cpp USED TO BE REMOVED HERE, one level past the
    # WIN32 removal above: the NDK ships no libunwind local-unwind API, so it
    # looked like the same exclusion for the same reason. It compiles fine —
    # the file picks between libunwind and clang's compiler-runtime
    # _Unwind_Backtrace (<unwind.h>, part of every Android binary, no
    # separate library) internally via "#if defined(CASCADE_ANDROID)", the
    # same macro this whole file filters on, so the host-native validation
    # configure below (no NDK) exercises the identical branch a real device
    # build does. See crash_handler_posix.cpp's file header for the two
    # unwinders and why the technique is safe on both. libunwind itself is
    # still never looked for under CASCADE_ANDROID (the pkg_check_modules
    # guard above) or linked (the target_link_libraries guard near the
    # bottom of this file) — Android's branch needs neither.
    #
    # gps_reader.cpp used to be removed here: it pulls in "gui/scope_view.hpp"
    # for one acceptance-rule constant, and nothing in a GUI-less core build
    # reached GPS. Both halves of that reasoning went when src/gui was
    # compiled for Android - AppWindow owns a GpsReader and starts it from the
    # rail - so the file is built, and the honest degradation is a RUNTIME one
    # rather than a link-time absence: there is no serial port on a phone, so
    # the port list is empty and the page says so. (Android's own position
    # source is LocationManager over JNI, still a later slice.)
endif()
file(GLOB CASCADE_DSP    CONFIGURE_DEPENDS src/dsp/*.cpp)
file(GLOB CASCADE_GUI CONFIGURE_DEPENDS src/gui/*.cpp)
if(CASCADE_ANDROID_CROSS_TARGET)
    # THE WHOLE GUI IS COMPILED FOR ANDROID, and lifting this exclusion is the
    # point of this slice: app_window.cpp, the spectrum, the waterfall, the
    # instrument faces, the pages - the real desktop interface, on the phone,
    # because the owner asked for the same layout rather than a phone-shaped
    # one. It builds against the SAME `imgui` target name (the Android
    # backends, above) and the platform seam gui/platform_window.hpp, so not
    # one of those files needed an Android branch for the WINDOW.
    #
    # Exactly one file goes: platform_window_glfw.cpp, which IS GLFW. Its
    # counterpart gui/platform_window_android.cpp is in this same glob and is
    # compiled only here - it includes EGL, ANativeWindow and
    # android_native_app_glue, so the desktop build must never see it either
    # (the exclusion below, in the NOT-Android branch).
    list(REMOVE_ITEM CASCADE_GUI
         "${CMAKE_CURRENT_SOURCE_DIR}/src/gui/platform_window_glfw.cpp")
    # The touch translation the platform window drives: a long press becomes a
    # right click and a two-finger drag becomes the wheel, because the desktop
    # interface uses both and a touchscreen has neither. It lives under
    # src/platform/android with the rest of the shell, so the glob above does
    # not reach it.
    list(APPEND CASCADE_GUI "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/android/input.cpp")
elseif(CASCADE_ANDROID)
    # THE HOST-NATIVE VALIDATION CONFIGURE (-DCASCADE_ANDROID=ON with no NDK)
    # keeps the GUI out, and it has to: gui/platform_window_android.cpp and
    # app_window.cpp's Android branches need EGL, GLESv3 and the NDK's
    # android/* headers, none of which exist on a Linux host, and the desktop
    # ImGui target is not built in this configuration either. What this
    # configure is FOR is unchanged - proving the portable core's exclusion set
    # is coherent without a device - and the GUI half is proved by the real
    # cross-build instead.
    #
    # A handful of gui/*.hpp headers are still reached from in-scope code
    # (core/config.cpp, net/web_server.cpp) for pure data/logic they declare —
    # demod_scope.hpp, rail_banks.hpp, scope_view.hpp, track_metrics.hpp,
    # font_blobs.hpp, plus page_geometry.hpp, viewport_policy.hpp,
    # list_pick.hpp, ui_scale.hpp, android_window_logic.hpp and
    # shell_open.hpp reached from elsewhere — and every one of them includes no
    # GLFW/ImGui/NDK header itself, so leaving the whole directory on the
    # include path (see target_include_directories below) costs nothing.
    set(CASCADE_GUI "")
else()
    # The desktop never sees the Android window: EGL, ANativeWindow and
    # android_native_app_glue are NDK sysroot headers.
    list(REMOVE_ITEM CASCADE_GUI
         "${CMAKE_CURRENT_SOURCE_DIR}/src/gui/platform_window_android.cpp")
endif()
file(GLOB CASCADE_SOURCE CONFIGURE_DEPENDS src/source/*.cpp)
if(CASCADE_ANDROID)
    # SoapySDR-only sources — see the "NOT CASCADE_ANDROID" guard around
    # find_package(SoapySDR) above for why the ABI itself is unavailable.
    # vendor_guard.cpp exists solely to protect soapy_enum_proc.cpp's calls
    # into SoapySDR::Device::enumerate(), so it goes with them. Every native
    # driver (rtlsdr, hackrf, airspy, airspyhf, rx888, mirisdr, msi2500,
    # pluto/iiod) reaches hardware through src/usb or a socket instead and is
    # untouched.
    list(FILTER CASCADE_SOURCE EXCLUDE REGEX
         "/(soapy_source|soapy_modules|soapy_enum_proc|soapy_log_bridge|vendor_guard)\\.cpp$")
    # ...and, once src/gui is compiled here, the CLASS still has to exist:
    # AppWindow reaches SoapySource::enumerate(), moduleSearchPaths(),
    # vendorInstalls() and anyDeviceOpen() from a dozen places that have
    # nothing to do with opening a device. soapy_source_android.cpp is the
    # honest refusal - the same shape audio_out/audio_out_aaudio use, one
    # class with two implementations and exactly one compiled. Only added for
    # the cross-target: the host validation configure has no GUI to reference
    # it, and linking two definitions of the class there would be an ODR
    # violation the moment anything else changed.
    if(CASCADE_ANDROID_CROSS_TARGET)
        list(APPEND CASCADE_SOURCE
             "${CMAKE_CURRENT_SOURCE_DIR}/src/source/soapy_source_android.cpp")
    endif()
else()
    # The desktop builds the real SoapySDR-backed class, never the refusal.
    list(FILTER CASCADE_SOURCE EXCLUDE REGEX "/soapy_source_android\\.cpp$")
endif()
file(GLOB CASCADE_SINK   CONFIGURE_DEPENDS src/sink/*.cpp)
# Two backends, one AudioOut class (see audio_out.hpp): audio_out.cpp is
# PortAudio, audio_out_aaudio.cpp is AAudio. Exactly one of them is compiled
# into the library. On Android the PortAudio pair goes (audio_in.cpp too -
# PortAudio has no Android host API; the microphone side is a later slice,
# and transmitter.cpp's use of AudioIn is a known link-time gap there); on
# every other platform the AAudio file goes and is reached only by
# tests/CMakeLists.txt's test_audio_out_aaudio, which compiles it against a
# fake AAudio for its host-testable ring/priming/underrun core.
if(CASCADE_ANDROID)
    # audio_out.cpp (PortAudio) goes; audio_in.cpp STAYS, and that changed
    # with this slice. It was excluded as "the microphone side is a later
    # slice", which was true and stopped being sufficient the moment src/gui
    # was compiled here: the TRANSMIT page holds an AudioIn and would not
    # link. The file now carries a CASCADE_ANDROID branch for exactly the six
    # members that name PortAudio - an empty device list and a refused open,
    # which is how the page degrades honestly - and shares the ring, the meter
    # and the realtime core with the desktop. See its own header comment.
    list(FILTER CASCADE_SINK EXCLUDE REGEX "/audio_out\\.cpp$")
else()
    list(FILTER CASCADE_SINK EXCLUDE REGEX "audio_out_aaudio\\.cpp$")
endif()
file(GLOB CASCADE_NET    CONFIGURE_DEPENDS src/net/*.cpp)
# Native radio drivers talk to the hardware through our own USB transport
# (src/usb) - WinUSB on Windows, usbfs directly on Linux - not through
# SoapySDR modules or libusb: see src/usb/usb_device.hpp.
file(GLOB CASCADE_USB    CONFIGURE_DEPENDS src/usb/*.cpp)
# usbfs_device.cpp includes <linux/usbdevice_fs.h> and friends, which exist
# nowhere else; it is compiled ONLY on Linux (Android included — Android IS
# Linux, __linux__ stays defined, and CMake's own UNIX test is not relied on
# here for exactly that reason); every other platform (Windows, macOS, *BSD)
# keeps building against winusb_device.cpp's portable stub for
# enumerateWinUsb()/enumerateUnbound()/openWinUsb() exactly as before this
# file existed. usb_android_bridge.cpp is the extern "C" wrapper the Android
# JNI layer calls into the adopted-device registry declared alongside
# usbfs_device.cpp's own header (src/usb/usbfs_device.hpp) - it has no
# purpose without that registry and #errors on any non-Linux build the same
# way, so it is excluded right beside it. CASCADE_ANDROID is named as well
# as UNIX because the NDK toolchain sets CMAKE_SYSTEM_NAME to Android, for
# which CMake's UNIX is true but is not something this file wants to rely on.
if(NOT ((UNIX AND NOT APPLE) OR CASCADE_ANDROID))
    list(FILTER CASCADE_USB EXCLUDE REGEX "usbfs_device\\.cpp$")
    list(FILTER CASCADE_USB EXCLUDE REGEX "usb_android_bridge\\.cpp$")
endif()

add_library(cascade_lib STATIC
    ${CASCADE_CORE} ${CASCADE_DSP} ${CASCADE_GUI} ${CASCADE_SOURCE} ${CASCADE_SINK}
    ${CASCADE_NET} ${CASCADE_USB})
# The name the Android port knows this library by: everything above and below
# this line still calls it cascade_lib (renaming it would touch every test
# target in tests/CMakeLists.txt for no behavioural gain), but under
# CASCADE_ANDROID it is exactly the portable core the port's build scripts
# and CI ask for as `cascade_core`.
add_library(cascade_core ALIAS cascade_lib)

target_include_directories(cascade_lib PUBLIC src "${CASCADE_COMMIT_HEADER_DIR}")
# See src/core/version.cpp: the reported version is injected, never written in.
target_compile_definitions(cascade_lib PRIVATE
    CASCADE_VERSION_STRING="${CASCADE_VERSION_STRING}"
    CASCADE_VERSION_SUFFIX="${CASCADE_VERSION_SUFFIX}")
# The commit comes from the generated header instead, so it is re-read on every
# build rather than frozen at configure time.
add_dependencies(cascade_lib cascade_git_commit)
if(CASCADE_ANDROID)
    # The single macro every "#if defined(CASCADE_ANDROID)" branch in src/
    # tests for — see net/web_auth.cpp, core/plugin_repo.cpp,
    # core/crash_upload.cpp, core/telemetry.cpp, core/crash_handler.cpp and
    # core/hang_watchdog.cpp. PUBLIC so a test binary built against
    # cascade_lib in the host-subset configuration (tests/CMakeLists.txt)
    # sees the same definition the library itself was compiled with.
    target_compile_definitions(cascade_lib PUBLIC CASCADE_ANDROID)
else()
    # PUBLIC, and only these two: the test binaries need to find the application
    # binary and the symbol archive in order to assert that this build's PDB was
    # actually archived under the build id a report would quote. Nothing in the
    # library itself reads them. Meaningless under CASCADE_ANDROID, where
    # neither `cascade` nor the symbol archive step exist — see the
    # "Application" section below.
    target_compile_definitions(cascade_lib PUBLIC
        CASCADE_APP_BINDIR="$<TARGET_FILE_DIR:cascade>"
        CASCADE_SYMBOL_ARCHIVE_DIR="${CASCADE_SYMBOL_ARCHIVE}")
endif()
# ...and these three so tests/test_diagnostics.cpp can hold the commit a
# report quotes to what git says HEAD is RIGHT NOW, and can run the commit
# generator against a scratch repository whose HEAD moves under it. Paths
# rather than a PATH lookup: a test that silently found a different git (or
# none) would pass by not testing anything. Kept PUBLIC under CASCADE_ANDROID
# too: nothing about reading git depends on the desktop GUI/audio/SoapySDR
# exclusions, and the host-subset build still has this test's dependencies.
target_compile_definitions(cascade_lib PUBLIC
    CASCADE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    CASCADE_GIT_EXECUTABLE="${GIT_EXECUTABLE}"
    CASCADE_CMAKE_COMMAND="${CMAKE_COMMAND}")
# Vendored targets are listed BEFORE SoapySDR so third_party/ include dirs
# always precede vcpkg's shared installed-include root (which still contains
# same-named headers for other consumers of that vcpkg tree). Under
# CASCADE_ANDROID none of imgui/glfw/OpenGL/portaudio_static/SoapySDR were
# even built above, so linking them is skipped along with them.
target_link_libraries(cascade_lib PUBLIC
    cascade_flags
    cascade_json
    cascade_pffft
    cascade_httplib)
if(NOT CASCADE_ANDROID)
    target_link_libraries(cascade_lib PUBLIC
        imgui
        glfw
        OpenGL::GL
        portaudio_static
        SoapySDR)
elseif(CASCADE_ANDROID_CROSS_TARGET)
    # The same ImGui (with the Android backends), the NativeActivity glue the
    # platform window pumps, GLES for the renderer and EGL for the surface.
    # aaudio is the sink's: src/sink/audio_out_aaudio.cpp replaces the
    # PortAudio one in this configuration.
    target_link_libraries(cascade_lib PUBLIC
        imgui
        native_app_glue
        GLESv3
        EGL
        aaudio
        android
        log)
endif()

if(WIN32)
    # The native drivers' USB transport: SetupAPI/CfgMgr for enumeration by
    # device interface (never opening a device to list it), WinUSB for the
    # pipes. All three are inbox on Windows 10+, so the installer payload
    # contract (cascade.exe + SoapySDR.dll) does not change.
    target_link_libraries(cascade_lib PUBLIC winusb setupapi cfgmgr32)
endif()

if(NOT WIN32 AND NOT CASCADE_ANDROID)
    # Crypto for the password/digest primitives, SSL for the plugin
    # catalogue's HTTPS client (cpp-httplib built with
    # CPPHTTPLIB_OPENSSL_SUPPORT). Windows takes both from the OS instead —
    # CNG and WinHTTP — which is why neither is a dependency there. Android
    # takes neither: see the find_package(OpenSSL)/pkg_check_modules(LIBUNWIND)
    # guards above and the ANDROID-TODO stubs they point at.
    target_link_libraries(cascade_lib PUBLIC OpenSSL::Crypto OpenSSL::SSL)
    # The stack unwinder crash_handler_posix.cpp and hang_watchdog.cpp use.
    # See the find_package(PkgConfig)/pkg_check_modules block above.
    target_link_libraries(cascade_lib PUBLIC PkgConfig::LIBUNWIND)
endif()

# ---------------------------------------------------------------------------
# Application
#
# NONE of this exists under CASCADE_ANDROID: no `cascade` executable (Android
# has no desktop GUI/audio to run one against — see the GLFW/PortAudio/ImGui
# guard above), so nothing here that references the `cascade` target — the
# icon, the delay-load, staging SoapySDR.dll beside it, staging the band
# plans, the symbol-archive POST_BUILD step — has anything to attach to.
# foxsdr-reports goes with it: it exists to resolve a `cascade` crash report
# against `symbols/`, neither of which this configuration produces.
# ---------------------------------------------------------------------------
if(NOT CASCADE_ANDROID)

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

# ---------------------------------------------------------------------------
# foxsdr-reports - the reader that makes an uploaded report readable
# ---------------------------------------------------------------------------
#
# A crash report arrives as module+offset, and an offset is unreadable hex
# forever unless the PDB from THAT EXACT LINK can be found. This tool resolves
# them against symbols/ - here, never on the server: the PDBs are the complete
# private debug information for every shipped build, they are tens of megabytes
# each, and uploading them to save a step is not a trade worth making. See
# src/core/report_reader.hpp.
#
# Built by the normal build so it cannot rot: a tool that only compiles when
# somebody remembers to compile it is a tool that will not compile on the day a
# report matters.
add_executable(foxsdr-reports tools/report-reader/main.cpp)
target_link_libraries(foxsdr-reports PRIVATE cascade_lib)

# Delay-load SoapySDR so a missing or broken hardware runtime cannot stop the
# app from starting. Without this the Windows loader kills the process before
# main() with an unactionable "SoapySDR.dll was not found" modal; with it, the
# app opens normally and SoapySource::runtimeAvailable() reports the problem
# in the Source panel while the generator and IQ-file sources keep working.
# Only the shipped executable needs it — the test binaries link it normally and
# are given a copy of the DLL beside them (see tests/CMakeLists.txt).
if(MSVC)
    target_link_options(cascade PRIVATE "/DELAYLOAD:SoapySDR.dll")
    target_link_libraries(cascade PRIVATE delayimp)
endif()

# Stage SoapySDR.dll beside the executable, explicitly.
#
# The installer refuses to package a build without it (a deliberate payload
# check), and until now the file arrived only through vcpkg's implicit
# applocal-deps step. That works in a build tree that has been around a while
# and NOT in a fresh one, so the first build from a clean directory failed to
# package with "SoapySDR.dll not found in build\Release" — and, before that,
# the test binaries hit a Windows "DLL not found" modal that blocks an
# unattended build rather than failing it. Both are one missing copy; neither
# should depend on a side effect of the dependency manager.
if(WIN32 AND TARGET SoapySDR)
    add_custom_command(TARGET cascade POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:SoapySDR> $<TARGET_FILE_DIR:cascade>)
endif()

# Stage the band plans beside the executable.
#
# BandPlan::defaultDir() is deliberately EXECUTABLE-relative, so the overlay
# can only ever find its data next to cascade.exe. Without this copy the
# feature is dead in the build tree, which is exactly how it went unnoticed
# that the installer never packaged the plans either: the model, the palette,
# the overlay renderer, the "what am I tuned to" readout and a full test file
# all worked, and no user has ever been able to see any of it.
add_custom_command(TARGET cascade POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/resources/bandplans"
            "$<TARGET_FILE_DIR:cascade>/resources/bandplans")

# ARCHIVE THE SYMBOLS, in the same action that produced the binary.
#
# This is deliberately a POST_BUILD step on the application rather than a line
# in the release or nightly script. Both of those build through CMake, so this
# covers them; and a step that only runs when someone remembers to run it is a
# step that will eventually be missed - at which point the shipped binary
# exists, its PDB does not, and every crash report against that build is
# unreadable hex forever. There is no repairing that afterwards.
#
# The script never fails the build (a missing PDB is a warning). The assertion
# that this build's symbols really are in the archive, under the build id a
# report would quote, lives in tests/test_diagnostics.cpp.
if(WIN32)
    add_custom_command(TARGET cascade POST_BUILD
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/archive-symbols.ps1"
                -Binary "$<TARGET_FILE:cascade>"
                -ArchiveRoot "${CASCADE_SYMBOL_ARCHIVE}"
                -Version "${CASCADE_VERSION_STRING}"
                # The header, not a variable: it is regenerated every build, so
                # the commit in the index is the commit in the binary. A CMake
                # variable here would be the configure-time SHA, which is what
                # left nine different build ids in symbols\index.txt all
                # stamped with the same commit.
                -CommitHeader "${CASCADE_COMMIT_HEADER}"
        VERBATIM)
else()
    # THE LINUX MIRROR of the step above - see tools/archive-symbols-linux.sh
    # for what differs (no PDB; a split-debug file plus the stripped module,
    # keyed by the GNU build id note instead of a CodeView record) and why it
    # never fails the build. Same governing rule as Windows: this is what
    # keeps symbols/ populated by the SAME action that produces the binary,
    # so no crash report against a Linux build is ever unreadable because
    # someone forgot a separate archiving step existed.
    add_custom_command(TARGET cascade POST_BUILD
        COMMAND bash
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/archive-symbols-linux.sh"
                --binary "$<TARGET_FILE:cascade>"
                --archive-root "${CASCADE_SYMBOL_ARCHIVE}"
                --version "${CASCADE_VERSION_STRING}"
                --commit-header "${CASCADE_COMMIT_HEADER}"
                --module "$<TARGET_FILE_BASE_NAME:cascade>"
        VERBATIM)
endif()

endif()  # NOT CASCADE_ANDROID (Application)

# ---------------------------------------------------------------------------
# The Android application — libfoxsdr.so
#
# The counterpart of the `cascade` executable above: the same cascade_lib,
# entered through android_main instead of main(). LAST, because it links
# cascade_lib and every target cascade_lib links, and CMake wants those
# defined first.
#
# Cross-target only. The host-native validation configure has no NDK, no
# NativeActivity and nothing to run an activity on.
# ---------------------------------------------------------------------------
if(CASCADE_ANDROID_CROSS_TARGET)
    add_subdirectory(src/platform/android)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

enable_testing()
add_subdirectory(tests)
