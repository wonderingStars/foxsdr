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
