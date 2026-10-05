// A configuration directory that will not answer must not freeze the window.
//
// THE FIELD REPORT THIS EXISTS FOR. "hang ntdll.dll @ __std_fs_get_stats"
// (0.99.59, Windows 10.0.19045, an RTL-SDR open and streaming, 880 s uptime).
// Resolved against the symbol archive the GUI thread's stack is
//
//   main -> AppWindow::run -> AppWindow::drawUi -> AppWindow::testerLinkPoll
//     -> core::claimLinkRequestFile -> __std_fs_get_stats -> KERNELBASE -> ntdll
//
// and it stopped there for five seconds or more. testerLinkPoll() is the
// beta-tester portal link's ONCE-A-SECOND check for a one-shot file in the
// user's config directory (%APPDATA%\foxsdr\link-request). Every session of
// every user runs it, tester or not, because it has to be running before a
// link click can arrive. It was a synchronous std::filesystem::exists() on the
// GUI thread, so any second in which %APPDATA% could not answer - a redirected
// or network profile, a cloud-synced folder, an antivirus holding the
// directory, a spun-down disk - was a second in which the frame loop did not
// turn over. It had been waved through with the comment "a stat() is cheap".
//
// WHAT IS TESTED, and at which level.
//
//   1. THE REAL THING, on a real slow filesystem (Windows). A config path on a
//      UNC share whose server never answers (192.0.2.1 is TEST-NET-1, reserved
//      and unroutable, so the SMB client sits in its connect timeout - measured
//      at 26 s on the machine that wrote this) makes the poll's exists() block
//      exactly as the field's did. A real AppWindow is pointed at it and driven
//      the way the frame loop drives it, under a real HangWatchdog. Against the
//      synchronous poll the loop stops beating and the watchdog files a hang
//      report; this must write none. If the machine answers the dead share
//      quickly (offline, or a network that rejects it at once) the block says
//      so and SKIPS - it never passes by not having been slow.
//
//   2. THE WIRING, because no test can stage every slow disk: no GUI source may
//      call claimLinkRequestFile directly. The call lives in gui/
//      link_request_poll.hpp, on a worker, and nowhere else.
//
//   3. THE SAME FRAME LOOP ON ANY PLATFORM, with the slow filesystem replaced
//      by a claimer that sleeps - which is a slow disk as far as the frame loop
//      can tell - first against gui::LinkRequestPoll alone and then against a
//      real AppWindow's testerLinkPoll(). Both under a real HangWatchdog, both
//      requiring no report and an unbroken heartbeat, with the synchronous
//      call of the same claimer as the control that proves the harness can see
//      the fault. Around them, the properties the fix must not lose: the answer
//      still arrives, only one worker is ever out, quit does not wait for a
//      wedged one, and a slow check is said once in the log and not once a
//      frame.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "core/hang_watchdog.hpp"
#include "gui/app_window.hpp"
#include "gui/link_request_poll.hpp"
#include "test_check.hpp"

namespace fs = std::filesystem;
using cascade::core::HangWatchdog;
using cascade::gui::LinkRequestPoll;

namespace cascade::gui {

// The friend AppWindow names for the test files that drive its private poll.
struct AppWindowTestAccess {
    static void bindClaimer(AppWindow& a, LinkRequestPoll::Claimer c) {
        a.linkRequestPoll_.bind(std::move(c));
    }
    static bool inFlight(AppWindow& a) { return a.linkRequestPoll_.inFlight(); }
    // A decision already pending, so a claimed token is HELD in the queue
    // rather than handed on to a network lookup - the queue is what a test can
    // read the token back from.
    static void holdPrompt(AppWindow& a) {
        a.testerLinkPending_ = AppWindow::TesterLinkPending{};
    }
    static std::string queuedToken(AppWindow& a) { return a.testerLinkQueuedToken_; }

    // The directory the poll looks in is derived from configPath_ on every
    // call, so pointing it somewhere else after construction is what lets a
    // test slow ONLY the poll - the constructor reads the config synchronously
    // (before any watchdog exists) and must not be the thing under test.
    static void pointConfigAt(AppWindow& a, const std::string& path) { a.configPath_ = path; }

