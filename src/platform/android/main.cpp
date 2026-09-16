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

#include "core/android_app_info.hpp"
#include "core/config.hpp"
#include "core/crash_handler.hpp"
#include "core/diag_log.hpp"
#include "core/net_post.hpp"
#include "core/plugin_host.hpp"
#include "core/version.hpp"
#include "gui/app_window.hpp"
#include "gui/platform_window_android.hpp"
#include "usb/usb_android_jni.hpp"

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

    // THE ONLY WAY OFF THIS DEVICE, handed over once, here, because here is
    // the only place that has it. The NDK ships no OpenSSL, so crash and usage
    // reports go through Java's HttpsURLConnection over JNI
    // (core/net_post.hpp); that needs the JavaVM and - less obviously - the
    // ACTIVITY OBJECT, because a native worker thread resolves classes through
    // the system class loader, which cannot see com.foxsdr.app.Net at all. The
    // activity is what the transport asks for the application's own loader.
    //
    // Arming it does not send anything and cannot: both senders are still
    // governed by the user's switch, and both are still off until it is on.
    if (app != nullptr && app->activity != nullptr) {
        cascade::core::androidNetInit(app->activity->vm, app->activity->clazz);
    }

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

    // WHERE THE DECODER PLUGINS ARE, and on this platform that is a question
    // only the framework can answer.
    //
    // Google Play forbids an application downloading executable code, so there
    // is no catalogue here and nothing is installed: the modules a user can
    // run are the ones compiled into this signed apk, and the installer put
    // them in the package's own native library directory beside libfoxsdr.so.
    // PluginHost cannot find that by itself - /proc/self/exe in a
    // NativeActivity is /system/bin/app_process64 - so it is handed over here,
    // from the one place that has a Context to ask
    // (core/android_app_info.hpp).
    //
    // AFTER the diagnostics ring is armed, deliberately: both the JNI reader
    // and PluginHost report through diagLogf, and a failure to learn this path
    // is exactly the kind of start-up fact a phone gives nobody any other
    // account of. BEFORE the AppWindow is constructed, because its own
    // start-up reads defaultPluginDir() and scans it.
    if (app != nullptr && app->activity != nullptr) {
        const std::string libDir =
            cascade::core::androidNativeLibraryDir(app->activity->vm, app->activity->clazz);
        cascade::core::PluginHost::setAndroidPluginDir(libDir);
        if (libDir.empty()) {
            cascade::core::diagLogf(
                "plugins: the native library directory is not known; no bundled module "
                "will be loaded");
        } else {
            cascade::core::diagLogf("plugins: bundled in this package, at %s", libDir.c_str());
        }
    }

    // THE RADIOS, and this is the one call that makes a plugged-in dongle
    // reachable at all on this platform.
    //
    // An Android application may not open a USB device from native code -
    // UsbManager, its permission dialog and the descriptor it hands out are
    // Java-only, and /dev/bus/usb is not readable by an app whatever its
    // manifest says. So the sequence lives in Java
    // (android/app/src/main/java/com/foxsdr/app/Usb.java) and this binds its
    // native methods and tells it to start; from then on every driver's
    // ordinary enumerateWinUsb()/openWinUsb() pair sees the device exactly as
    // the sysfs walk presents one on desktop Linux.
    //
    // AFTER the diagnostics ring, like the two calls above and for the same
    // reason: the attach, the permission answer and the registration are all
    // reported through diagLogf, and on a tablet that log is the only account
    // of why a radio did or did not appear. BEFORE the AppWindow, so a device
    // already attached at launch is registered by the time the Source section
    // first scans - and if it is not, the scan is ungated and picks it up on
    // the next one anyway.
    if (app != nullptr && app->activity != nullptr) {
        cascade::usb::androidUsbInit(app->activity->vm, app->activity->clazz);
    }

    // THE USER'S STORED PREFERENCE, read before anything is armed - the same
    // rule main.cpp's storedDiagnosticsEnabled() follows and for the same
    // reason: a user who opted out must not get a crashes directory created
    // on their behalf just because a fault happened before Settings was ever
    // opened. ConfigStore::defaultPath() already resolves inside the sandbox
    // (XDG_CONFIG_HOME was pointed at internalDataPath/config just above), and
    // a missing or corrupt file leaves AppConfig's default in place, which is
    // "on" - a user who has never chosen has not opted out.
    cascade::core::AppConfig startupCfg;
    std::string startupCfgErr;
    cascade::core::ConfigStore::load(cascade::core::ConfigStore::defaultPath(), startupCfg,
                                     startupCfgErr);
    const bool wantDiagnostics = startupCfg.diagnosticsEnabled;

    // REGISTRATIONS FIRST, ON-DISK CAPTURE SECOND - exactly main.cpp's
    // two-step, and for the same reason: AndroidPlatformWindow's EGL bring-up
    // and AppWindow's own construction below are exactly the kind of
    // fault-prone start-up work a report needs to cover, and both run before
    // AppWindow::run() ever reaches its internal applyDiagnosticsEnabled()
    // call (see app_window.cpp, armed once the frame loop starts). Crash
    // capture is no longer stubbed here - crash_handler_posix.cpp builds
    // under CASCADE_ANDROID too now; see its file header for the
    // _Unwind_Backtrace-based unwinder that replaces the libunwind package
    // the NDK does not ship.
    cascade::core::CrashHandlerConfig crashCfg;
    crashCfg.crashDir = cascade::core::diagCrashDir();
    crashCfg.enabled = false;
    cascade::core::installCrashHandlers(crashCfg);
    cascade::core::setCrashCaptureEnabled(wantDiagnostics, false);

    // DECLARED BEFORE THE AppWindow, exactly as on the desktop and for the
    // same reason: AppWindow reads the session clock off the platform window
    // from its destructor's neighbourhood as well as from run(), so a platform
    // window destroyed first would leave a dangling reference for as long as
    // the destructor takes. Reverse declaration order is what makes that
    // impossible.
    cascade::gui::AndroidPlatformWindow platform(app);
    cascade::gui::AppWindow window(cascade::core::ConfigStore::defaultPath(),
                                   /*announceConfig=*/false);

    // THE CRASH DIRECTORY, HANDED OVER. diagCrashDir() resolves under
    // XDG_STATE_HOME (pointed at internalDataPath/state above), which is the
    // SAME directory the existing Diagnostics page already reads through
    // core::diagCrashDir() - no separate wiring needed there, and no risk of
    // the two ever disagreeing. run()'s own applyDiagnosticsEnabled(), fed
    // from the config AppWindow's constructor just loaded (the same file
    // `startupCfg` above already read), re-applies `wantDiagnostics` onto the
    // crash handler, the disk log and the watchdog together - the same single
    // call the desktop's app.setDiagnosticsDir()+run() relies on.
    window.setDiagnosticsDir(cascade::core::diagCrashDir());

    // -1: run until the activity is finished. There is no --frames on a phone.
    const int rc = window.run(-1, platform);
    __android_log_print(ANDROID_LOG_INFO, kTag, "android_main: run() returned %d", rc);
}
