// Tests for core/net_post.hpp - the seam every outbound POST goes through.
//
// WHAT THESE ARE FOR. The Android port cannot use the transport the other
// platforms use: the NDK ships no OpenSSL, so cpp-httplib has no TLS there and
// the only HTTPS stack on the device is Java's HttpsURLConnection, reached
// over JNI. That is a THIRD way out of the machine, and the risk it carries is
// not that it fails - a failure is visible - but that it quietly sends
// something DIFFERENT: a different body, a different content type, a different
// endpoint, or a request the https-only rule should have refused.
//
// So the tests below do not test a transport. They test what is HANDED to one.
// A fake is installed in the exact place the real client is built (see
// setNetPostHookForTest and the "THE FAKE TRANSPORT GOES HERE" comments in
// telemetry.cpp and crash_upload.cpp), the real senders are driven, and the
// request they produce is pinned field by field - then the Android entry point
// is driven with the same input and the two are compared for equality. That
// comparison is the whole point of the file: it is what makes "Android sends
// the same bytes to the same endpoint" a test result instead of a claim.
//
// NOTHING HERE OPENS A SOCKET, and nothing here can reach the real endpoints:
// every URL is either a fake hostname or is refused by the gate before any
// transport sees it.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/crash_upload.hpp"
#include "core/net_post.hpp"
#include "core/telemetry.hpp"
#include "test_check.hpp"

using namespace cascade::core;

namespace {

// What the fake transport saw, and what it answers with.
struct Recorder {
    std::vector<NetPost> seen;
    NetPostResult reply;

