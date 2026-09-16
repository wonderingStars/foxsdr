// net_post.hpp - the one description of an outbound POST this application
// makes, and the seam every platform's transport sits behind.
//
// WHY THIS EXISTS. Two features send something off this machine: the usage
// report (core/telemetry.cpp) and the crash report (core/crash_upload.cpp).
// Until the Android port there were two transports for them - WinHTTP on
// Windows, cpp-httplib+OpenSSL everywhere else - and each sender built its
// request inline, twice, inside its own #if. Android needs a THIRD transport,
// because the NDK ships no OpenSSL and therefore cpp-httplib has no TLS here
// (see net/web_server.cpp for the rule that every translation unit in this
// program must agree about CPPHTTPLIB_OPENSSL_SUPPORT); the only HTTPS stack
// on the platform is Java's HttpsURLConnection, reached over JNI.
//
// A third inline copy of "what to send" is how the Android build would quietly
// start sending something the other two do not. So the REQUEST is built here,
// by code compiled identically on every platform, and only the function that
// CONSUMES it differs. tests/test_net_post.cpp pins the built request field by
// field and injects a fake transport in the place the real client sits, which
// is what makes "Android sends the same bytes to the same endpoint" a test
// result rather than a claim.
//
// WHAT IS DELIBERATELY NOT HERE: the payload itself. TelemetryReport::toJson()
// and the crash report's own serialiser are unchanged and untouched by this
// file - it carries their output as an opaque string. PRIVACY.md's field
// inventory and tests/test_telemetry.cpp's assertion of it therefore still
// describe every platform, Android included.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_NET_POST_HPP
#define CASCADE_CORE_NET_POST_HPP

#include <cstdint>
#include <functional>
#include <string>

