// device_location_platform.cpp - the platform half of core/device_location.hpp.
//
// ONE FILE, TWO PLATFORMS, and the split is an #if rather than two files
// because the desktop half is four lines: there is no location service to ask,
// and saying so honestly is the whole of it. The same shape as
// core/android_app_info.cpp, which answers one Java-only question the same way.
//
// WHAT JAVA IS FOR HERE. Android's location APIs are Java-only. The NDK has no
// LocationManager, no FusedLocationProvider and no way to ask for a runtime
// permission - every one of those is an object with a Looper behind it. So the
// sequence lives in android/app/src/main/java/com/foxsdr/app/Loc.java and this
// file is the translator, exactly as src/usb/usb_android_jni.cpp is for
// UsbManager. Two calls out (start, stop), five calls in (a position, a
// refusal, a denial, a satellite count, a log line), and no policy on either
// side of the boundary that is not in device_location.cpp.
//
// THE BINDING IS RegisterNatives, NOT name resolution, for the reason
// usb_android_jni.hpp gives at length: this library is loaded by
// NativeActivity, JNI_OnLoad is never called on it, and FindClass on a native
// thread searches the SYSTEM class loader, which cannot see this application's
// own classes. The class is therefore resolved through the activity's own
// loader and the methods bound explicitly.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/device_location.hpp"

#include "core/diag_log.hpp"

#if defined(__ANDROID__)

#include <jni.h>

#include <mutex>
#include <string>

