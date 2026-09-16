# The FoxSDR Android application shell.
#
# Reached only through the root CMakeLists.txt's `if(CASCADE_ANDROID)` early
# return, which is itself reached only from android/app/build.gradle. Nothing
# here is ever configured by a desktop build.
#
# WHAT THIS BUILDS, and the order matters for understanding it:
#
#   native_app_glue      the NDK's own NativeActivity <-> native-thread glue.
#                        NDK-PROVIDED, not vendored: it ships inside
#                        $ANDROID_NDK/sources/android/native_app_glue and is
#                        compiled from there, so there is no copy of it in this
#                        repository to go stale, and nothing to record in
#                        THIRD_PARTY.md.
#
#   imgui_android        Dear ImGui core plus the two backends this platform
#                        uses - imgui_impl_android (vendored at 1.92.8-docking
#                        by commit cb71770) and imgui_impl_opengl3 compiled in
#                        GLES mode. Deliberately a SEPARATE target from the
#                        desktop's `imgui`, which links GLFW and desktop GL.
#
#   cascade_core_min     THE PLACEHOLDER, and the one line that will change.
#                        A parallel slice is adding a CASCADE_ANDROID build of
#                        the portable core as `cascade_core`; until it lands,
#                        this compiles only what the first screen actually
#                        needs - all of src/dsp (self-contained: its only
#                        outside include is <pffft/pffft.h>), the signal
#                        generator and its IqSource adapter, and the two GUI
#                        files that carry the product's look. When the core
#                        arrives, delete this target and change the single
#                        target_link_libraries line at the bottom of this file.
#
#   foxsdr               the shared library the NativeActivity loads, named by
#                        android.app.lib_name in AndroidManifest.xml.
#
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

cmake_minimum_required(VERSION 3.22)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CASCADE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../..")
set(CASCADE_TP "${CASCADE_ROOT}/third_party")

if(NOT DEFINED ANDROID_NDK)
    # The NDK toolchain file always sets this; a build that somehow got here
    # without it would fail much later with a confusing missing-header error.
    message(FATAL_ERROR "CASCADE_ANDROID=ON but ANDROID_NDK is not set - this "
                        "file must be configured through the NDK toolchain.")
endif()

# ---------------------------------------------------------------------------
# NDK-provided: android_native_app_glue
# ---------------------------------------------------------------------------
#
# It is C, not C++, and it is upstream code, so it builds at its own defaults
# with no warning flags of ours imposed on it.
set(CASCADE_GLUE_DIR "${ANDROID_NDK}/sources/android/native_app_glue")
if(NOT EXISTS "${CASCADE_GLUE_DIR}/android_native_app_glue.c")
    message(FATAL_ERROR "native_app_glue not found at ${CASCADE_GLUE_DIR}")
endif()
add_library(native_app_glue STATIC "${CASCADE_GLUE_DIR}/android_native_app_glue.c")
target_include_directories(native_app_glue PUBLIC "${CASCADE_GLUE_DIR}")
target_link_libraries(native_app_glue PUBLIC android log)

# ---------------------------------------------------------------------------
# Dear ImGui, in GLES mode
# ---------------------------------------------------------------------------
#
# IMGUI_IMPL_OPENGL_ES3 is PUBLIC and it is load-bearing in both directions:
# it is what makes imgui_impl_opengl3.cpp include <GLES3/gl3.h> instead of
# trying to load desktop GL entry points, and what makes it emit the
# "#version 300 es" shader prologue. Any translation unit of OURS that includes
# imgui_impl_opengl3.h must see the same define, or the two disagree about
# which GL they are talking to.
add_library(imgui_android STATIC
    "${CASCADE_TP}/imgui/imgui.cpp"
    "${CASCADE_TP}/imgui/imgui_draw.cpp"
    "${CASCADE_TP}/imgui/imgui_tables.cpp"
    "${CASCADE_TP}/imgui/imgui_widgets.cpp"
    "${CASCADE_TP}/imgui/misc/cpp/imgui_stdlib.cpp"
    "${CASCADE_TP}/imgui/backends/imgui_impl_android.cpp"
    "${CASCADE_TP}/imgui/backends/imgui_impl_opengl3.cpp")
