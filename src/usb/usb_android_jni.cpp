// usb_android_jni.cpp - see usb_android_jni.hpp for what this is and why the
// native methods are BOUND (RegisterNatives) rather than resolved by name.
//
// WHAT JAVA CALLS, AND WHAT EACH CALL IS FOR. Five methods, no more: this
// layer is a translator, not a policy. Which devices to ask about, when to
// ask, and what to do when the answer is no are all decided in
// android/app/src/main/java/com/foxsdr/app/Usb.java, where the Android APIs
// that make those decisions possible actually live.
//
//   nativeLog(String)                 one line into core::diagLogf, so the
//                                     Java half's account of an attach, a
//                                     permission grant and a detach lands in
//                                     the SAME log as the native half's -
//                                     one story, in order, in logcat and in
//                                     the in-memory ring the Diagnostics
//                                     page shows.
//   nativeRegister(...)               foxsdr_usb_register(), plus the log
//                                     line naming the ids and the path.
//   nativeUnregister(String)          foxsdr_usb_unregister().
//   nativeRegisteredCount()           foxsdr_usb_list_count().
//   nativeDescribeRadios()            what the Source section's own scan
//                                     would list right now. The one call
//                                     here that is not a thin wrapper, and
//                                     it earns its place twice over: on a
//                                     real tablet it is the only way to see
//                                     from the log whether a registration
//                                     actually reached the drivers, and it
//                                     is what the on-device self-test
//                                     asserts against.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "usb/usb_android_jni.hpp"

#if defined(__ANDROID__)

#include <jni.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "core/diag_log.hpp"
#include "source/airspy_source.hpp"
#include "source/airspyhf_source.hpp"
#include "source/device_source.hpp"
#include "source/hackrf_source.hpp"
#include "source/mirisdr_source.hpp"
#include "source/rtlsdr_source.hpp"
#include "source/rx888_source.hpp"
#include "usb/usb_android_bridge.h"

