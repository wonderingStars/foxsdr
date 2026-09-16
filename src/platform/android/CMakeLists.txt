# The FoxSDR Android application: libfoxsdr.so.
#
# Reached from the BOTTOM of the root CMakeLists.txt (under
# CASCADE_ANDROID_CROSS_TARGET), not from an early return at the top of it any
# more. That change is the substance of this slice: there is one CMake tree,
# cascade_lib is built for Android with src/gui compiled into it, and this file
# adds the entry point and links.
#
# WHAT IS LEFT HERE, now that the first-screen shell is gone:
#
#   main.cpp    android_main: point every writable path at the app's sandbox,
#               arm the diagnostics ring, construct AndroidPlatformWindow and
#               AppWindow, call run(). The counterpart of src/main.cpp.
#
#   input.cpp   the touch-to-mouse translation (long press -> right click,
#               two-finger drag -> wheel). NOT built here: it is compiled into
#               cascade_lib beside gui/platform_window_android.cpp, which is
#               what drives it. See the root CMakeLists' CASCADE_GUI list.
#
# WHAT WENT: frame.cpp (the loop is AppWindow::run now), egl.cpp (absorbed into
# gui/platform_window_android.cpp, which needs the surface to come and go
# without the GL context going with it) and screen_first.cpp (the placeholder
# spectrum). The imgui_android, cascade_core_min, cascade_pffft_android and
# native_app_glue targets went too - the first three replaced by the root
# file's `imgui`, cascade_lib and cascade_pffft, and the glue moved up beside
# them because the platform window is what pumps the activity's looper.
#
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

# The shared library the NativeActivity loads, named by android.app.lib_name
# in AndroidManifest.xml.
add_library(foxsdr SHARED main.cpp)
target_compile_options(foxsdr PRIVATE -Wall -Wextra)

# cascade_lib carries its own include dirs, the ImGui target, GLESv3, EGL, the
# NativeActivity glue and AAudio (see the root file), so this is the whole
# dependency list.
target_link_libraries(foxsdr PRIVATE cascade_lib)

# WITHOUT THIS THE APP INSTALLS AND THEN DOES NOTHING.
#
# ANativeActivity_onCreate lives in android_native_app_glue.c and is called by
# the framework BY NAME; nothing in our code references it, so the linker drops
# the whole glue object out of the static archive and the activity starts with
# no entry point. -u forces the symbol to be kept. This is the single most
# common way a NativeActivity build fails silently at runtime rather than at
# link.
target_link_options(foxsdr PRIVATE "-u" "ANativeActivity_onCreate")

# libfoxsdr.so already carries a GNU build-id note without any extra linker
# flag - confirmed by reading a built .so with llvm-readelf -n: the NDK's lld
# defaults to --build-id for an Android target the way it does not always for
# a bare desktop Linux link. Nothing to add here for that; this step is the
# other half - actually ARCHIVING symbols under that id, the same way the
# desktop `cascade` target does (see the root CMakeLists.txt's POST_BUILD
# step). Without it, crash_handler_posix.cpp's reports are exactly as
# symbolisable in principle as the desktop's and exactly as unreadable in
# practice, because nothing kept a copy of THIS build's debug info anywhere
# a symbolication pass could find it again.
#
# tools/archive-symbols-linux.sh is reused UNCHANGED rather than forked: it is
# already keyed by the GNU build-id note (not a version or file name) and
# reads it with elf_symmap.py's own note parser, which does not care what
# produced the ELF file - confirmed by running host objcopy/nm directly
# against a cross-compiled Android x86_64 .so before wiring this in (same
# ELFCLASS64/EM_X86_64 as a native Linux binary; only the ELF OSABI/interp
# differ, and neither tool reads those fields). `--module foxsdr` keys the
# archive under a name distinct from the desktop's `cascade`, so the two
# never collide even though nothing stops a report from either platform
# reaching the same symbols/ tree.
add_custom_command(TARGET foxsdr POST_BUILD
    COMMAND bash
            "${CMAKE_SOURCE_DIR}/tools/archive-symbols-linux.sh"
            --binary "$<TARGET_FILE:foxsdr>"
            --archive-root "${CASCADE_SYMBOL_ARCHIVE}"
            --version "${CASCADE_VERSION_STRING}"
            --commit-header "${CASCADE_COMMIT_HEADER}"
            --module foxsdr
    VERBATIM)