    void install() {
        seen.clear();
        setNetPostHookForTest([this](const NetPost& p) {
            this->seen.push_back(p);
            return this->reply;
        });
    }
    static void remove() { setNetPostHookForTest(NetPostHook()); }
};

bool same(const NetPost& a, const NetPost& b) {
    return a.url == b.url && a.body == b.body && a.contentType == b.contentType &&
           a.connectTimeoutMs == b.connectTimeoutMs && a.readTimeoutMs == b.readTimeoutMs &&
           a.writeTimeoutMs == b.writeTimeoutMs && a.allowPlainLoopback == b.allowPlainLoopback &&
           a.userAgent == b.userAgent;
}

const char* kUsageUrl = "https://telemetry.invalid/";
const char* kCrashUrl = "https://crash.invalid/api/crash";
const char* kUsageBody = "{\"id\":\"0123456789abcdef0123456789abcdef\",\"v\":\"0.97.0\"}";
const char* kCrashBody = "{\"signature\":\"AABBCCDD\",\"v\":\"0.97.0\"}";

// ---------------------------------------------------------------------------
// The request shapes
// ---------------------------------------------------------------------------

void testTheUsageRequestIsPinned() {
    const NetPost p = telemetryPost(kUsageUrl, kUsageBody);
    CHECK(p.url == kUsageUrl);
    // THE BODY IS CARRIED, NEVER REWRITTEN. TelemetryReport::toJson() is the
    // only thing that decides what is in a usage report, and PRIVACY.md's
    // field inventory (asserted by tests/test_telemetry.cpp) describes it on
    // every platform because of this line.
    CHECK(p.body == kUsageBody);
    CHECK(p.contentType == "application/json");
    // The numbers WinHTTP has used since usage reporting was written.
    CHECK(p.connectTimeoutMs == 4000);
    CHECK(p.readTimeoutMs == 6000);
    CHECK(p.writeTimeoutMs == 6000);
    CHECK(p.userAgent == "FoxSDR-usage/1.0");
#if defined(CASCADE_ANDROID)
    // The one deliberate platform difference, and it is narrower than the
    // crash uploader's long-standing rule: plain http to LOOPBACK only, which
    // never leaves the device, and only reachable through an explicit
    // FOXSDR_TELEMETRY_URL. See net_post.hpp.
    CHECK(p.allowPlainLoopback);
#else
    CHECK(!p.allowPlainLoopback);
#endif
}

void testTheCrashRequestIsPinned() {
    const NetPost p = crashPost(kCrashUrl, kCrashBody);
    CHECK(p.url == kCrashUrl);
    CHECK(p.body == kCrashBody);
    CHECK(p.contentType == "application/json");
    CHECK(p.connectTimeoutMs == 3000);
    CHECK(p.readTimeoutMs == 5000);
    CHECK(p.writeTimeoutMs == 5000);
    CHECK(p.userAgent == "FoxSDR-crash/1.0");
    // Always, on every platform: it is what lets a test drive the real
    // transport against a socket.
    CHECK(p.allowPlainLoopback);
}

// ---------------------------------------------------------------------------
// The gate
// ---------------------------------------------------------------------------

void testTheGateRefusesEverythingButHttpsAndLoopbackHttp() {
    // https, anywhere: allowed for both.
    CHECK(netPostAllowed(telemetryPost("https://telemetry.foxsdr.com/", "{}")));
    CHECK(netPostAllowed(crashPost("https://foxsdr.com/api/crash", "{}")));

    // THE ONE THAT MATTERS. Plain http to a real host would put an install id
    // in clear on whatever network is in between. Refused for BOTH senders on
    // every platform - the loopback exception is a loopback exception.
    CHECK(!netPostAllowed(telemetryPost("http://telemetry.foxsdr.com/", "{}")));
    CHECK(!netPostAllowed(crashPost("http://foxsdr.com/api/crash", "{}")));
    CHECK(!netPostAllowed(crashPost("http://10.0.2.2:8099/", "{}")));
    CHECK(!netPostAllowed(crashPost("http://192.168.1.10/", "{}")));
    // ...including a host whose NAME merely contains a loopback address.
    CHECK(!netPostAllowed(crashPost("http://127.0.0.1.evil.example/", "{}")));

    // Plain http to loopback: the crash uploader always, the usage sender on
    // Android only.
    CHECK(netPostAllowed(crashPost("http://127.0.0.1:8099/u", "{}")));
    CHECK(netPostAllowed(crashPost("http://localhost:8099/u", "{}")));
#if defined(CASCADE_ANDROID)
    CHECK(netPostAllowed(telemetryPost("http://127.0.0.1:8099/u", "{}")));
#else
    CHECK(!netPostAllowed(telemetryPost("http://127.0.0.1:8099/u", "{}")));
#endif

    // Anything else at all.
    CHECK(!netPostAllowed(crashPost("ftp://127.0.0.1/", "{}")));
    CHECK(!netPostAllowed(crashPost("file:///etc/passwd", "{}")));
    CHECK(!netPostAllowed(crashPost("telemetry.foxsdr.com", "{}")));  // no scheme
    CHECK(!netPostAllowed(crashPost("https:///nohost", "{}")));       // empty authority
    CHECK(!netPostAllowed(crashPost("https://host:port/", "{}")));    // port not a number
    CHECK(!netPostAllowed(crashPost("https://host:0/", "{}")));
    CHECK(!netPostAllowed(crashPost("https://host:70000/", "{}")));

    // An empty url or an empty body is refused before anything else: both
    // desktop transports have always returned early on them.
    CHECK(!netPostAllowed(crashPost("", "{}")));
    CHECK(!netPostAllowed(crashPost(kCrashUrl, "")));
}

void testTheUrlSplit() {
    NetPostUrl u;
    CHECK(netPostSplitUrl("https://foxsdr.com/api/crash", u));
    CHECK(u.scheme == "https");
    CHECK(u.host == "foxsdr.com");
    CHECK(u.port == 443);
    CHECK(u.authority == "foxsdr.com");
    CHECK(u.target == "/api/crash");

    CHECK(netPostSplitUrl("http://127.0.0.1:8099", u));
    CHECK(u.scheme == "http");
    CHECK(u.host == "127.0.0.1");
    CHECK(u.port == 8099);
    CHECK(u.authority == "127.0.0.1:8099");
    // Never empty: httplib's Post() and Java's URL both want a path.
    CHECK(u.target == "/");

    CHECK(netPostSplitUrl("https://telemetry.foxsdr.com/u?x=1", u));
    CHECK(u.target == "/u?x=1");

    CHECK(!netPostSplitUrl("nonsense", u));
    CHECK(!netPostSplitUrl("://nohost/", u));

    CHECK(netPostLoopbackHost("127.0.0.1"));
    CHECK(netPostLoopbackHost("localhost"));
    CHECK(netPostLoopbackHost("::1"));
    CHECK(!netPostLoopbackHost("127.0.0.2"));
    CHECK(!netPostLoopbackHost("localhost.evil.example"));
    CHECK(!netPostLoopbackHost(""));
}

// ---------------------------------------------------------------------------
// What the real senders hand a transport
// ---------------------------------------------------------------------------

void testTheUsageSenderHandsTheTransportExactlyThePayload() {
    Recorder rec;
    rec.reply.attempted = true;
    rec.reply.status = 204;
    rec.install();
    {
        TelemetryReporter r;
        r.send(kUsageUrl, kUsageBody);
        // The destructor joins the sending thread, so by the closing brace the
        // hook has certainly run.
    }
    Recorder::remove();

    CHECK(rec.seen.size() == 1);
    if (rec.seen.size() == 1) {
        // THE WHOLE REQUEST, not just the body: a content type or an endpoint
        // that drifted would be exactly as wrong as a changed payload.
        CHECK(same(rec.seen[0], telemetryPost(kUsageUrl, kUsageBody)));
    }
}

void testTheHeartbeatSenderHandsTheTransportTheBeat() {
    Recorder rec;
    rec.reply.attempted = true;
    rec.reply.status = 204;
    rec.install();
    const std::string id = newInstallId();
    {
        HeartbeatSender h;
        h.configure(kUsageUrl, id, "0.97.0", 300);
        CHECK(h.due(1000.0));
        h.poll(1000.0);
        // The destructor joins the beat thread.
    }
    Recorder::remove();

    CHECK(rec.seen.size() == 1);
    if (rec.seen.size() == 1) {
        CHECK(same(rec.seen[0], telemetryPost(kUsageUrl, HeartbeatSender::beatJson(id, "0.97.0"))));
        // And the beat is still the beat - three fields, no session data.
        CHECK(rec.seen[0].body.find("\"beat\"") != std::string::npos);
        CHECK(rec.seen[0].body.find("sessionSec") == std::string::npos);
    }
}

void testTheCrashSenderHandsTheTransportExactlyTheReport() {
    Recorder rec;
    rec.reply.attempted = true;
    rec.reply.status = 202;
    rec.install();
    auto cancel = std::make_shared<UploadCancel>();
    const UploadResult res = postCrashReport(kCrashUrl, kCrashBody, cancel);
    Recorder::remove();

    CHECK(rec.seen.size() == 1);
    if (rec.seen.size() == 1) {
        CHECK(same(rec.seen[0], crashPost(kCrashUrl, kCrashBody)));
    }
    // ...and the transport's answer is mapped back the way the sweep reads it.
    CHECK(res.attempted);
    CHECK(res.accepted);
    CHECK(res.status == 202);
    CHECK(!res.rateLimited);
}

void testRateLimitingSurvivesTheSeam() {
    Recorder rec;
    rec.reply.attempted = true;
    rec.reply.status = 429;
    rec.reply.retryAfterSeconds = 3600;
    rec.install();
    auto cancel = std::make_shared<UploadCancel>();
    const UploadResult res = postCrashReport(kCrashUrl, kCrashBody, cancel);
    Recorder::remove();

    CHECK(res.attempted);
    CHECK(!res.accepted);
    CHECK(res.status == 429);
    CHECK(res.rateLimited);
    CHECK(res.retryAfterSeconds == 3600);
}

void testARefusedDestinationNeverReachesTheTransport() {
    // THE NEGATIVE THAT MATTERS. It is not enough that a plain-http report
    // fails; nothing may be handed to a transport at all, because a transport
    // is the thing that could send it.
    Recorder rec;
    rec.reply.attempted = true;
    rec.reply.status = 204;
    rec.install();

    auto cancel = std::make_shared<UploadCancel>();
    const UploadResult res = postCrashReport("http://crash.invalid/api/crash", kCrashBody, cancel);
    CHECK(rec.seen.empty());
    CHECK(!res.attempted);

    {
        TelemetryReporter r;
        r.send("http://telemetry.foxsdr.com/", kUsageBody);
    }
#if defined(CASCADE_ANDROID)
    // Android's usage sender permits loopback, and telemetry.foxsdr.com is
    // not loopback, so this is still refused.
    CHECK(rec.seen.empty());
#else
    CHECK(rec.seen.empty());
#endif

    // And the Android entry point applies the same gate before its own hook.
    const NetPostResult a = androidNetPost(crashPost("http://crash.invalid/", kCrashBody), nullptr);
    CHECK(rec.seen.empty());
    CHECK(!a.attempted);
    Recorder::remove();
}

// ---------------------------------------------------------------------------
// The Android entry point
// ---------------------------------------------------------------------------

void testAndroidSendsTheSameBytesToTheSameEndpoint() {
    // THE ASSERTION THIS FILE EXISTS FOR. Capture what the desktop senders
    // hand a transport, then capture what the Android entry point hands the
    // same transport for the same input, and require the two to be equal in
    // every field - endpoint, body, content type, timeouts, scheme rule and
    // user agent.
    Recorder rec;
    rec.reply.attempted = true;
    rec.reply.status = 204;
    rec.install();
    {
        TelemetryReporter r;
        r.send(kUsageUrl, kUsageBody);
    }
    auto cancel = std::make_shared<UploadCancel>();
    postCrashReport(kCrashUrl, kCrashBody, cancel);
    androidNetPost(telemetryPost(kUsageUrl, kUsageBody), nullptr);
    androidNetPost(crashPost(kCrashUrl, kCrashBody), nullptr);
    Recorder::remove();

    CHECK(rec.seen.size() == 4);
    if (rec.seen.size() == 4) {
        CHECK(same(rec.seen[0], rec.seen[2]));  // usage: desktop vs Android
        CHECK(same(rec.seen[1], rec.seen[3]));  // crash: desktop vs Android
        CHECK(rec.seen[2].body == kUsageBody);
        CHECK(rec.seen[3].body == kCrashBody);
    }
}

void testTheAndroidTransportRefusesHonestlyWithNoJavaSide() {
    // No hook, no Java: this is a host build, so there is nothing to call. It
    // must say so by NOT reporting an attempt rather than by pretending.
    CHECK(!androidNetReady());
    // Handing it a null VM changes nothing and must not crash.
    androidNetInit(nullptr, nullptr);
    CHECK(!androidNetReady());
    const NetPostResult r = androidNetPost(crashPost(kCrashUrl, kCrashBody), nullptr);
    CHECK(!r.attempted);
    CHECK(r.status == 0);
    CHECK(!r.cancelled);
}

// THE REAL UploadCancel CANNOT STAND IN HERE, and finding that out is worth
// the paragraph. The void* it carries has TWO meanings, chosen at compile
// time: on Windows a WinHTTP request handle, on Linux a httplib::ClientImpl*
// it calls stop() on, and on Android the Java transport's call TOKEN, which is
// a small integer and not a pointer to anything. Every real build pairs the
// producer and the consumer correctly - the Android branch of
// UploadCancel::cancel() hands its handle to androidNetCancel() - but a HOST
// build driving the Android entry point pairs the token with the httplib
// branch, and the first cancel dereferences 0x4d. (It did, in the first run of
// this file: SIGSEGV inside pthread_mutex_lock under UploadCancel::cancel.)
//
// So this fake implements the same three-call contract and nothing else. What
// it verifies is the SKELETON in androidNetPost - publish, send, take, and the
// two orderings that matter - which is identical code on both platforms.
struct FakeCancel {
    std::atomic<bool> cancelled{false};
    std::atomic<void*> handle{nullptr};
    void* lastPublished = nullptr;

