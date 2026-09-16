// net_post.cpp - see net_post.hpp for why this file exists at all.
//
// TWO HALVES. Everything above the "The Android transport" banner is compiled
// on EVERY platform and has no platform code in it: the request shapes, the
// URL split, the loopback rule, the scheme gate and the test hook. That is
// deliberate - it is the half tests/test_net_post.cpp can reach on a Linux
// host, and it is the half that decides what leaves the machine.
//
// Below the banner is the Java transport, and only its BODY is behind
// __ANDROID__ (which the NDK toolchain defines and a host compiler does not) -
// the same shape sink/audio_out_aaudio.cpp uses to stay host-compilable. The
// entry point androidNetPost() exists everywhere, applies the same gate and
// honours the same test hook, so a host test can drive the Android code path
// itself rather than a lookalike.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/net_post.hpp"

#include "core/diag_log.hpp"

#include <cctype>
#include <cstdlib>
#include <mutex>

#if defined(__ANDROID__)
#include <atomic>

#include <jni.h>
#endif

namespace cascade::core {

// ---------------------------------------------------------------------------
// The two requests this program makes
// ---------------------------------------------------------------------------

NetPost telemetryPost(const std::string& url, const std::string& json) {
    NetPost p;
    p.url = url;
    p.body = json;
    p.contentType = "application/json";
    // 4 s / 6 s / 6 s - the numbers the WinHTTP branch has used since usage
    // reporting was written, and the ones the cpp-httplib branch was given to
    // match it. Short on purpose: this runs on a thread the destructor joins.
    p.connectTimeoutMs = 4000;
    p.readTimeoutMs = 6000;
    p.writeTimeoutMs = 6000;
#if defined(CASCADE_ANDROID)
    // See the comment on telemetryPost() in net_post.hpp. Narrow, Android
    // only, and unreachable without an explicit FOXSDR_TELEMETRY_URL naming a
    // loopback address.
    p.allowPlainLoopback = true;
#else
    p.allowPlainLoopback = false;
#endif
    p.userAgent = "FoxSDR-usage/1.0";
    return p;
}

NetPost crashPost(const std::string& url, const std::string& json) {
    NetPost p;
    p.url = url;
    p.body = json;
    p.contentType = "application/json";
    // 3 s / 5 s / 5 s, matching the WinHTTP and cpp-httplib uploaders. These
    // bound the worst case even when cancellation is never asked for.
    p.connectTimeoutMs = 3000;
    p.readTimeoutMs = 5000;
    p.writeTimeoutMs = 5000;
    p.allowPlainLoopback = true;
    p.userAgent = "FoxSDR-crash/1.0";
    return p;
}

// ---------------------------------------------------------------------------
// The gate
// ---------------------------------------------------------------------------

bool netPostLoopbackHost(const std::string& host) {
    return host == "127.0.0.1" || host == "localhost" || host == "::1";
}

bool netPostSplitUrl(const std::string& url, NetPostUrl& out) {
    out = NetPostUrl();
    const std::size_t schemeEnd = url.find("://");
    if (schemeEnd == std::string::npos) { return false; }
    out.scheme = url.substr(0, schemeEnd);
    if (out.scheme.empty()) { return false; }
    out.port = (out.scheme == "https") ? 443 : 80;
    const std::string rest = url.substr(schemeEnd + 3);
    const std::size_t slash = rest.find('/');
    out.authority = (slash == std::string::npos) ? rest : rest.substr(0, slash);
    out.target = (slash == std::string::npos) ? std::string("/") : rest.substr(slash);
    if (out.authority.empty()) { return false; }
    const std::size_t colon = out.authority.find(':');
    if (colon == std::string::npos) {
        out.host = out.authority;
    } else {
        out.host = out.authority.substr(0, colon);
        const std::string portText = out.authority.substr(colon + 1);
        if (portText.empty() || portText.find_first_not_of("0123456789") != std::string::npos) {
            return false;
        }
        const long p = std::strtol(portText.c_str(), nullptr, 10);
        if (p <= 0 || p > 65535) { return false; }
        out.port = static_cast<int>(p);
    }
    return !out.host.empty();
}

bool netPostAllowed(const NetPost& p) {
    if (p.url.empty() || p.body.empty()) { return false; }
    NetPostUrl u;
    if (!netPostSplitUrl(p.url, u)) { return false; }
    if (u.scheme == "https") { return true; }
    // Anything that is not https is refused outright unless it is plain http
    // to a loopback address AND the caller asked for that exception. A usage
    // report is not secret, but sending one in clear would put an install id
    // on the wire for any network in between to collect.
    if (!p.allowPlainLoopback) { return false; }
    return u.scheme == "http" && netPostLoopbackHost(u.host);
}

// ---------------------------------------------------------------------------
// The test seam
// ---------------------------------------------------------------------------

namespace {

std::mutex& hookMutex() {
    static std::mutex m;
    return m;
}

NetPostHook& hookSlot() {
    static NetPostHook h;
    return h;
}

}  // namespace

void setNetPostHookForTest(NetPostHook hook) {
    const std::lock_guard<std::mutex> lock(hookMutex());
    hookSlot() = std::move(hook);
}

NetPostResult runNetPostHook(const NetPost& p, bool& handled) {
    NetPostHook copy;
    {
        // Copied under the lock and called outside it: a transport runs on a
        // worker thread, and a hook that itself posted would otherwise
        // deadlock against a concurrent setNetPostHookForTest.
        const std::lock_guard<std::mutex> lock(hookMutex());
        copy = hookSlot();
    }
    handled = static_cast<bool>(copy);
    if (!handled) { return NetPostResult(); }
    return copy(p);
}

// ---------------------------------------------------------------------------
// The Android transport
// ---------------------------------------------------------------------------

namespace {

// The cancellation key. An int rather than a pointer, because what it names
// lives on the Java side (a HttpURLConnection in Net.java's in-flight map) and
// there is no C++ object to point at. Shared rather than Android-only so the
// skeleton below - publish, send, take - is the same code a host test drives
// and a device runs; only the "send" step differs.
int nextCallToken() {
    static std::atomic<int> next{1};
    int t = next.fetch_add(1);
    if (t <= 0) {  // wrapped: still a usable, non-zero key
        next.store(2);
        t = 1;
    }
    return t;
}

}  // namespace

#if defined(__ANDROID__)
namespace {

JavaVM* gVm = nullptr;
jobject gActivity = nullptr;       // global ref
jclass gNetClass = nullptr;        // global ref, resolved lazily
jmethodID gPostMethod = nullptr;
jmethodID gCancelMethod = nullptr;
std::mutex gJniMutex;

// Attaches this thread to the VM if it is not already attached. `detach` says
// whether the caller must detach when it is done: detaching a thread somebody
// else attached would pull the env out from under them.
JNIEnv* attach(bool& detach) {
    detach = false;
    if (gVm == nullptr) { return nullptr; }
    JNIEnv* env = nullptr;
    const jint got = gVm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (got == JNI_OK && env != nullptr) { return env; }
    if (gVm->AttachCurrentThread(&env, nullptr) != JNI_OK) { return nullptr; }
    detach = true;
    return env;
}

// Clears a pending Java exception and says whether there was one. Every JNI
// call below is followed by this: a pending exception makes the NEXT JNI call
// undefined, which is how a missing method turns into a process abort instead
// of a failed upload.
bool failed(JNIEnv* env, const char* what) {
    if (env->ExceptionCheck() == JNI_FALSE) { return false; }
    env->ExceptionDescribe();
    env->ExceptionClear();
    diagWarnf("net: JNI %s threw; nothing sent", what);
    return true;
}

// Resolves com.foxsdr.app.Net THROUGH THE APPLICATION CLASS LOADER. See
// androidNetInit's comment in the header: FindClass() on a worker thread uses
// the system loader, which cannot see this application's own classes.
// Caller holds gJniMutex.
bool resolveNetClass(JNIEnv* env) {
    if (gNetClass != nullptr) { return true; }
    if (gActivity == nullptr) { return false; }

    jclass activityCls = env->GetObjectClass(gActivity);
    if (activityCls == nullptr || failed(env, "GetObjectClass(activity)")) { return false; }
    jmethodID getLoader =
        env->GetMethodID(activityCls, "getClassLoader", "()Ljava/lang/ClassLoader;");
    if (getLoader == nullptr || failed(env, "getClassLoader id")) { return false; }
    jobject loader = env->CallObjectMethod(gActivity, getLoader);
    if (loader == nullptr || failed(env, "getClassLoader()")) { return false; }

    jclass loaderCls = env->FindClass("java/lang/ClassLoader");
    if (loaderCls == nullptr || failed(env, "FindClass(ClassLoader)")) { return false; }
    jmethodID loadClass =
        env->GetMethodID(loaderCls, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
    if (loadClass == nullptr || failed(env, "loadClass id")) { return false; }

    jstring name = env->NewStringUTF("com.foxsdr.app.Net");
    if (name == nullptr) { return false; }
    jobject cls = env->CallObjectMethod(loader, loadClass, name);
    env->DeleteLocalRef(name);
    if (cls == nullptr || failed(env, "loadClass(com.foxsdr.app.Net)")) { return false; }

    jmethodID post = env->GetStaticMethodID(
        static_cast<jclass>(cls), "post",
        "(Ljava/lang/String;[BLjava/lang/String;Ljava/lang/String;III)[I");
    if (post == nullptr || failed(env, "Net.post id")) { return false; }
    jmethodID cancel = env->GetStaticMethodID(static_cast<jclass>(cls), "cancel", "(I)V");
    if (cancel == nullptr || failed(env, "Net.cancel id")) { return false; }

    gNetClass = static_cast<jclass>(env->NewGlobalRef(cls));
    gPostMethod = post;
    gCancelMethod = cancel;
    return gNetClass != nullptr;
}

}  // namespace

void androidNetInit(void* javaVm, void* activityObject) {
    const std::lock_guard<std::mutex> lock(gJniMutex);
    gVm = static_cast<JavaVM*>(javaVm);
    if (gVm == nullptr || activityObject == nullptr) { return; }
    bool detach = false;
    JNIEnv* env = attach(detach);
    if (env == nullptr) {
        gVm = nullptr;
        return;
    }
    // A global ref, because the activity object handed to android_main is a
    // local ref belonging to a frame that is long gone by the time a usage
    // report is sent.
    gActivity = env->NewGlobalRef(static_cast<jobject>(activityObject));
    if (detach) { gVm->DetachCurrentThread(); }
    diagLogf("net: Java transport armed (HttpsURLConnection over JNI)");
}

bool androidNetReady() {
    const std::lock_guard<std::mutex> lock(gJniMutex);
    return gVm != nullptr && gActivity != nullptr;
}

namespace {

// The JNI half only. The gate, the hook and the cancellation skeleton around
// it live in androidNetPost() below, which is compiled on every platform.
NetPostResult javaPost(const NetPost& p, int token) {
    NetPostResult res;
    JNIEnv* env = nullptr;
    bool detach = false;
    {
        const std::lock_guard<std::mutex> lock(gJniMutex);
        if (gVm == nullptr) {
            diagWarnf("net: no Java VM registered; nothing sent");
            return res;
        }
        env = attach(detach);
        if (env == nullptr) { return res; }
        if (!resolveNetClass(env)) {
            if (detach) { gVm->DetachCurrentThread(); }
            return res;
        }
    }

    jstring url = env->NewStringUTF(p.url.c_str());
    jstring type = env->NewStringUTF(p.contentType.c_str());
    jstring agent = env->NewStringUTF(p.userAgent.c_str());
    jbyteArray body = env->NewByteArray(static_cast<jsize>(p.body.size()));
    if (url != nullptr && type != nullptr && agent != nullptr && body != nullptr) {
        env->SetByteArrayRegion(body, 0, static_cast<jsize>(p.body.size()),
                                reinterpret_cast<const jbyte*>(p.body.data()));
        if (!failed(env, "SetByteArrayRegion")) {
            res.attempted = true;
            jobject out = env->CallStaticObjectMethod(gNetClass, gPostMethod, url, body, type,
                                                      agent, p.connectTimeoutMs, p.readTimeoutMs,
                                                      token);
            if (failed(env, "Net.post")) {
                out = nullptr;
            }
            if (out == nullptr) {
                // Java refused the request before opening it (a URL it could
                // not parse, a scheme it would not take). Not an attempt.
                res.attempted = false;
            } else {
                jint vals[2] = {0, 0};
                env->GetIntArrayRegion(static_cast<jintArray>(out), 0, 2, vals);
                if (!failed(env, "GetIntArrayRegion")) {
                    res.status = static_cast<int>(vals[0]);
                    res.retryAfterSeconds =
                        vals[1] > 0 ? static_cast<std::uint64_t>(vals[1]) : 0;
                }
                env->DeleteLocalRef(out);
            }
        }
    }
    if (url != nullptr) { env->DeleteLocalRef(url); }
    if (type != nullptr) { env->DeleteLocalRef(type); }
    if (agent != nullptr) { env->DeleteLocalRef(agent); }
    if (body != nullptr) { env->DeleteLocalRef(body); }

    if (detach) { gVm->DetachCurrentThread(); }
    return res;
}

}  // namespace

void androidNetCancel(void* tokenHandle) {
    if (tokenHandle == nullptr) { return; }
    const int token = static_cast<int>(reinterpret_cast<intptr_t>(tokenHandle));
    const std::lock_guard<std::mutex> lock(gJniMutex);
    if (gVm == nullptr || gNetClass == nullptr || gCancelMethod == nullptr) { return; }
    bool detach = false;
    JNIEnv* env = attach(detach);
    if (env == nullptr) { return; }
    env->CallStaticVoidMethod(gNetClass, gCancelMethod, token);
    (void)failed(env, "Net.cancel");
    if (detach) { gVm->DetachCurrentThread(); }
}

#else  // __ANDROID__

namespace {

// The host build has no Java to reach. It still compiles the WHOLE skeleton
// below, which is the point of the seam: a test on this machine drives the
// Android entry point itself - gate, hook and cancellation - rather than a
// lookalike, and only this one step is missing.
NetPostResult javaPost(const NetPost& p, int token) {
    (void)p;
    (void)token;
    diagWarnf("net: the Java transport is Android-only; nothing sent");
    return NetPostResult();
}

}  // namespace

void androidNetInit(void* javaVm, void* activityObject) {
    (void)javaVm;
    (void)activityObject;
}

bool androidNetReady() { return false; }

void androidNetCancel(void* tokenHandle) { (void)tokenHandle; }

#endif  // __ANDROID__

// ONE SKELETON FOR BOTH BUILDS. The gate, the fake-transport hook and the
// publish/send/take cancellation dance are compiled identically on a phone and
// on a test machine; javaPost() above is the only step that differs.
NetPostResult androidNetPost(const NetPost& p, const NetPostCancel* cancel) {
    NetPostResult res;
    // THE GATE FIRST, before the hook: a request the rules refuse must never
    // be visible to a transport, real or fake.
    if (!netPostAllowed(p)) { return res; }

    const int token = nextCallToken();
    if (cancel != nullptr && cancel->publish) {
        // A cancel that already landed means nothing is sent at all - not an
        // attempt, not a failure, cancelled.
        if (!cancel->publish(reinterpret_cast<void*>(static_cast<std::intptr_t>(token)))) {
            res.cancelled = true;
            return res;
        }
    }

    bool handled = false;
    const NetPostResult hooked = runNetPostHook(p, handled);
    res = handled ? hooked : javaPost(p, token);

    if (cancel != nullptr) {
        // Taken before anything reads the result, so a later cancel() finds
        // nullptr and touches a token this call has finished with - the same
        // ordering the cpp-httplib uploader uses around its ClientImpl.
        if (cancel->take) { cancel->take(); }
        if (!res.cancelled && res.status == 0 && cancel->cancelled && cancel->cancelled()) {
            res.cancelled = true;
        }
    }
    return res;
}

}  // namespace cascade::core
