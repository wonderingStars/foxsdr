// Implementation of core/android_app_info.hpp. See that header for why a JNI
// call is the only honest way to answer the one question in it.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/android_app_info.hpp"

#include "core/diag_log.hpp"

#if defined(__ANDROID__)
#include <android/log.h>
#include <jni.h>
#endif

namespace cascade::core {

#if defined(__ANDROID__)

namespace {

// THE SAME TWO-STEP core/net_post.cpp's transport uses, and for the same
// reason: the thread android_main runs on is created by the native glue, so
// whether it is already attached to the VM is not something this code may
// assume. `detach` says whether the caller must detach - detaching a thread
// somebody else attached would pull the env out from under them.
JNIEnv* attachTo(JavaVM* vm, bool& detach) {
    detach = false;
    if (vm == nullptr) { return nullptr; }
    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK && env != nullptr) {
        return env;
    }
    if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK) { return nullptr; }
    detach = true;
    return env;
}

// Clears a pending Java exception and says whether there was one. EVERY JNI
// call below is followed by this: a pending exception makes the NEXT JNI call
// undefined, which is how a missing field turns into a process abort instead
// of an empty answer.
bool threw(JNIEnv* env, const char* what) {
    if (env->ExceptionCheck() == JNI_FALSE) { return false; }
    env->ExceptionDescribe();
    env->ExceptionClear();
    diagLogf("android: JNI %s threw; the native library directory is not known", what);
    return true;
}

}  // namespace

std::string androidNativeLibraryDir(void* javaVm, void* activityObject) {
    JavaVM* vm = static_cast<JavaVM*>(javaVm);
    jobject activity = static_cast<jobject>(activityObject);
    if (vm == nullptr || activity == nullptr) { return {}; }

    bool detach = false;
    JNIEnv* env = attachTo(vm, detach);
    if (env == nullptr) {
        diagLogf("android: no JNI environment; the native library directory is not known");
        return {};
    }

    std::string out;
    // getApplicationInfo() is declared on android.content.Context and the
    // activity IS a Context; a method id resolved on the base class dispatches
    // virtually, which is ordinary JNI and is exactly how net_post.cpp reads
    // the debuggable flag off the same object.
    jclass ctxCls = env->FindClass("android/content/Context");
    if (ctxCls != nullptr && !threw(env, "FindClass(Context)")) {
        jmethodID getInfo = env->GetMethodID(ctxCls, "getApplicationInfo",
                                             "()Landroid/content/pm/ApplicationInfo;");
        if (getInfo != nullptr && !threw(env, "getApplicationInfo id")) {
            jobject info = env->CallObjectMethod(activity, getInfo);
            if (info != nullptr && !threw(env, "getApplicationInfo()")) {
                jclass infoCls = env->FindClass("android/content/pm/ApplicationInfo");
                if (infoCls != nullptr && !threw(env, "FindClass(ApplicationInfo)")) {
                    jfieldID dirId =
                        env->GetFieldID(infoCls, "nativeLibraryDir", "Ljava/lang/String;");
                    if (dirId != nullptr && !threw(env, "nativeLibraryDir id")) {
                        jstring dir =
                            static_cast<jstring>(env->GetObjectField(info, dirId));
                        if (dir != nullptr && !threw(env, "nativeLibraryDir")) {
                            const char* utf = env->GetStringUTFChars(dir, nullptr);
                            if (utf != nullptr) {
                                out.assign(utf);
                                env->ReleaseStringUTFChars(dir, utf);
                            }
                            env->DeleteLocalRef(dir);
                        }
                    }
                    env->DeleteLocalRef(infoCls);
                }
                env->DeleteLocalRef(info);
            }
        }
        env->DeleteLocalRef(ctxCls);
    }

    if (detach) { vm->DetachCurrentThread(); }
    return out;
}

#else

std::string androidNativeLibraryDir(void* javaVm, void* activityObject) {
    // NOT AN ERROR AND NOT LOGGED. A desktop build has no VM and no package;
    // the empty answer is the definition of the question on this platform, and
    // it exists so that the rule which consumes it can be tested here.
    (void)javaVm;
    (void)activityObject;
    return {};
}

#endif

}  // namespace cascade::core