namespace cascade::core {

namespace {

std::mutex gMutex;
JavaVM* gVm = nullptr;
jobject gActivity = nullptr;   // global ref, see androidLocationInit
jclass gLocClass = nullptr;    // global ref to com.foxsdr.app.Loc
jmethodID gStartId = nullptr;  // static void start(Activity, long timeoutMs)
jmethodID gStopId = nullptr;   // static void stop()

// THE ONE INSTANCE. There is exactly one DeviceLocation in the application
// (AppWindow owns it) and Java has no handle to pass back, so the target is
// remembered when its platform half is made. Cleared by the destructor's
// stop() path only in the sense that a dead target stops being asked - which
// is why every callback below checks it under the lock before using it.
DeviceLocation* gTarget = nullptr;

// The two-step attach android_app_info.cpp documents: the thread android_main
// runs on is created by the native glue, so whether it is already attached is
// not something this code may assume. `detach` says whether the caller must
// detach - detaching a thread somebody else attached would pull the env out
// from under them.
JNIEnv* attachTo(bool& detach) {
    detach = false;
    if (gVm == nullptr) { return nullptr; }
    JNIEnv* env = nullptr;
    if (gVm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK && env != nullptr) {
        return env;
    }
    if (gVm->AttachCurrentThread(&env, nullptr) != JNI_OK) { return nullptr; }
    detach = true;
    return env;
}

// Clears a pending Java exception and says whether there was one. EVERY JNI
// call is followed by this: a pending exception makes the next JNI call
// undefined, which is how a missing method turns into a process abort instead
// of an empty answer.
bool threw(JNIEnv* env, const char* what) {
    if (env->ExceptionCheck() == JNI_FALSE) { return false; }
    env->ExceptionDescribe();
    env->ExceptionClear();
    diagLogf("location: JNI %s threw", what);
    return true;
}

std::string toStdString(JNIEnv* env, jstring s) {
    if (s == nullptr) { return std::string(); }
    const char* chars = env->GetStringUTFChars(s, nullptr);
    if (chars == nullptr) { return std::string(); }
    std::string out(chars);
    env->ReleaseStringUTFChars(s, chars);
    return out;
}

DeviceLocation* target() {
    const std::lock_guard<std::mutex> lock(gMutex);
    return gTarget;
}

// --- what Java calls -------------------------------------------------------

void jniLog(JNIEnv* env, jclass, jstring message) {
    // The Java half's account of a permission prompt, a provider being off
    // and a listener being removed lands in the SAME log as the native
    // half's - one story, in order. Java is held to the same privacy rule:
    // it never passes a coordinate to this.
    const std::string text = toStdString(env, message);
    if (!text.empty()) { diagLogf("location: %s", text.c_str()); }
}

void jniFix(JNIEnv*, jclass, jdouble lat, jdouble lon, jfloat accuracyM, jint providerCode) {
    DeviceLocation* t = target();
    if (t == nullptr) { return; }
    DeviceLocation::Provider provider = DeviceLocation::Provider::Unknown;
    if (providerCode == 1) { provider = DeviceLocation::Provider::Satellites; }
    if (providerCode == 2) { provider = DeviceLocation::Provider::Network; }
    t->onFix(static_cast<double>(lat), static_cast<double>(lon),
             static_cast<double>(accuracyM), provider);
}

void jniFailed(JNIEnv* env, jclass, jstring reason) {
    DeviceLocation* t = target();
    if (t == nullptr) { return; }
    t->onFailed(toStdString(env, reason));
}

void jniDenied(JNIEnv* env, jclass, jstring reason) {
    DeviceLocation* t = target();
    if (t == nullptr) { return; }
    t->onDenied(toStdString(env, reason));
}

void jniSatellites(JNIEnv*, jclass, jint used) {
    DeviceLocation* t = target();
    if (t == nullptr) { return; }
    t->onSatellites(static_cast<int>(used));
}

const JNINativeMethod kMethods[] = {
    {"nativeLog", "(Ljava/lang/String;)V", reinterpret_cast<void*>(&jniLog)},
    {"nativeFix", "(DDFI)V", reinterpret_cast<void*>(&jniFix)},
    {"nativeFailed", "(Ljava/lang/String;)V", reinterpret_cast<void*>(&jniFailed)},
    {"nativeDenied", "(Ljava/lang/String;)V", reinterpret_cast<void*>(&jniDenied)},
    {"nativeSatellites", "(I)V", reinterpret_cast<void*>(&jniSatellites)},
};

// Resolves com.foxsdr.app.Loc THROUGH THE APPLICATION CLASS LOADER - see the
// file header. Returns a local ref.
jclass resolveLocClass(JNIEnv* env) {
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

    jstring name = env->NewStringUTF("com.foxsdr.app.Loc");
    if (name == nullptr) { return nullptr; }
    jobject cls = env->CallObjectMethod(loader, loadClass, name);
    env->DeleteLocalRef(name);
    if (cls == nullptr || threw(env, "loadClass(com.foxsdr.app.Loc)")) { return nullptr; }
    return static_cast<jclass>(cls);
}

bool callJavaStart(double timeoutS, std::string& error) {
    bool detach = false;
    jclass cls = nullptr;
    jmethodID startId = nullptr;
    jobject activity = nullptr;
    {
        const std::lock_guard<std::mutex> lock(gMutex);
        cls = gLocClass;
        startId = gStartId;
        activity = gActivity;
    }
    if (cls == nullptr || startId == nullptr || activity == nullptr) {
        error = "the device's location service could not be reached";
        return false;
    }
    JNIEnv* env = attachTo(detach);
    if (env == nullptr) {
        error = "the device's location service could not be reached";
        return false;
    }
    const jlong ms = static_cast<jlong>(timeoutS * 1000.0);
    env->CallStaticVoidMethod(cls, startId, activity, ms);
    const bool bad = threw(env, "Loc.start()");
    if (detach) { gVm->DetachCurrentThread(); }
    if (bad) {
        error = "the device's location service refused the request";
        return false;
    }
    return true;
}

void callJavaStop() {
    bool detach = false;
    jclass cls = nullptr;
    jmethodID stopId = nullptr;
    {
        const std::lock_guard<std::mutex> lock(gMutex);
        cls = gLocClass;
        stopId = gStopId;
    }
    if (cls == nullptr || stopId == nullptr) { return; }
    JNIEnv* env = attachTo(detach);
    if (env == nullptr) { return; }
    env->CallStaticVoidMethod(cls, stopId);
    threw(env, "Loc.stop()");
    if (detach) { gVm->DetachCurrentThread(); }
}

}  // namespace

DeviceLocation::Platform makePlatformLocation(DeviceLocation& target) {
    {
        const std::lock_guard<std::mutex> lock(gMutex);
        gTarget = &target;
    }
    DeviceLocation::Platform p;
    p.start = [](double timeoutS, std::string& error) { return callJavaStart(timeoutS, error); };
    p.stop = []() { callJavaStop(); };
    return p;
}

void androidLocationInit(void* javaVm, void* activityObject) {
    if (javaVm == nullptr || activityObject == nullptr) {
        diagLogf("location: no JavaVM or activity; the device's position cannot be read");
        return;
    }
    {
        const std::lock_guard<std::mutex> lock(gMutex);
        gVm = static_cast<JavaVM*>(javaVm);
    }
    bool detach = false;
    JNIEnv* env = attachTo(detach);
    if (env == nullptr) {
        diagLogf("location: could not attach to the JavaVM; the Java half is not armed");
        return;
    }
    {
        const std::lock_guard<std::mutex> lock(gMutex);
        // A global ref: the activity handed to android_main is a local ref
        // belonging to a frame that is long gone by the time the user presses
        // the key, and the class-loader lookup needs it for the life of the
        // process.
        if (gActivity == nullptr) {
            gActivity = env->NewGlobalRef(static_cast<jobject>(activityObject));
        }
    }

    jclass local = resolveLocClass(env);
    if (local == nullptr) {
        diagLogf("location: com.foxsdr.app.Loc could not be loaded; the device's position "
                 "cannot be read");
        if (detach) { gVm->DetachCurrentThread(); }
        return;
    }

    const jint bound =
        env->RegisterNatives(local, kMethods, static_cast<jint>(sizeof(kMethods) / sizeof(kMethods[0])));
    if (bound != JNI_OK || threw(env, "RegisterNatives(Loc)")) {
        diagLogf("location: RegisterNatives failed (%d); the device's position cannot be read",
                 bound);
        env->DeleteLocalRef(local);
        if (detach) { gVm->DetachCurrentThread(); }
        return;
    }

    jmethodID startId = env->GetStaticMethodID(local, "start", "(Landroid/app/Activity;J)V");
    const bool startBad = (startId == nullptr) || threw(env, "Loc.start id");
    jmethodID stopId = env->GetStaticMethodID(local, "stop", "()V");
    const bool stopBad = (stopId == nullptr) || threw(env, "Loc.stop id");
    if (startBad || stopBad) {
        diagLogf("location: the Java half is missing start/stop; the device's position cannot "
                 "be read");
        env->DeleteLocalRef(local);
        if (detach) { gVm->DetachCurrentThread(); }
        return;
    }

    {
        const std::lock_guard<std::mutex> lock(gMutex);
        gLocClass = static_cast<jclass>(env->NewGlobalRef(local));
        gStartId = startId;
        gStopId = stopId;
    }
    env->DeleteLocalRef(local);
    if (detach) { gVm->DetachCurrentThread(); }
    diagLogf("location: %zu native method(s) bound to com.foxsdr.app.Loc",
             sizeof(kMethods) / sizeof(kMethods[0]));
}

}  // namespace cascade::core

#else  // not Android

namespace cascade::core {

// NO LOCATION SERVICE, AND SAYING SO IS THE WHOLE IMPLEMENTATION. An empty
// Platform is what DeviceLocation::start() reads as Unavailable, and the GUI
// never draws the control on a platform where platformHasDeviceLocation() is
// false - so this exists to make the class constructible and testable on the
// machine the tests run on, not to be used.
DeviceLocation::Platform makePlatformLocation(DeviceLocation&) {
    return DeviceLocation::Platform();
}

void androidLocationInit(void*, void*) {}

}  // namespace cascade::core

#endif
