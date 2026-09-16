// main.cpp - the Android entry point.
//
// THE COUNTERPART OF src/main.cpp, and now almost the same shape as it. On the
// desktop main() parses a command line, arms the crash handler and the
// diagnostics log, declares a GlfwPlatformWindow and an AppWindow, and hands
// control to AppWindow::run(). Here the framework starts a NativeActivity, the
// NDK's android_native_app_glue spawns a thread for it and calls android_main
// on that thread, and the last three lines are the same three lines: a platform
// window, an AppWindow, and run().
//
// WHAT USED TO BE HERE. A first-screen shell - its own frame loop, its own EGL
// bring-up, its own small spectrum drawn out of the signal generator - which
// existed to prove the DSP, the threading, the surface and the fonts all worked
// for this ABI before the real interface was attempted. It has served its
// purpose and is gone: frame.cpp, egl.cpp and screen_first.cpp were deleted in
// this change and their EGL and lifecycle handling absorbed into
// gui/platform_window_android.cpp. What draws now is the product.
//
// android_main NEVER RETURNS EARLY. Returning from it is how a native activity
// says "I am finished" and the glue then tears the activity down, so the only
// legal exit is AppWindow::run() returning - which it does when the framework
// sets destroyRequested (the back gesture, the task swiped away) or when the
// interface's own close key is pressed.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include <android/log.h>
#include <android_native_app_glue.h>
#include <stdlib.h>

#include <string>

#include "core/config.hpp"
#include "core/diag_log.hpp"
#include "core/version.hpp"
#include "gui/app_window.hpp"
#include "gui/platform_window_android.hpp"

namespace {

constexpr const char* kTag = "FoxSDR";

// EVERY PATH THIS APPLICATION WRITES TO, POINTED INSIDE THE SANDBOX, IN ONE
// PLACE.
//
// PRIVACY, and it is the reason this is done here rather than in six files.
// The portable code derives its own locations from the environment the way a
// Linux desktop does - ConfigStore::defaultPath() from XDG_CONFIG_HOME or
// $HOME/.config, diagBaseDir() from XDG_STATE_HOME or $HOME/.local/state,
// plugin_host's directory from XDG_DATA_HOME or $HOME/.local/share,
// freq_manager from XDG_CONFIG_HOME. On Android none of those variables is set
// and $HOME is either unset or "/", so every one of them falls back to the
// process's working directory - which IS "/" and is not writable: a silent
// failure on every save.
//
// Setting them to the activity's own internalDataPath - /data/data/com.foxsdr/
// files, private to this application and removed when it is uninstalled - puts
// the config, the frequency lists, the log ring's file and the crash directory
// inside the sandbox with one call each and NO change to the portable code.
// The alternative, threading a base directory through four modules, would be
// four places for a later slice to forget.
//
// overwrite = 1 on purpose: a stale value inherited from whatever launched the
// activity must not be able to redirect a write out of the sandbox.
void pointStorageAtTheSandbox(const char* internalDataPath) {
    if (internalDataPath == nullptr || internalDataPath[0] == '\0') { return; }
    const std::string base(internalDataPath);
    setenv("HOME", base.c_str(), 1);
    setenv("XDG_CONFIG_HOME", (base + "/config").c_str(), 1);
    setenv("XDG_STATE_HOME", (base + "/state").c_str(), 1);
    setenv("XDG_DATA_HOME", (base + "/data").c_str(), 1);
    setenv("XDG_CACHE_HOME", (base + "/cache").c_str(), 1);
    setenv("TMPDIR", (base + "/cache").c_str(), 1);
}

}  // namespace

extern "C" void android_main(struct android_app* app) {
    __android_log_print(ANDROID_LOG_INFO, kTag, "android_main: FoxSDR %s (%s)",
                        cascade::versionString(), cascade::gitCommit());

    const char* dataPath =
        (app != nullptr && app->activity != nullptr) ? app->activity->internalDataPath : nullptr;
    pointStorageAtTheSandbox(dataPath);

    // THE DIAGNOSTICS RING, ON; THE FILE BEHIND IT, OFF. A deliberate
    // difference from the desktop, where main() reads the user's stored switch
    // before arming anything because there the log is a file in their profile
    // that they did not ask for. Here the in-memory ring is the only account
    // of a start-up failure anyone will ever have - a phone has no console -
    // and an empty directory means diagLogf writes to the ring and to nothing
    // else, so nothing reaches the disk on a guess. The on-disk half is still
    // governed by the user's switch, which AppWindow::run applies.
    cascade::core::DiagLog::instance().configure(std::string(), false);
    cascade::core::diagLogf("FoxSDR %s (%s) starting on Android", cascade::versionString(),
                            cascade::gitCommit());

    // DECLARED BEFORE THE AppWindow, exactly as on the desktop and for the
    // same reason: AppWindow reads the session clock off the platform window
    // from its destructor's neighbourhood as well as from run(), so a platform
    // window destroyed first would leave a dangling reference for as long as
    // the destructor takes. Reverse declaration order is what makes that
    // impossible.
    cascade::gui::AndroidPlatformWindow platform(app);
    cascade::gui::AppWindow window(cascade::core::ConfigStore::defaultPath(),
                                   /*announceConfig=*/false);

    // NO CRASH DIRECTORY HANDED OVER, and that is not an oversight: crash and
    // hang CAPTURE is stubbed on Android - ANDROID-TODO(crash-capture) in
    // core/crash_handler.cpp and core/hang_watchdog.cpp, because the NDK has
    // no libunwind local-unwind API - so a directory would only ever collect
    // empty reports. An empty string is what the desktop hands a run that may
    // not write, and AppWindow already reads it as "write nothing".
    window.setDiagnosticsDir(std::string());

    // -1: run until the activity is finished. There is no --frames on a phone.
    const int rc = window.run(-1, platform);
    __android_log_print(ANDROID_LOG_INFO, kTag, "android_main: run() returned %d", rc);
}