    // One frame's worth of the poll, with the ~1 Hz gate forced open. `now` in
    // testerLinkPoll is glfwGetTime(), which is a constant 0 in a process that
    // never initialised GLFW, so without this reset only the first call of a
    // run would ever reach the file.
    static void frame(AppWindow& a) {
        a.testerLinkPollLast_ = -1.0e9;
        a.testerLinkPoll();
    }
    // A frame that does not touch the gate.
    static void tick(AppWindow& a) { a.testerLinkPoll(); }
};

}  // namespace cascade::gui

using Access = cascade::gui::AppWindowTestAccess;
using cascade::gui::AppWindow;

namespace {

fs::path g_scratch;

void setEnv(const char* name, const std::string& value) {
#if defined(_WIN32)
    _putenv_s(name, value.c_str());
    ::SetEnvironmentVariableA(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

// Per-user directories pointed at a scratch folder and every network-facing URL
// at a port nothing listens on, exactly as tests/test_tester_link_app.cpp does:
// constructing an AppWindow must not touch the owner's real FoxSDR folders or
// reach a real endpoint.
void isolate() {
#if defined(_WIN32)
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(getpid());
#endif
    g_scratch = fs::temp_directory_path() / ("foxsdr_link_request_poll_" + std::to_string(pid));
    std::error_code ec;
    fs::remove_all(g_scratch, ec);
    fs::create_directories(g_scratch, ec);
    const std::string s = g_scratch.string();
    for (const char* v : {"LOCALAPPDATA", "APPDATA", "USERPROFILE", "HOME", "XDG_CONFIG_HOME",
                          "XDG_STATE_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"}) {
        setEnv(v, s);
    }
    setEnv("FOXSDR_TELEMETRY_URL", "http://127.0.0.1:9");
    setEnv("FOXSDR_TESTER_USAGE_URL", "http://127.0.0.1:9");
    setEnv("FOXSDR_BETA_API_URL", "http://127.0.0.1:9");
}

double nowMs() {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

#if defined(_WIN32)

// A share on a server that never answers. TEST-NET-1 (RFC 5737) is reserved
// for documentation and routed nowhere, so the SMB client waits out its own
// connect timeout - a real filesystem call that really does not return.
//
// A DIFFERENT ADDRESS EVERY RUN. The SMB client remembers a server it could not
// reach and fails the next attempt at it quickly (seen on the machine that
// wrote this: 26 s on the first call, then instant for the runs that followed),
// so a fixed address would make every run after the first skip this block and
// make a regression invisible. The last octet is taken from the clock and the
// process id, spread over the 253 addresses of the documentation range.
std::string deadShareConfig() {
    static const std::string path = [] {
        const auto t = std::chrono::steady_clock::now().time_since_epoch().count();
        const unsigned long pid = ::GetCurrentProcessId();
        const unsigned host = 1u + static_cast<unsigned>((static_cast<unsigned long long>(t) / 1000u + pid * 7u) % 253u);
        return "\\\\192.0.2." + std::to_string(host) + "\\foxsdr-test\\foxsdr\\config.json";
    }();
    return path;
}

// Whether a status call on the dead share is slow RIGHT NOW. Run on a detached
// thread with shared state: a std::async future would block its own destructor
// for the whole timeout, which is the very thing being avoided.
bool deadShareIsSlow(double askMs) {
    struct State {
        std::atomic<bool> done{false};
    };
    auto state = std::make_shared<State>();
    std::thread([state] {
        std::error_code ec;
        (void)fs::exists(fs::path(deadShareConfig()).parent_path() / "link-request", ec);
        state->done.store(true);
    }).detach();
    const double until = nowMs() + askMs;
    while (nowMs() < until) {
        if (state->done.load()) { return false; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return !state->done.load();
}

#endif

// --- 1. THE FIELD HANG, ON A REAL SLOW FILESYSTEM ---------------------------
void checkDeadConfigDirectoryDoesNotFreezeTheFrameLoop() {
#if defined(_WIN32)
    if (!deadShareIsSlow(2500.0)) {
        std::printf("SKIP: a status call on the dead share answered inside 2.5 s on this "
                    "machine, so it cannot reproduce a slow configuration directory here\n");
        ++g_checksSkipped;
        return;
    }

    const fs::path cfg = g_scratch / "dead" / "config.json";
    std::error_code ec;
    fs::create_directories(cfg.parent_path(), ec);
    AppWindow app(cfg.string());
    Access::pointConfigAt(app, deadShareConfig());

    const fs::path reports = g_scratch / "reports";
    fs::create_directories(reports, ec);
    HangWatchdog w;
    w.setSuppressionForTest(HangWatchdog::SuppressionForTest::NeverSuppress);
    w.start(reports.string(), 800);
    CHECK(w.running());

    // Healthy frames first, so a report afterwards cannot be blamed on the
    // loop never having started.
    for (int i = 0; i < 50; ++i) {
        w.heartbeat();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(w.reportsWritten() == 0u);

    // The frame loop: beat, run the poll the way drawUi does, present. Three
    // seconds of it is far longer than the 800 ms threshold and far shorter
    // than the share's own timeout, so a poll that waits for the share cannot
    // come back inside the window.
    double worstGap = 0.0;
    double last = nowMs();
    int frames = 0;
    const double until = last + 3000.0;
    while (nowMs() < until) {
        w.heartbeat();
        Access::frame(app);
        const double t = nowMs();
        if (t - last > worstGap) { worstGap = t - last; }
        last = t;
        ++frames;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // THE CHECK THE FIELD REPORT IS. The synchronous poll writes one hang
    // report here and never gets through a second frame.
    CHECK(w.reportsWritten() == 0u);
    CHECK(frames > 100);
    CHECK(worstGap < 800.0);
    std::printf("  dead share: %d frames in 3 s, worst gap %.0f ms, hang reports %u\n", frames,
                worstGap, w.reportsWritten());
    w.stop();
#else
    std::printf("SKIP (not Windows): the dead-share reproduction is a UNC path\n");
    ++g_checksSkipped;
#endif
}

// --- 2. THE WIRING ----------------------------------------------------------
//
// The call site lives in AppWindow, which no test can run against every slow
// disk, so this reads the GUI sources themselves: nothing under src/gui may
// call claimLinkRequestFile directly (it is the poll's worker that does), and
// AppWindow must actually go through the poll.
void checkTheClaimIsNotMadeOnTheGuiThread() {
    const fs::path guiDir = fs::path(__FILE__).parent_path().parent_path() / "src" / "gui";
    std::error_code ec;
    CHECK(fs::is_directory(guiDir, ec));
    int scanned = 0;
    int directCalls = 0;
    int pollUses = 0;
    for (const auto& entry : fs::directory_iterator(guiDir, ec)) {
        const fs::path p = entry.path();
        if (p.extension() != ".cpp" && p.extension() != ".hpp") { continue; }
        std::ifstream in(p, std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
        ++scanned;
        const bool isPollHeader = p.filename() == "link_request_poll.hpp";
        std::size_t line = 1;
        std::size_t lineStart = 0;
        for (std::size_t i = 0; i < text.size(); ++i) {
            if (text[i] != '\n') { continue; }
            const std::string l = text.substr(lineStart, i - lineStart);
            const std::size_t code = l.find("//");
            const std::string body = code == std::string::npos ? l : l.substr(0, code);
            if (body.find("claimLinkRequestFile(") != std::string::npos && !isPollHeader) {
                std::printf("  %s:%zu claims the link-request file on the calling thread\n",
                            p.filename().string().c_str(), line);
                ++directCalls;
            }
            if (p.filename() == "app_window.cpp" &&
                body.find("linkRequestPoll_.request(") != std::string::npos) {
                ++pollUses;
            }
            ++line;
            lineStart = i + 1;
        }
    }
    CHECK(scanned > 10);
    CHECK(directCalls == 0);
    CHECK(pollUses >= 1);
}

// --- 3. THE SAME LOOP, ON ANY PLATFORM --------------------------------------

// A claimer that behaves like a slow disk: it takes `blockMs` to answer,
// records how many times and from which thread it was asked, and notices being
// run twice at once - which is what a second worker piling up behind a dead
// directory would look like.
struct SlowDisk {
    std::atomic<int> blockMs{0};
    std::atomic<int> calls{0};
    std::atomic<int> concurrent{0};
    std::atomic<int> maxConcurrent{0};
    std::atomic<bool> askedFromCaller{false};
    std::atomic<bool> throws{false};
    std::string token;
    const std::thread::id caller = std::this_thread::get_id();

    LinkRequestPoll::Claimer claimer() {
        return [this](const std::string&) -> std::string {
            if (std::this_thread::get_id() == caller) { askedFromCaller.store(true); }
            const int live = concurrent.fetch_add(1) + 1;
            int seen = maxConcurrent.load();
            while (live > seen && !maxConcurrent.compare_exchange_weak(seen, live)) {}
            calls.fetch_add(1);
            std::this_thread::sleep_for(std::chrono::milliseconds(blockMs.load()));
            concurrent.fetch_sub(1);
            if (throws.load()) { throw std::runtime_error("the disk went away"); }
            return token;
        };
    }
};

struct LoopStats {
    int frames = 0;
    double worstGapMs = 0.0;
};

// What AppWindow::run does between two presents: beat, do the frame's work,
// wait out the rest of the frame.
template <class Frame>
LoopStats runFrameLoop(HangWatchdog& w, double forMs, Frame&& frame) {
    LoopStats s;
    double last = nowMs();
    const double until = last + forMs;
    while (nowMs() < until) {
        w.heartbeat();
        frame();
        const double t = nowMs();
        if (t - last > s.worstGapMs) { s.worstGapMs = t - last; }
        last = t;
        ++s.frames;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return s;
}

fs::path reportDir(const std::string& tag) {
    const fs::path d = g_scratch / ("reports-" + tag);
    std::error_code ec;
    fs::create_directories(d, ec);
    return d;
}

void startWatchdog(HangWatchdog& w, const fs::path& dir) {
    w.setSuppressionForTest(HangWatchdog::SuppressionForTest::NeverSuppress);
    w.start(dir.string(), 800);
    CHECK(w.running());
    // Healthy frames first, so a report afterwards cannot be blamed on the
    // loop never having started.
    for (int i = 0; i < 50; ++i) {
        w.heartbeat();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(w.reportsWritten() == 0u);
}

// THE CONTROL. The same claimer, called where testerLinkPoll used to call it:
// on the thread that heartbeats. If the harness cannot see THIS, a green result
// below means nothing.
void checkTheSynchronousCallIsReported() {
    SlowDisk disk;
    disk.blockMs.store(2500);
    HangWatchdog w;
    startWatchdog(w, reportDir("control"));
    (void)disk.claimer()("dir");  // the old poll: the frame that asks also waits
    CHECK(w.reportsWritten() == 1u);
    w.stop();
}

// THE FIELD HANG, against the poll class: a 2.5 s answer against an 800 ms
// threshold. The loop must keep beating, the watchdog must write nothing, and
// the answer must still arrive - a fix that merely dropped the check would pass
// everything but the last of these.
void checkSlowProbeDoesNotStallTheFrameLoop() {
    SlowDisk disk;
    disk.blockMs.store(2500);
    disk.token = "token-from-the-slow-disk";
    HangWatchdog w;
    startWatchdog(w, reportDir("class"));

    LinkRequestPoll poll;
    poll.bind(disk.claimer());
    int collected = 0;
    std::string got;
    const double t0 = nowMs();
    const LoopStats s = runFrameLoop(w, 3200.0, [&] {
        // Asked EVERY frame until it has an answer, as a caller with no memory
        // of having asked would: the disk must still be asked once.
        if (collected == 0) { poll.request("dir"); }
        std::string tok;
        if (poll.poll(tok)) {
            ++collected;
            got = tok;
        }
    });
    CHECK(nowMs() - t0 < 3600.0);

    CHECK(w.reportsWritten() == 0u);
    CHECK(s.frames > 100);
    CHECK(s.worstGapMs < 800.0);
    // The answer arrived, once, on a later frame - and the disk was asked once,
    // from a thread that is not this one.
    CHECK(collected == 1);
    CHECK(got == "token-from-the-slow-disk");
    CHECK(disk.calls.load() == 1);
    CHECK(!disk.askedFromCaller.load());
    std::printf("  slow probe: %d frames in 3.2 s, worst gap %.0f ms, hang reports %u\n",
                s.frames, s.worstGapMs, w.reportsWritten());
    w.stop();
}

// A directory that never answers must cost one parked thread, not one a second
// for the rest of the session.
void checkOnlyOneWorkerIsEverOut() {
    SlowDisk disk;
    disk.blockMs.store(1200);
    LinkRequestPoll poll;
    poll.bind(disk.claimer());
    CHECK(poll.request("dir"));
    CHECK(poll.inFlight());
    int started = 0;
    for (int i = 0; i < 300; ++i) {
        if (poll.request("dir")) { ++started; }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(started == 0);
    CHECK(disk.calls.load() == 1);

    const double until = nowMs() + 5000.0;
    std::string tok;
    while (nowMs() < until && !poll.poll(tok)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(!poll.inFlight());
    // Free again once it has answered.
    CHECK(poll.request("dir"));
    CHECK(disk.maxConcurrent.load() == 1);
    poll.reap();
}

// The common case: a disk that answers in milliseconds, and the answers a poll
// can carry - a token, nothing, and an empty directory (the hermetic run).
void checkAnswersAreDeliveredOnceAndHermeticRunsAskNothing() {
    SlowDisk disk;
    disk.blockMs.store(0);
    disk.token = "abc";
    LinkRequestPoll poll;
    poll.bind(disk.claimer());

    CHECK(poll.request("dir"));
    std::string tok;
    const double until = nowMs() + 3000.0;
    bool got = false;
    while (nowMs() < until && !(got = poll.poll(tok))) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(got);
    CHECK(tok == "abc");
    // Collected exactly once: a token handed out twice would start two lookups.
    CHECK(!poll.poll(tok));
    CHECK(tok.empty());

    // "Nothing there" is an answer too, and is not a token.
    disk.token.clear();
    CHECK(poll.request("dir"));
    got = false;
    const double until2 = nowMs() + 3000.0;
    while (nowMs() < until2 && !(got = poll.poll(tok))) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(got);
    CHECK(tok.empty());

    // An empty directory is how a hermetic run says "do not touch the disk".
    const int before = disk.calls.load();
    CHECK(!poll.request(""));
    CHECK(!poll.inFlight());
    CHECK(disk.calls.load() == before);
}

// A throw on the worker must not be rethrown out of the frame loop.
void checkAThrowingDiskIsContained() {
    SlowDisk disk;
    disk.throws.store(true);
    LinkRequestPoll poll;
    poll.bind(disk.claimer());
    CHECK(poll.request("dir"));
    std::string tok = "stale";
    bool got = false;
    const double until = nowMs() + 3000.0;
    try {
        while (nowMs() < until && !(got = poll.poll(tok))) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    } catch (...) {
        CHECK(false);  // reached the frame loop
    }
    CHECK(got);
    CHECK(tok.empty());
}

// QUIT does not wait for a directory that is not coming back.
void checkQuitAbandonsAWedgedProbe() {
    auto disk = std::make_shared<SlowDisk>();
    disk->blockMs.store(2500);
    double reapMs = 0.0;
    {
        LinkRequestPoll poll;
        // The worker owns what it touches: the disk outlives the poll here
        // because it holds a reference of its own.
        poll.bind([disk](const std::string& d) { return disk->claimer()(d); });
        CHECK(poll.request("dir"));
        const double t0 = nowMs();
        poll.reap();
        reapMs = nowMs() - t0;
        CHECK(!poll.inFlight());
        poll.reap();  // twice is a no-op, not a second detached thread
    }
    // Bounded by kQuitGrace, not by the 2500 ms the disk is taking.
    CHECK(reapMs < 1200.0);
    CHECK(disk->concurrent.load() == 1);  // still inside the disk: abandoned, not joined
    const double until = nowMs() + 5000.0;
    while (nowMs() < until && disk->concurrent.load() != 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(disk->concurrent.load() == 0);
}

// "Still working" has to be visible somewhere: said once when the check has been
// out too long, once when it comes back - never once a frame, and never for a
// disk that answered at once.
void checkASlowCheckIsReportedOnceNotEveryFrame() {
    SlowDisk disk;
    disk.blockMs.store(700);
    LinkRequestPoll poll;
    poll.bind(disk.claimer());
    poll.setStuckAfterForTest(std::chrono::milliseconds(200));
    double s = -1.0;
    CHECK(poll.request("dir"));
    CHECK(poll.takeNotice(s) == LinkRequestPoll::Notice::None);
    std::this_thread::sleep_for(std::chrono::milliseconds(350));
    CHECK(poll.takeNotice(s) == LinkRequestPoll::Notice::Stuck);
    CHECK(s >= 0.2);
    for (int i = 0; i < 40; ++i) {
        CHECK(poll.takeNotice(s) == LinkRequestPoll::Notice::None);
    }
    std::string tok;
    const double until = nowMs() + 5000.0;
    while (nowMs() < until && !poll.poll(tok)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(poll.takeNotice(s) == LinkRequestPoll::Notice::Recovered);
    CHECK(s >= 0.6);
    CHECK(poll.takeNotice(s) == LinkRequestPoll::Notice::None);

    // A disk that answers at once is never mentioned.
    disk.blockMs.store(0);
    CHECK(poll.request("dir"));
    const double until2 = nowMs() + 3000.0;
    while (nowMs() < until2 && !poll.poll(tok)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(poll.takeNotice(s) == LinkRequestPoll::Notice::None);
}

// --- 4. THE REAL testerLinkPoll ---------------------------------------------
//
// The class above is only half of it: the other half is that testerLinkPoll()
// goes through it. A real AppWindow, its poll bound to a claimer that sleeps,
// driven by the frame loop under a real HangWatchdog.
void checkAppWindowPollNeverHoldsAFrame() {
    const fs::path cfg = g_scratch / "inject" / "config.json";
    std::error_code ec;
    fs::create_directories(cfg.parent_path(), ec);
    AppWindow app(cfg.string());

    SlowDisk disk;
    disk.blockMs.store(2500);
    disk.token = "token-claimed-from-the-config-dir";
    Access::bindClaimer(app, disk.claimer());
    // A prompt already awaiting a decision, so the claimed token is held in the
    // queue where this test can read it, not handed to a network lookup.
    Access::holdPrompt(app);

    HangWatchdog w;
    startWatchdog(w, reportDir("appwindow"));
    // The first frame opens the ~1 Hz gate and asks; every later frame runs the
    // poll with the gate as the clock leaves it, which is what collects.
    bool first = true;
    const LoopStats s = runFrameLoop(w, 3300.0, [&] {
        if (first) {
            Access::frame(app);
            first = false;
        } else {
            Access::tick(app);
        }
    });

    CHECK(w.reportsWritten() == 0u);
    CHECK(s.frames > 100);
    CHECK(s.worstGapMs < 800.0);
    // testerLinkPoll DID ask the disk - a poll that quietly stopped asking would
    // pass everything above - once, from a worker, and the answer reached the
    // queue on a later frame.
    CHECK(disk.calls.load() == 1);
    CHECK(!disk.askedFromCaller.load());
    CHECK(Access::queuedToken(app) == "token-claimed-from-the-config-dir");
    CHECK(!Access::inFlight(app));
    std::printf("  AppWindow poll: %d frames in 3.3 s, worst gap %.0f ms, hang reports %u\n",
                s.frames, s.worstGapMs, w.reportsWritten());
    w.stop();
}

}  // namespace

int main() {
    isolate();
    checkTheClaimIsNotMadeOnTheGuiThread();
    checkTheSynchronousCallIsReported();
    checkSlowProbeDoesNotStallTheFrameLoop();
    checkOnlyOneWorkerIsEverOut();
    checkAnswersAreDeliveredOnceAndHermeticRunsAskNothing();
    checkAThrowingDiskIsContained();
    checkQuitAbandonsAWedgedProbe();
    checkASlowCheckIsReportedOnceNotEveryFrame();
    checkAppWindowPollNeverHoldsAFrame();
    checkDeadConfigDirectoryDoesNotFreezeTheFrameLoop();
    std::error_code ec;
    if (g_checksFailed == 0) { fs::remove_all(g_scratch, ec); }
    return testSummary("test_link_request_poll");
}