namespace cascade::usb {

namespace {

JavaVM* gVm = nullptr;
jobject gActivity = nullptr;  // global ref
std::mutex gJniMutex;

// Attaches this thread to the VM if it is not already attached. `detach` says
// whether the caller must detach: detaching a thread somebody else attached
// would pull the env out from under them. The same two-step
// core/net_post.cpp and core/android_app_info.cpp use, and for the same
// reason - the thread android_main runs on is created by the native glue.
JNIEnv* attach(bool& detach) {
    detach = false;
    if (gVm == nullptr) { return nullptr; }
    JNIEnv* env = nullptr;
    if (gVm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK &&
        env != nullptr) {
        return env;
    }
    if (gVm->AttachCurrentThread(&env, nullptr) != JNI_OK) { return nullptr; }
    detach = true;
    return env;
}

// Clears a pending Java exception and says whether there was one. EVERY JNI
// call below is followed by this: a pending exception makes the NEXT JNI call
// undefined, which is how a missing method turns into a process abort instead
// of a radio that does not appear.
bool threw(JNIEnv* env, const char* what) {
    if (env->ExceptionCheck() == JNI_FALSE) { return false; }
    env->ExceptionDescribe();
    env->ExceptionClear();
    core::diagLogf("usb: JNI %s threw; the Java USB layer is not armed", what);
    return true;
}

// A jstring as a std::string. An absent string is an EMPTY string, not an
// error: UsbDevice.getSerialNumber() needs a permission this application does
// not hold and returns null far more often than not, and a dongle with no
// serial EEPROM has none to report either.
std::string toUtf8(JNIEnv* env, jstring s) {
    if (s == nullptr) { return {}; }
    const char* utf = env->GetStringUTFChars(s, nullptr);
    if (utf == nullptr) { return {}; }
    std::string out(utf);
    env->ReleaseStringUTFChars(s, utf);
    return out;
}

// --- the bound methods ----------------------------------------------------

void jniLog(JNIEnv* env, jclass, jstring line) {
    const std::string text = toUtf8(env, line);
    if (text.empty()) { return; }
    // "usb: " prefixed here rather than in Java, so every line the Java half
    // writes is grep-able the same way the native half's already are, whoever
    // wrote the call.
    core::diagLogf("usb: %s", text.c_str());
}

jstring jniRegister(JNIEnv* env, jclass, jint fd, jint vid, jint pid, jstring serial,
                    jstring product, jstring deviceName) {
    const std::string serialText = toUtf8(env, serial);
    const std::string productText = toUtf8(env, product);
    const std::string nameText = toUtf8(env, deviceName);

    char* path = foxsdr_usb_register(static_cast<int>(fd),
                                     static_cast<std::uint16_t>(vid & 0xFFFF),
                                     static_cast<std::uint16_t>(pid & 0xFFFF),
                                     serialText.empty() ? nullptr : serialText.c_str(),
                                     productText.empty() ? nullptr : productText.c_str());
    if (path == nullptr) {
        // The only way this happens is a negative fd, which is what Java gets
        // from a UsbDeviceConnection it failed to open. Said plainly rather
        // than left as a silent absence.
        core::diagLogf("usb: %s (%04x:%04x) was NOT registered - fd %d is not usable",
                       nameText.c_str(), vid & 0xFFFF, pid & 0xFFFF, static_cast<int>(fd));
        return nullptr;
    }
    core::diagLogf("usb: registered %s (%04x:%04x, serial \"%s\", product \"%s\") as %s; "
                   "%d device(s) now adopted",
                   nameText.c_str(), vid & 0xFFFF, pid & 0xFFFF, serialText.c_str(),
                   productText.c_str(), path, foxsdr_usb_list_count());
    jstring out = env->NewStringUTF(path);
    foxsdr_usb_free_string(path);
    return out;
}

void jniUnregister(JNIEnv* env, jclass, jstring path) {
    const std::string pathText = toUtf8(env, path);
    if (pathText.empty()) { return; }
    foxsdr_usb_unregister(pathText.c_str());
    core::diagLogf("usb: unregistered %s; %d device(s) still adopted", pathText.c_str(),
                   foxsdr_usb_list_count());
}

jint jniRegisteredCount(JNIEnv*, jclass) { return foxsdr_usb_list_count(); }

jstring jniDescribeRadios(JNIEnv* env, jclass) {
    // THE SIX USB ENUMERATIONS THE SOURCE SECTION RUNS, in its own order (see
    // gui/app_window.cpp's AppWindow::scanNative()). The two it also runs and
    // this does not are the SDRplay service query and SoapySDR, neither of
    // which exists on Android - they are stubbed out of this build entirely.
    //
    // None of these opens a device, sends a transfer or resets anything
    // (usb_device.hpp rule 1), so this is safe to call at any time, including
    // while a radio is streaming - which is what lets it be logged on every
    // attach.
    std::vector<cascade::source::NativeDeviceInfo> devices =
        cascade::source::enumerateRtlSdr();
    const auto append = [&devices](std::vector<cascade::source::NativeDeviceInfo> more) {
        for (cascade::source::NativeDeviceInfo& d : more) { devices.push_back(std::move(d)); }
    };
    append(cascade::source::enumerateHackRf());
    append(cascade::source::enumerateAirspy());
    append(cascade::source::enumerateAirspyHf());
    append(cascade::source::enumerateMiriSdr());
    append(cascade::source::enumerateRx888());

    std::string out;
    for (const cascade::source::NativeDeviceInfo& d : devices) {
        if (!out.empty()) { out += " | "; }
        out += d.driver;
        out += ": ";
        out += d.label;
        out += " [";
        out += d.args;
        out += "]";
    }
    if (out.empty()) { out = "(no radio)"; }
    return env->NewStringUTF(out.c_str());
}

const JNINativeMethod kMethods[] = {
    {"nativeLog", "(Ljava/lang/String;)V", reinterpret_cast<void*>(&jniLog)},
    {"nativeRegister",
     "(IIILjava/lang/String;Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",
     reinterpret_cast<void*>(&jniRegister)},
    {"nativeUnregister", "(Ljava/lang/String;)V", reinterpret_cast<void*>(&jniUnregister)},
    {"nativeRegisteredCount", "()I", reinterpret_cast<void*>(&jniRegisteredCount)},
    {"nativeDescribeRadios", "()Ljava/lang/String;",
     reinterpret_cast<void*>(&jniDescribeRadios)},
};

// Resolves com.foxsdr.app.Usb THROUGH THE APPLICATION CLASS LOADER. See the
// header: FindClass() on a native thread uses the system loader, which cannot
// see this application's own classes. Returns a LOCAL ref (the caller uses it
// within this frame only).
jclass resolveUsbClass(JNIEnv* env) {
    if (gActivity == nullptr) { return nullptr; }
    jclass activityCls = env->GetObjectClass(gActivity);
    if (activityCls == nullptr || threw(env, "GetObjectClass(activity)")) { return nullptr; }
    jmethodID getLoader =
        env->GetMethodID(activityCls, "getClassLoader", "()Ljava/lang/ClassLoader;");
    if (getLoader == nullptr || threw(env, "getClassLoader id")) { return nullptr; }
    jobject loader = env->CallObjectMethod(gActivity, getLoader);
    if (loader == nullptr || threw(env, "getClassLoader()")) { return nullptr; }

    jclass loaderCls = env->FindClass("java/lang/ClassLoader");
    if (loaderCls == nullptr || threw(env, "FindClass(ClassLoader)")) { return nullptr; }
    jmethodID loadClass =
        env->GetMethodID(loaderCls, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
    if (loadClass == nullptr || threw(env, "loadClass id")) { return nullptr; }

    jstring name = env->NewStringUTF("com.foxsdr.app.Usb");
    if (name == nullptr) { return nullptr; }
    jobject cls = env->CallObjectMethod(loader, loadClass, name);
    env->DeleteLocalRef(name);
    if (cls == nullptr || threw(env, "loadClass(com.foxsdr.app.Usb)")) { return nullptr; }
    return static_cast<jclass>(cls);
}

}  // namespace

void androidUsbInit(void* javaVm, void* activityObject) {
    const std::lock_guard<std::mutex> lock(gJniMutex);
    if (javaVm == nullptr || activityObject == nullptr) {
        core::diagLogf("usb: no JavaVM or activity; no USB radio can be reached on this "
                       "platform");
        return;
    }
    gVm = static_cast<JavaVM*>(javaVm);
    bool detach = false;
    JNIEnv* env = attach(detach);
    if (env == nullptr) {
        core::diagLogf("usb: could not attach to the JavaVM; the Java USB layer is not armed");
        gVm = nullptr;
        return;
    }
    // A global ref: the activity handed to android_main is a local ref
    // belonging to a frame that is long gone by the time a device is plugged
    // in, and the class-loader lookup above needs it for the life of the
    // process.
    gActivity = env->NewGlobalRef(static_cast<jobject>(activityObject));

    jclass usbCls = resolveUsbClass(env);
    if (usbCls == nullptr) {
        core::diagLogf("usb: com.foxsdr.app.Usb could not be loaded; no USB radio will be "
                       "offered (the apk's Java half is missing or was minified away)");
        if (detach) { gVm->DetachCurrentThread(); }
        return;
    }

    const jint bound = env->RegisterNatives(
        usbCls, kMethods, static_cast<jint>(sizeof(kMethods) / sizeof(kMethods[0])));
    if (bound != JNI_OK || threw(env, "RegisterNatives(Usb)")) {
        core::diagLogf("usb: RegisterNatives failed (%d); no USB radio will be offered", bound);
        env->DeleteLocalRef(usbCls);
        if (detach) { gVm->DetachCurrentThread(); }
        return;
    }
    core::diagLogf("usb: %zu native method(s) bound to com.foxsdr.app.Usb",
                   sizeof(kMethods) / sizeof(kMethods[0]));

    // AND ONLY NOW DOES JAVA START. See the header's ordering note: until
    // RegisterNatives has run, every one of Java's calls would be an
    // UnsatisfiedLinkError.
    jmethodID onReady =
        env->GetStaticMethodID(usbCls, "onNativeReady", "(Landroid/app/Activity;)V");
    if (onReady == nullptr || threw(env, "Usb.onNativeReady id")) {
        env->DeleteLocalRef(usbCls);
        if (detach) { gVm->DetachCurrentThread(); }
        return;
    }
    env->CallStaticVoidMethod(usbCls, onReady, gActivity);
    threw(env, "Usb.onNativeReady()");
    env->DeleteLocalRef(usbCls);
    if (detach) { gVm->DetachCurrentThread(); }
}

}  // namespace cascade::usb

#else  // !__ANDROID__

namespace cascade::usb {

// NOT AN ERROR AND NOT LOGGED, the same shape as
// core/android_app_info.cpp's non-Android branch: a desktop build has no VM,
// no UsbManager and no Java half, and its own sysfs/SetupAPI enumeration
// needs none of them. The symbol exists so the header can be included
// anywhere without a platform guard around the include itself.
void androidUsbInit(void* javaVm, void* activityObject) {
    (void)javaVm;
    (void)activityObject;
}

}  // namespace cascade::usb

#endif  // __ANDROID__