namespace cascade::core {

// One outbound POST, described completely. Everything a transport needs and
// nothing it does not: there is no header map, because the only headers this
// program has ever sent are a content type and a user agent, and a general
// header bag would be a place for a later change to add something the privacy
// notice does not mention.
struct NetPost {
    std::string url;
    std::string body;
    std::string contentType;
    // Milliseconds. The desktop clients take seconds; these are multiplied out
    // at the call site so one set of numbers describes every platform.
    int connectTimeoutMs = 0;
    int readTimeoutMs = 0;
    int writeTimeoutMs = 0;
    // Plain http to 127.0.0.1/localhost/::1 only. FALSE means https or
    // nothing. See netPostAllowed() for what this actually permits and why the
    // two senders answer it differently.
    bool allowPlainLoopback = false;
    // Sent by the WinHTTP and the Android transports; the cpp-httplib one has
    // never set it and still does not, so this is NOT part of the contract the
    // two must share - it is recorded here so the Android build sends the same
    // name Windows already does rather than inventing a third one.
    std::string userAgent;
};

struct NetPostResult {
    // The transport got as far as opening a request. Distinguishes "refused
    // before any I/O" from "tried and failed", which is what the crash
    // uploader's retry bookkeeping counts.
    bool attempted = false;
    bool cancelled = false;
    int status = 0;
    // Retry-After, delta-seconds form only - an HTTP-date would need a parser
    // and a clock comparison for a header the endpoint controls, and an
    // unparsed value falling back to the default backoff is the safe
    // direction. Matches both desktop transports exactly.
    std::uint64_t retryAfterSeconds = 0;
};

// The cancellation seam, as three closures rather than a type, so this header
// does not have to know about core/crash_upload.hpp's UploadCancel (which is
// what supplies them) and the usage sender - which has no cancellation at all
// and never did - can simply pass nullptr.
//
//   publish(handle)  false => a cancel already landed; send nothing.
//   take()           hands the handle back; nullptr if cancel() took it first.
//   cancelled()      true if a cancel landed while the request was in flight.
struct NetPostCancel {
    std::function<bool(void*)> publish;
    std::function<void*()> take;
    std::function<bool()> cancelled;
};

// --------------------------------------------------------------------------
// The two requests this program makes
// --------------------------------------------------------------------------

// The usage report and the heartbeat (they differ only in body). https only on
// the desktop, exactly as before this file existed.
//
// ON ANDROID ONLY, plain http to LOOPBACK is additionally permitted, and that
// is a deliberate, narrow difference rather than an oversight. The compiled-in
// endpoint is https and unchanged; the only way to reach the exception is an
// explicit FOXSDR_TELEMETRY_URL naming 127.0.0.1, which is how this transport
// is exercised on a device that trusts no local certificate authority. A
// loopback address never leaves the phone, so nothing about what this build
// can put on a network changes. The crash uploader has permitted exactly this,
// on every platform, since it was written.
NetPost telemetryPost(const std::string& url, const std::string& json);

// A captured crash or hang report. https, or plain http to loopback - the
// second half is what lets tests/test_crash_upload.cpp drive the real
// transport against a socket.
NetPost crashPost(const std::string& url, const std::string& json);

// --------------------------------------------------------------------------
// The gate every transport applies before any I/O
// --------------------------------------------------------------------------

struct NetPostUrl {
    std::string scheme;
    std::string host;       // no port; this is what the loopback check compares
    int port = 0;           // the scheme's default when the URL carries none
    std::string authority;  // host[:port] exactly as written, port and all
    std::string target;     // path (+query), never empty
};

bool netPostSplitUrl(const std::string& url, NetPostUrl& out);
bool netPostLoopbackHost(const std::string& host);

// TRUE if this request may be sent at all. A shipped binary can never be
// talked into putting an install id in clear on somebody's network: https
// always, plain http only to loopback and only when the request asked for it.
// Every transport calls this, so there is one rule rather than three.
bool netPostAllowed(const NetPost& p);

// --------------------------------------------------------------------------
// The test seam
// --------------------------------------------------------------------------

// Installed by tests only. When set, EVERY transport - WinHTTP, cpp-httplib
// and the Android one - hands the request here instead of opening a socket,
// which is what lets a host test observe the exact bytes and headers the
// Android build would send without an Android build. Pass an empty function to
// remove it. Never set in a shipped run: nothing but
// tests/test_net_post.cpp calls this.
using NetPostHook = std::function<NetPostResult(const NetPost&)>;
void setNetPostHookForTest(NetPostHook hook);

// Runs the installed hook, if any. `handled` says whether one was installed,
// so a null result and "no hook" are never confused.
NetPostResult runNetPostHook(const NetPost& p, bool& handled);

// --------------------------------------------------------------------------
// The Android transport
// --------------------------------------------------------------------------

// Handed the JavaVM and the activity object once, from android_main. Both are
// void* so this header stays free of <jni.h>, exactly as
// usb/usb_android_bridge.h keeps the USB boundary free of this program's C++
// types: one ABI seam at a time.
//
// THE ACTIVITY OBJECT IS NOT OPTIONAL, and the reason is subtle. A worker
// thread attached to the VM resolves classes through the SYSTEM class loader,
// which cannot see com.foxsdr.app.Net at all; the activity is what this code
// asks for the APPLICATION class loader (getClassLoader().loadClass(...)).
// FindClass() from a native thread would find nothing and there would be no
// error anywhere saying why.
void androidNetInit(void* javaVm, void* activityObject);

// True once androidNetInit has been given a VM. False on every other platform
// and in a build with no Java side, where androidNetPost refuses honestly.
bool androidNetReady();

// One POST through Java's HttpsURLConnection, on the calling thread (which is
// always a worker - see TelemetryReporter::send and the crash sweep). Attaches
// to the VM and detaches before returning.
NetPostResult androidNetPost(const NetPost& p, const NetPostCancel* cancel);

// Aborts an in-flight androidNetPost by the token publish() was handed.
// Best-effort: it disconnects the connection from another thread, which is
// what turns a blocked read into an immediate failure. The short timeouts in
// the NetPost are the guarantee; this is the courtesy.
void androidNetCancel(void* token);

}  // namespace cascade::core

#endif  // CASCADE_CORE_NET_POST_HPP
