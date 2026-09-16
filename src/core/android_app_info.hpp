// android_app_info.hpp - the few facts about this INSTALLED PACKAGE that only
// the Java side knows, read over JNI.
//
// WHY THIS FILE EXISTS AT ALL. Almost everything the Android port needs is
// reachable from the NDK: the activity's internal data path comes off
// ANativeActivity, the display metrics come off the window, audio comes from
// AAudio. The package's own layout does not. ApplicationInfo is a framework
// object, and the NDK exposes no equivalent - so the one honest way to ask
// "where did the installer put this application's native libraries" is to ask
// the framework, and that means JNI.
//
// THE ANSWER MATTERS BECAUSE THE DECODER PLUGINS LIVE THERE. Google Play
// forbids an application downloading executable code, so the plugin catalogue
// this product serves on the desktop cannot exist here: the modules an Android
// user can run are the ones compiled into the signed apk, and the platform
// installs them into the application's native library directory beside
// libfoxsdr.so. That directory is what core/plugin_host.cpp scans (see
// PluginHost::chooseAndroidPluginDir), and neither of its two desktop
// candidates can find it.
//
// WHAT THIS IS NOT. It is not a general framework bridge and it does not hold
// state. One call, one string, no globals kept afterwards - the caller (
// src/platform/android/main.cpp) hands the answer straight to the one module
// that needs it. The transport that DOES keep JNI state, because it has to
// reach a class of ours from a worker thread, is core/net_post.hpp; this
// header deliberately borrows its attach/exception discipline and none of its
// lifetime.
//
// EVERY DECLARATION IS COMPILED ON EVERY PLATFORM. The non-Android
// implementation answers with an empty string, which is what lets the rule
// that consumes it (chooseAndroidPluginDir) be pinned by a test on a Linux
// host rather than only on a device.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_ANDROID_APP_INFO_HPP
#define CASCADE_CORE_ANDROID_APP_INFO_HPP

#include <string>

namespace cascade::core {

// ApplicationInfo.nativeLibraryDir for the running package, or EMPTY.
//
// `javaVm` is a JavaVM* and `activityObject` the activity jobject - exactly
// what android_main is handed (app->activity->vm and app->activity->clazz),
// passed as void* so this header carries no jni.h dependency into portable
// code.
//
// EMPTY ON EVERY FAILURE, and the failures are not equivalent to an error: a
// non-Android build has no VM and answers empty by definition, and on a device
// a null VM, a detachable thread that will not attach, or a framework call
// that throws all mean the same thing to the caller - nobody told us where the
// libraries are. What must NOT happen is a plausible-looking path invented
// here; see PluginHost::chooseAndroidPluginDir for why the empty answer is the
// honest one and what the user is shown instead.
//
// Safe to call from the thread android_main runs on; attaches to the VM if
// that thread is not already attached and detaches only if it did.
std::string androidNativeLibraryDir(void* javaVm, void* activityObject);

}  // namespace cascade::core

#endif  // CASCADE_CORE_ANDROID_APP_INFO_HPP