    bool publish(void* h) {
        lastPublished = h;
        if (cancelled.load()) { return false; }
        handle.store(h);
        return !cancelled.load();
    }
    void* take() { return handle.exchange(nullptr); }
    void cancel() {
        cancelled.store(true);
        handle.store(nullptr);
    }

    NetPostCancel hooks() {
        NetPostCancel c;
        c.publish = [this](void* h) { return this->publish(h); };
        c.take = [this]() { return this->take(); };
        c.cancelled = [this]() { return this->cancelled.load(); };
        return c;
    }
};

void testACancelBeforeThePostSendsNothing() {
    Recorder rec;
    rec.reply.attempted = true;
    rec.reply.status = 204;
    rec.install();

    FakeCancel fake;
    fake.cancel();  // the shutdown path got there first
    const NetPostCancel hooks = fake.hooks();

    const NetPostResult r = androidNetPost(crashPost(kCrashUrl, kCrashBody), &hooks);
    Recorder::remove();

    CHECK(r.cancelled);
    CHECK(!r.attempted);
    // NOTHING WAS HANDED TO THE TRANSPORT. A cancelled upload that still
    // reached a socket would be a cancel in name only.
    CHECK(rec.seen.empty());
}

void testACancelDuringThePostIsReported() {
    Recorder rec;
    rec.reply.attempted = true;
    rec.reply.status = 0;  // the transport failed, which is what a cancel looks like

    FakeCancel fake;
    const NetPostCancel hooks = fake.hooks();
    // Cancel from inside the transport, which is where it lands in a real run:
    // the worker is blocked in the request when another thread cancels.
    setNetPostHookForTest([&rec, &fake](const NetPost& p) {
        rec.seen.push_back(p);
        fake.cancel();
        return rec.reply;
    });

    const NetPostResult r = androidNetPost(crashPost(kCrashUrl, kCrashBody), &hooks);
    Recorder::remove();

    CHECK(rec.seen.size() == 1);
    CHECK(r.attempted);
    CHECK(r.cancelled);
    // The token the transport published is a real, non-zero key; cancel()
    // consumed it, so take() finds nothing left.
    CHECK(fake.lastPublished != nullptr);
    CHECK(fake.take() == nullptr);
}

void testAPublishedTokenIsNeverNull() {
    // The Java side keys its in-flight map by this value and a zero would
    // collide with "no request"; the C++ side stores it in the same slot a
    // pointer used to occupy, where nullptr means "nothing to cancel".
    Recorder rec;
    rec.reply.attempted = true;
    rec.reply.status = 204;
    rec.install();

    std::vector<void*> published;
    NetPostCancel hooks;
    hooks.publish = [&published](void* h) {
        published.push_back(h);
        return true;
    };
    hooks.take = []() -> void* { return nullptr; };
    hooks.cancelled = []() { return false; };

    androidNetPost(crashPost(kCrashUrl, kCrashBody), &hooks);
    androidNetPost(crashPost(kCrashUrl, kCrashBody), &hooks);
    Recorder::remove();

    CHECK(published.size() == 2);
    if (published.size() == 2) {
        CHECK(published[0] != nullptr);
        CHECK(published[1] != nullptr);
        // Distinct, or one cancel would abort somebody else's upload.
        CHECK(published[0] != published[1]);
    }
}

// ---------------------------------------------------------------------------
// The hook itself
// ---------------------------------------------------------------------------

void testTheHookIsAbsentUnlessATestInstalledIt() {
    // A shipped run must never take the fake path. Nothing but a test calls
    // setNetPostHookForTest, and removing it must actually remove it.
    bool handled = true;
    (void)runNetPostHook(crashPost(kCrashUrl, kCrashBody), handled);
    CHECK(!handled);

    setNetPostHookForTest([](const NetPost&) {
        NetPostResult r;
        r.status = 418;
        return r;
    });
    handled = false;
    const NetPostResult r = runNetPostHook(crashPost(kCrashUrl, kCrashBody), handled);
    CHECK(handled);
    CHECK(r.status == 418);

    setNetPostHookForTest(NetPostHook());
    handled = true;
    (void)runNetPostHook(crashPost(kCrashUrl, kCrashBody), handled);
    CHECK(!handled);
}

void testTheOsFieldNamesThePlatform() {
    // PRIVACY.md's OS row lists what this field can say. On Android it must
    // say Android: uname() answers "Linux <vendor kernel>" there, which would
    // file every phone under the desktop Linux row AND carry a vendor build id
    // that identifies the device far better than the rest of the payload does.
    const std::string os = osDescription();
    CHECK(!os.empty());
    CHECK(os.size() < 40);
#if defined(CASCADE_ANDROID)
    CHECK(os.rfind("Android", 0) == 0);
    CHECK(os.find("Linux") == std::string::npos);
    // Digits and dots after the name, never a vendor string.
    for (std::size_t i = 8; i < os.size(); ++i) {
        const char c = os[i];
        CHECK((c >= '0' && c <= '9') || c == '.');
    }
#elif defined(_WIN32)
    CHECK(os.rfind("Windows", 0) == 0 || os == "unknown");
#else
    CHECK(os.find("Android") == std::string::npos);
#endif
}

}  // namespace

int main() {
    testTheUsageRequestIsPinned();
    testTheCrashRequestIsPinned();
    testTheGateRefusesEverythingButHttpsAndLoopbackHttp();
    testTheUrlSplit();
    testTheUsageSenderHandsTheTransportExactlyThePayload();
    testTheHeartbeatSenderHandsTheTransportTheBeat();
    testTheCrashSenderHandsTheTransportExactlyTheReport();
    testRateLimitingSurvivesTheSeam();
    testARefusedDestinationNeverReachesTheTransport();
    testAndroidSendsTheSameBytesToTheSameEndpoint();
    testTheAndroidTransportRefusesHonestlyWithNoJavaSide();
    testACancelBeforeThePostSendsNothing();
    testACancelDuringThePostIsReported();
    testAPublishedTokenIsNeverNull();
    testTheHookIsAbsentUnlessATestInstalledIt();
    testTheOsFieldNamesThePlatform();
    return testSummary("test_net_post");
}
