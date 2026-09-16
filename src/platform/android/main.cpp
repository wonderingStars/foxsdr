// main.cpp - the Android entry point.
//
// THE COUNTERPART OF src/main.cpp, and deliberately the only thing in this
// directory that knows it. On the desktop main() parses a command line, sets
// up the crash handler and the diagnostics log, and hands control to
// AppWindow::run. Here the framework starts a NativeActivity, the NDK's
// android_native_app_glue spawns a thread for it and calls android_main on
// that thread, and everything after that is the frame loop.
//
// android_main NEVER RETURNS EARLY. Returning from it is how a native activity
// says "I am finished"; the glue then tears the activity down. So the only
// legal exit is the one runFrameLoop takes when the framework has set
// destroyRequested.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include <android/log.h>
#include <android_native_app_glue.h>

#include "frame.hpp"

extern "C" void android_main(struct android_app* app) {
    __android_log_print(ANDROID_LOG_INFO, "FoxSDR", "android_main");
    cascade::platform::android::runFrameLoop(app);
}