target_include_directories(imgui_android SYSTEM PUBLIC
    "${CASCADE_TP}/imgui"
    "${CASCADE_TP}/imgui/backends"
    "${CASCADE_TP}/imgui/misc/cpp")
target_compile_definitions(imgui_android PUBLIC IMGUI_IMPL_OPENGL_ES3)
# imgui_impl_android.cpp includes <android/native_window.h> and friends; the
# renderer needs GLESv3. EGL is the shell's, not ImGui's, but it comes along
# for anything that links this.
target_link_libraries(imgui_android PUBLIC android log GLESv3 EGL)
# imgui_demo.cpp is deliberately absent. It is a megabyte of example code that
# nothing on the phone calls, and the first screen is a real instrument rather
# than the demo window.

# ---------------------------------------------------------------------------
# pffft - the FFT behind dsp/fft.cpp
# ---------------------------------------------------------------------------
add_library(cascade_pffft_android STATIC "${CASCADE_TP}/pffft/pffft.c")
target_include_directories(cascade_pffft_android SYSTEM PUBLIC "${CASCADE_TP}")
target_link_libraries(cascade_pffft_android PUBLIC m)

# ---------------------------------------------------------------------------
# cascade_core_min - THE PLACEHOLDER (see the header comment)
# ---------------------------------------------------------------------------
file(GLOB CASCADE_ANDROID_DSP CONFIGURE_DEPENDS "${CASCADE_ROOT}/src/dsp/*.cpp")

add_library(cascade_core_min STATIC
    ${CASCADE_ANDROID_DSP}
    # The hardware-free source the first screen runs on, and its IqSource
    # adapter. Nothing else in src/source can compile here yet: every other
    # source in that directory reaches for src/usb, SoapySDR or a vendor SDK.
    "${CASCADE_ROOT}/src/source/siggen.cpp"
    "${CASCADE_ROOT}/src/source/siggen_source.cpp"
    # The product's palette and its three typefaces. Both are ImGui-only -
    # theme.cpp includes nothing but theme.hpp, fonts.cpp nothing but ImGui and
    # <filesystem> - so the phone wears the same bench the desktop does instead
    # of arriving in default ImGui blue.
    "${CASCADE_ROOT}/src/gui/theme.cpp"
    "${CASCADE_ROOT}/src/gui/fonts.cpp")
target_include_directories(cascade_core_min PUBLIC "${CASCADE_ROOT}/src")
target_compile_options(cascade_core_min PRIVATE -Wall -Wextra)
target_link_libraries(cascade_core_min PUBLIC imgui_android cascade_pffft_android)

# ---------------------------------------------------------------------------
# The shell
# ---------------------------------------------------------------------------
add_library(foxsdr SHARED
    main.cpp
    egl.cpp
    input.cpp
    frame.cpp
    screen_first.cpp)
target_compile_options(foxsdr PRIVATE -Wall -Wextra)
target_compile_definitions(foxsdr PRIVATE
    CASCADE_VERSION_STRING="${CASCADE_VERSION_STRING}")

# THE SWAP POINT. When the portable core lands as `cascade_core`, this is the
# only line that changes: cascade_core_min -> cascade_core, and the target
# above is deleted.
target_link_libraries(foxsdr PRIVATE cascade_core_min)

target_link_libraries(foxsdr PRIVATE native_app_glue imgui_android EGL GLESv3 android log)

# WITHOUT THIS THE APP INSTALLS AND THEN DOES NOTHING.
#
# ANativeActivity_onCreate lives in android_native_app_glue.c and is called by
# the framework by name; nothing in our code references it, so the linker drops
# the whole glue object out of a static archive and the activity starts with no
# entry point. -u forces the symbol to be kept. This is the single most common
# way a NativeActivity build fails silently at runtime rather than at link.
target_link_options(foxsdr PRIVATE "-u" "ANativeActivity_onCreate")
