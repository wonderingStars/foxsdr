// test_pass_control.cpp - THE ENGINE'S PASS ON ITS OWN THREAD: how it is woken, paced and driven (the pump move's steps P4 and P5;
// docs/engine-stage4.md; the survey's section 3).
//
// THE ENGINE ALONE - no window. The window's half (a frame that looks while this runs) is tests/test_window_threaded_look.cpp; this
// holds the engine's: the thread, the wake, the floor, the stepped mode, runPassNow, the work posted to the pass, the proxy apply of a
// plugin directive for a front end that is blocked in stepPass, the engine's own clock and the control watchdog's switch.
//
//   A  A SUBMIT WAKES THE PASS. period 1500 ms, so without the wake a command lands up to 1.5 s late; with it, within a pass's time.
//      Twenty submits from an idle front end: the lag from submit to the state that shows it (the smoke figure P7 measures properly)
//   B  THE FLOOR. A flood of submits for a second becomes at most 1000/floor passes, not a pass per submit; with the floor off the same
//      flood is a flood of passes (the control); a wake after an idle spell is served at once, floor or no floor
//   C  runPassNow: a whole pass that STARTED after the call, so what was submitted before it is applied and published when it returns;
//      bounded (false for a pass that does not come), refused on the control thread itself, inline with no thread
//   D  STEPPED MODE. No pass of its own (not for a wake, not for a period); the three parts run when asked, in order, on the control
//      thread, each visible in what it does (the front part publishes, the back part is the config copy), and a whole runPassNow
//      is the three
//   E  WORK POSTED TO THE PASS (postToPass, runOnPass): run on the control thread in order, at once (a post wakes), inline on the
//      caller for a stepped or absent thread, and never after runOnPass has returned
//   F  A FRONT END BLOCKED IN stepPass STILL ANSWERS THE UNLOAD HANDSHAKE: the pass posts a release directive and waits for the interface
//      to apply it; the interface is the thread inside stepPass, which applies it for the pass (the proxy apply), so the handshake
//      completes in milliseconds, not by timing out at the bound with the modules still mapped
//   G  THE ENGINE'S CLOCK is its own: it runs with no front end at all
//   H  THE CONTROL WATCHDOG follows the diagnostics switch (the dir moved at run time), and a stepped thread that idles while its front
//      end draws is not a hung pass
//   I  THE PASS'S OWN FIGURES (the measurement run's, P7): the log-linear histogram's arithmetic, pinned; and the engine's submit-to-publish
//      lag and pass durations - every wake a submit made is served by a pass whose publish is timed from it, the window the harness marks
//      is the window the JSON reports, and a build with no wake has no lag (the break-it: no stamp in wakeControl -> 0 served)
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "core/app_commands.hpp"
#include "core/diag_log.hpp"
#include "engine/engine.hpp"
#include "engine/engine_host.hpp"
#include "engine/pass_stats.hpp"
#include "test_check.hpp"

using cascade::engine::ControlThreadOptions;
using cascade::engine::Engine;
using cascade::engine::PassPart;
using Clock = std::chrono::steady_clock;
namespace cmd = cascade::core::cmd;

namespace {

void setEnv(const char* n, const std::string& v) {
#if defined(_WIN32)
    ::SetEnvironmentVariableA(n, v.c_str());
    _putenv_s(n, v.c_str());
#else
    ::setenv(n, v.c_str(), 1);
#endif
}

std::filesystem::path g_scratch;

void isolate() {
#if defined(_WIN32)
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(getpid());
#endif
    g_scratch = std::filesystem::temp_directory_path() / ("foxsdr_pass_control_" + std::to_string(pid));
    std::error_code ec;
    std::filesystem::remove_all(g_scratch, ec);
    std::filesystem::create_directories(g_scratch / "plugins");
    for (const char* v : {"LOCALAPPDATA", "APPDATA", "USERPROFILE", "HOME", "XDG_CONFIG_HOME", "XDG_STATE_HOME", "XDG_DATA_HOME",
                          "XDG_CACHE_HOME"}) {
        setEnv(v, g_scratch.string());
    }
    setEnv("FOXSDR_TELEMETRY_URL", "http://127.0.0.1:9");
}

std::string pluginDir() { return (g_scratch / "plugins").string(); }

double msSince(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

template <class F>
double waitFor(F done, int ms) {
    const auto t0 = Clock::now();
    while (!done() && msSince(t0) < ms) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
    return done() ? msSince(t0) : -1.0;
}

// A wait that SPINS (yielding), for a latency probe: a sleep_for(1 ms) is a scheduler tick on Windows (15.6 ms), which would be the
// whole of what is measured.
template <class F>
double spinFor(F done, int ms) {
    const auto t0 = Clock::now();
    while (!done() && msSince(t0) < ms) { std::this_thread::yield(); }
    return done() ? msSince(t0) : -1.0;
}

double median(std::vector<double> v) {
    if (v.empty()) { return -1.0; }
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

ControlThreadOptions opts(int periodMs, int floorMs, bool stepped = false) {
    ControlThreadOptions o;
    o.period = std::chrono::milliseconds(periodMs);
    o.floor = std::chrono::milliseconds(floorMs);
    o.stepped = stepped;
    o.watchdogThresholdMs = 20000;
    return o;
}

}  // namespace

namespace cascade::gui {
struct AppWindowTestAccess {
    static std::uint64_t passes(Engine& e) { return e.controlPasses_.load(); }
    static void park(Engine& e, bool on) { e.controlParkForTest_.store(on); }
    static void stall(Engine& e, int ms) { e.controlStallForTestMs_.store(ms); }
    static float publishedVolume(Engine& e) {
        cascade::core::PublishedState ps;
        for (int i = 0; i < 100; ++i) {
            if (e.receiverSnapshot_->read(ps)) { return static_cast<float>(ps.rx.volume); }
        }
        return -1.0f;
    }
    static unsigned watchdogReports(Engine& e) { return e.controlWatchdog_.reportsWritten(); }
    static std::string watchdogDir(Engine& e) { return e.controlWatchdog_.reportDir(); }
    static void setUnmapHook(void (*h)()) { Engine::testHooks_.pluginsAboutToUnmap = h; }
    static void setPluginDir(std::string (*f)()) { Engine::testHooks_.pluginDir = f; }
    static void betweenPublishes(void (*f)()) { Engine::testHooks_.betweenPublishes = f; }
};
}  // namespace cascade::gui
using A = cascade::gui::AppWindowTestAccess;

namespace {

float volumeOf(int i) { return 0.05f + static_cast<float>(i % 90) / 100.0f; }  // distinct figures, all inside 0..1

// A: the wake ----------------------------------------------------------------------------------------------------------------
void testWake() {
    std::printf("  A: a submit wakes the pass (period 1500 ms)\n");
    Engine e;
    e.initialise();
    e.startControlThread(opts(1500, 2));
    CHECK(e.runPassNow());
    std::vector<double> lags;
    for (int i = 0; i < 20; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(40));   // idle: the loop is in its period wait
        const float want = volumeOf(i + 1);
        const auto t0 = Clock::now();
        e.submitCommand(cmd::makeNum(FOXAPI_OP_SET_VOLUME, want));
        const double ms = spinFor([&] { return A::publishedVolume(e) == want; }, 3000);
        CHECK(ms >= 0.0);
        lags.push_back(ms >= 0.0 ? ms : msSince(t0));
    }
    std::sort(lags.begin(), lags.end());
    std::printf("      submit to the state that shows it, 20 submits: median %.1f ms, min %.1f, max %.1f (the period is 1500)\n",
                median(lags), lags.front(), lags.back());
    CHECK(median(lags) < 150.0);
    CHECK(lags.back() < 400.0);
    // A posted piece of work wakes it too (and runs on the control thread).
    std::atomic<bool> ran{false};
    std::atomic<bool> onThread{false};
    const auto t1 = Clock::now();
    e.postToPass([&] {
        onThread = e.onControlThread();
        ran = true;
    });
    const double postMs = spinFor([&] { return ran.load(); }, 3000);
    std::printf("      postToPass: ran after %.1f ms, on the control thread: %d\n", postMs, onThread.load() ? 1 : 0);
    CHECK(postMs >= 0.0 && postMs < 400.0);
    CHECK(onThread.load());
    (void)t1;
    // THE PASS'S OWN FIGURES (for the measurement run): every one of those submits started a pass by a wake, and none ran near the period.
    const Engine::PassStats st = e.passStats();
    std::printf("      pass stats: %llu passes, mean %.0f us, longest %.0f us, %llu over the period, %llu started by a wake\n",
                static_cast<unsigned long long>(st.passes), st.passes > 0 ? static_cast<double>(st.sumUs) / static_cast<double>(st.passes) : 0.0,
                static_cast<double>(st.maxUs), static_cast<unsigned long long>(st.overPeriod), static_cast<unsigned long long>(st.wakes));
    CHECK(st.passes >= 20u);
    CHECK(st.wakes >= 20u);
    CHECK(st.overPeriod == 0u);
    CHECK(st.maxUs < 500000u);
    // SUBMIT TO PUBLISH, the engine's own figure (case I has the arithmetic): each of the twenty submits and the post was a wake the pass
    // served, so there are at least twenty lags, all far inside the period, and the percentiles are ordered.
    std::printf("      submit to publish (engine's own): %llu served, p50 <= %llu us, p99 <= %llu us, longest %llu us\n",
                static_cast<unsigned long long>(st.lagCount), static_cast<unsigned long long>(st.lagP50Us),
                static_cast<unsigned long long>(st.lagP99Us), static_cast<unsigned long long>(st.lagMaxUs));
    CHECK(st.lagCount >= 20u);
    CHECK(st.lagP50Us <= st.lagP99Us);
    CHECK(st.lagP50Us < 150000u);
    CHECK(st.lagMaxUs < 400000u);
    CHECK(st.durationP50Us <= st.durationP99Us);
    e.stopControlThread();
}

// I: the pass's own figures ---------------------------------------------------------------------------------------------------
void testPassFigures() {
    std::printf("  I: the histogram's arithmetic, the lag and the window\n");
    using cascade::engine::PassHist;
    // Every value lands in a bucket whose upper edge is at or above it and whose lower neighbour's edge is below it; the edges rise.
    bool ordered = true;
    for (std::uint64_t us : {0ull, 1ull, 3ull, 4ull, 5ull, 7ull, 8ull, 9ull, 10ull, 11ull, 12ull, 99ull, 100ull, 1000ull, 15999ull, 16000ull,
                             1000000ull, 123456789ull}) {
        const int b = PassHist::bucketOf(us);
        if (PassHist::upperEdge(b) < us) { ordered = false; }
        if (b > 0 && PassHist::upperEdge(b - 1) >= us) { ordered = false; }
    }
    CHECK(ordered);
    for (int b = 1; b < PassHist::kBuckets; ++b) { CHECK(PassHist::upperEdge(b) > PassHist::upperEdge(b - 1)); }
    CHECK((PassHist::bucketOf(4)) == (4));
    CHECK((PassHist::upperEdge(4)) == (4u));
    CHECK((PassHist::bucketOf(8)) == (8));
    CHECK((PassHist::upperEdge(8)) == (9u));
    CHECK((PassHist::bucketOf(11)) == (9));
    CHECK((PassHist::upperEdge(9)) == (11u));
    // A percentile is the upper edge of the bucket holding the nearest rank: 99 values of 10 us and one of 5000 us -> p50 and p99 are the
    // 10 us bucket's edge (11), the longest is the 5000 us bucket's; an empty histogram says 0.
    PassHist h;
    for (int i = 0; i < 99; ++i) { h.add(10); }
    h.add(5000);
    std::uint64_t c[PassHist::kBuckets];
    h.copyTo(c);
    CHECK((PassHist::percentile(c, 50.0)) == (11u));
    CHECK((PassHist::percentile(c, 99.0)) == (11u));
    CHECK(PassHist::percentile(c, 100.0) >= 5000u);
    std::uint64_t none[PassHist::kBuckets] = {};
    CHECK((PassHist::percentile(none, 50.0)) == (0u));

    // THE ENGINE'S FIGURES. Marked after a first pass, then thirty submits each followed by its state: the window's JSON counts at least
    // thirty served, carries the marked flag and the window's seconds, and the unmarked cumulative figure is at least the window's.
    Engine e;
    e.initialise();
    e.startControlThread(opts(1500, 2));
    CHECK(e.runPassNow());
    for (int i = 0; i < 5; ++i) {
        const float want = volumeOf(i + 1);
        e.submitCommand(cmd::makeNum(FOXAPI_OP_SET_VOLUME, want));
        CHECK(spinFor([&] { return A::publishedVolume(e) == want; }, 3000) >= 0.0);
    }
    // (the lag is recorded when the pass that published has returned: a moment after the state shows - wait for it)
    CHECK(spinFor([&] { return e.passStats().lagCount >= 5u; }, 3000) >= 0.0);
    const Engine::PassStats before = e.passStats();
    e.markPassStats();
    for (int i = 0; i < 30; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const float want = volumeOf(i + 11);
        e.submitCommand(cmd::makeNum(FOXAPI_OP_SET_VOLUME, want));
        CHECK(spinFor([&] { return A::publishedVolume(e) == want; }, 3000) >= 0.0);
    }
    CHECK(spinFor([&] { return e.passStats().lagCount >= before.lagCount + 30u; }, 3000) >= 0.0);
    const std::string js = e.passStatsJson();
    const Engine::PassStats after = e.passStats();
    std::printf("      the window: %s\n", js.c_str());
    CHECK(js.find("\"marked\": true") != std::string::npos);
    CHECK(js.find("\"submitToPublish\": {\"served\": ") != std::string::npos);
    CHECK(after.lagCount - before.lagCount >= 30u);
    // the JSON's served count is the window's: read it back out of the text
    const std::size_t at = js.find("\"served\": ");
    const unsigned long long served = at == std::string::npos ? 0ull : std::strtoull(js.c_str() + at + 10, nullptr, 10);
    CHECK(served >= 30ull);
    CHECK(served == after.lagCount - before.lagCount);
    // a run with no wake at all (the period ran out, a runPassNow) adds no lag: the lag is for SUBMITS
    const std::uint64_t lagsBefore = e.passStats().lagCount;
    CHECK(e.runPassNow());
    CHECK(e.runPassNow());
    CHECK((e.passStats().lagCount) == (lagsBefore));
    e.stopControlThread();
}

// A2: the period means what it says ----------------------------------------------------------------------------------------------
void testPeriodRate() {
    std::printf("  A2: a 16 ms period is ~62 passes a second, not the timer tick's half of that\n");
    Engine e;
    e.initialise();
    e.startControlThread(opts(16, 2));
    const std::uint64_t p0 = e.passesEnded();
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    const std::uint64_t n = e.passesEnded() - p0;
    std::printf("      no submits for 1.5 s: %llu passes (%.1f a second)\n", static_cast<unsigned long long>(n), static_cast<double>(n) / 1.5);
    CHECK(n >= 60u);       // on a 15.6 ms timer tick a 16 ms wait ends on the second tick and this reads ~48 (35 a second in the smoke)
    CHECK(n <= 130u);
    e.stopControlThread();
}

// B: the floor ---------------------------------------------------------------------------------------------------------------
std::uint64_t floodPasses(int floorMs, int floodMs) {
    Engine e;
    e.initialise();
    e.startControlThread(opts(1000, floorMs));
    (void)e.runPassNow();
    const std::uint64_t before = e.passesEnded();
    std::atomic<bool> go{true};
    std::thread flood([&] {
        int i = 0;
        while (go.load()) { e.submitCommand(cmd::makeNum(FOXAPI_OP_SET_VOLUME, volumeOf(i++))); }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(floodMs));
    go = false;
    flood.join();
    const std::uint64_t passes = e.passesEnded() - before;
    e.stopControlThread();
    return passes;
}

void testFloor() {
    std::printf("  B: the floor turns a flood of submits into a bounded number of passes\n");
    const int floodMs = 1000;
    const std::uint64_t off = floodPasses(0, floodMs);
    const std::uint64_t on = floodPasses(40, floodMs);
    const std::uint64_t dflt = floodPasses(2, floodMs);   // the product's own floor (printed; nothing here depends on the machine's timer)
    CHECK(dflt <= static_cast<std::uint64_t>(floodMs / 2) + 20u);
    std::printf("      the product's floor, 2 ms: %llu passes in the same second (the bound is %d)\n", static_cast<unsigned long long>(dflt), floodMs / 2);
    std::printf("      one second of submits as fast as a thread can make them: floor 0 -> %llu passes, floor 40 ms -> %llu passes (bound %d)\n",
                static_cast<unsigned long long>(off), static_cast<unsigned long long>(on), floodMs / 40);
    CHECK(on <= static_cast<std::uint64_t>(floodMs / 40) + 4u);
    CHECK(on >= 5u);                    // it still runs
    CHECK(off > on * 3u);               // and without the floor it is a flood
    // An idle spell, then a submit: served at once, not after the floor (floor 120 ms; a wake measured from the wake would wait for it).
    Engine e;
    e.initialise();
    e.startControlThread(opts(1500, 120));
    CHECK(e.runPassNow());
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const auto t0 = Clock::now();
    e.submitCommand(cmd::makeNum(FOXAPI_OP_SET_VOLUME, 0.77));
    const double ms = spinFor([&] { return A::publishedVolume(e) == 0.77f; }, 3000);
    std::printf("      after 300 ms idle, floor 120 ms: the submit is shown after %.1f ms\n", ms);
    CHECK(ms >= 0.0 && ms < 100.0);
    (void)t0;
    e.stopControlThread();
}

// C: runPassNow --------------------------------------------------------------------------------------------------------------
void testRunPassNow() {
    std::printf("  C: runPassNow - a whole pass that started after the call\n");
    Engine e;
    e.initialise();
    // No thread: inline.
    e.submitCommand(cmd::makeNum(FOXAPI_OP_SET_VOLUME, 0.31));
    CHECK(e.runPassNow());
    CHECK(A::publishedVolume(e) == 0.31f);
    e.startControlThread(opts(40, 2));
    int bad = 0;
    for (int i = 0; i < 60; ++i) {
        const float want = volumeOf(i + 7);
        e.submitCommand(cmd::makeNum(FOXAPI_OP_SET_VOLUME, want));
        const std::uint64_t begunBefore = e.passesBegun();
        const bool ok = e.runPassNow();
        if (!ok || A::publishedVolume(e) != want || e.passesBegun() <= begunBefore || e.passesEnded() < begunBefore + 1u) { ++bad; }
    }
    std::printf("      60 submit+runPassNow rounds: %d where the state did not show the submit on return\n", bad);
    CHECK(bad == 0);
    // A stall: the pass the call waits for includes it.
    A::stall(e, 150);
    e.submitCommand(cmd::makeNum(FOXAPI_OP_SET_VOLUME, 0.42));
    const auto t0 = Clock::now();
    CHECK(e.runPassNow());
    const double stallMs = msSince(t0);
    std::printf("      with the next pass stalled 150 ms: returned after %.0f ms with the state showing it\n", stallMs);
    CHECK(stallMs >= 140.0);
    CHECK(A::publishedVolume(e) == 0.42f);
    // Bounded: a pass that cannot come.
    A::park(e, true);
    const auto t1 = Clock::now();
    CHECK(!e.runPassNow(std::chrono::milliseconds(80)));
    const double boundMs = msSince(t1);
    std::printf("      the thread parked: runPassNow(80 ms) gave up after %.0f ms\n", boundMs);
    CHECK(boundMs >= 70.0 && boundMs < 400.0);
    A::park(e, false);
    CHECK(e.runPassNow());
    // From the control thread itself: refused, not deadlocked.
    std::atomic<int> fromPass{-1};
    e.postToPass([&] { fromPass = e.runPassNow() ? 1 : 0; });
    CHECK(waitFor([&] { return fromPass.load() != -1; }, 3000) >= 0.0);
    CHECK(fromPass.load() == 0);
    // Stepped: the three parts, and the counters.
    e.stopControlThread();
    Engine s;
    s.initialise();
    s.startControlThread(opts(10, 2, true));
    const std::uint64_t b0 = s.passesBegun();
    CHECK(s.runPassNow());
    CHECK(s.passesBegun() == b0 + 1u && s.passesEnded() == b0 + 1u);
    s.stopControlThread();
}

// D: stepped -----------------------------------------------------------------------------------------------------------------
void testStepped() {
    std::printf("  D: stepped - no pass of its own, the three parts when asked\n");
    Engine e;
    e.initialise();
    e.startControlThread(opts(5, 1, /*stepped=*/true));
    CHECK(e.controlStepped());
    CHECK(e.controlThreadRunning());
    // Nothing runs by itself: not for the period, not for a wake.
    const std::uint64_t p0 = A::passes(e);
    e.submitCommand(cmd::makeNum(FOXAPI_OP_SET_VOLUME, 0.66));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    std::printf("      300 ms with a submit pending and a 5 ms period: %llu passes, %llu begun\n",
                static_cast<unsigned long long>(A::passes(e) - p0), static_cast<unsigned long long>(e.passesBegun()));
    CHECK(A::passes(e) == p0);
    CHECK(e.passesBegun() == 0u);
    CHECK(A::publishedVolume(e) != 0.66f);
    // THE FRONT PART applies the command and publishes it; the config copy (the back part's) has not moved.
    std::atomic<bool> onThread{false};
    e.postToPass([&] { onThread = e.onControlThread(); });
    e.stepPass(PassPart::Front);
    CHECK(onThread.load());                                        // the work posted ran inside the front part, on the thread
    CHECK(e.passesBegun() == 1u && e.passesEnded() == 0u);
    CHECK(A::publishedVolume(e) == 0.66f);
    const float cfgAfterFront = e.configSnapshot().volume;
    CHECK(cfgAfterFront != 0.66f);
    e.stepPass(PassPart::Mute);
    CHECK(e.passesEnded() == 0u);
    CHECK(e.configSnapshot().volume != 0.66f);
    // THE BACK PART copies the config and ends the pass.
    e.stepPass(PassPart::Back);
    CHECK(e.passesEnded() == 1u);
    CHECK(e.configSnapshot().volume == 0.66f);
    std::printf("      front: state shows the submit, config copy not yet (%.2f); back: config copy %.2f; passes %llu\n",
                static_cast<double>(cfgAfterFront), static_cast<double>(e.configSnapshot().volume),
                static_cast<unsigned long long>(e.passesEnded()));
    // Many rounds: the parts are strictly serial.
    for (int i = 0; i < 200; ++i) {
        e.submitCommand(cmd::makeNum(FOXAPI_OP_SET_VOLUME, volumeOf(i + 3)));
        e.stepPass(PassPart::Front);
        CHECK(A::publishedVolume(e) == volumeOf(i + 3));
        e.stepPass(PassPart::Mute);
        e.stepPass(PassPart::Back);
    }
    CHECK(e.passesBegun() == e.passesEnded());
    e.stopControlThread();
    // A stepPass on an engine with no thread runs the part inline, on this thread.
    Engine n;
    n.initialise();
    n.submitCommand(cmd::makeNum(FOXAPI_OP_SET_VOLUME, 0.12));
    n.stepPass(PassPart::Front);
    CHECK(A::publishedVolume(n) == 0.12f);
    // ...and on a FREE-RUNNING thread it does nothing at all.
    Engine f;
    f.initialise();
    f.startControlThread(opts(500, 2));
    (void)f.runPassNow();
    f.submitCommand(cmd::makeNum(FOXAPI_OP_SET_VOLUME, 0.13));
    const std::uint64_t begun = f.passesBegun();
    f.stepPass(PassPart::Front);
    CHECK(f.passesBegun() <= begun + 1u);   // (a wake may have started the pass the submit asked for; stepPass itself ran none)
    f.stopControlThread();
}

// D2: a stop in the middle of a step, then a restart ---------------------------------------------------------------------------
void testSteppedRestart() {
    std::printf("  D2: a stepped thread stopped in the middle of a step, then started again\n");
    Engine e;
    e.initialise();
    e.startControlThread(opts(5, 1, /*stepped=*/true));
    A::stall(e, 300);                                      // the front part sleeps 300 ms before it runs
    std::atomic<bool> returned{false};
    std::thread stepper([&] {
        e.stepPass(PassPart::Front);
        returned = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    CHECK(!returned.load());
    e.stopControlThread();                                 // the thread ends without running the part; the stepper is let go
    stepper.join();
    CHECK(returned.load());
    e.startControlThread(opts(5, 1, /*stepped=*/true));
    const auto t0 = Clock::now();
    e.submitCommand(cmd::makeNum(FOXAPI_OP_SET_VOLUME, 0.58));
    e.stepPass(PassPart::Front);                           // would wait out its 60 s bound if the old request still counted
    const double ms = msSince(t0);
    std::printf("      the first step after the restart took %.1f ms and the state shows the submit: %d\n", ms, A::publishedVolume(e) == 0.58f ? 1 : 0);
    CHECK(ms < 1000.0);
    CHECK(A::publishedVolume(e) == 0.58f);
    e.stopControlThread();
}

// D3: the front end's watchdog beat while it waits ----------------------------------------------------------------------------
void testWaitBeat() {
    std::printf("  D3: a front end blocked in stepPass beats its watchdog from the wait\n");
    Engine e;
    e.initialise();
    e.startControlThread(opts(5, 1, /*stepped=*/true));
    std::atomic<int> beats{0};
    e.setFrontEndWaitBeat([&] { ++beats; });
    A::stall(e, 150);
    const auto t0 = Clock::now();
    e.stepPass(PassPart::Front);
    const double ms = msSince(t0);
    std::printf("      a front part that took %.0f ms: %d beat(s) from the waiting thread (every %d ms)\n", ms, beats.load(),
                static_cast<int>(Engine::kPassWaitNet.count()));
    CHECK(ms >= 140.0);
    CHECK(beats.load() >= 3);
    e.setFrontEndWaitBeat({});
    e.stepPass(PassPart::Mute);
    e.stepPass(PassPart::Back);
    e.stopControlThread();
}

// E: work posted to the pass -------------------------------------------------------------------------------------------------
void testPostedWork() {
    std::printf("  E: postToPass and runOnPass\n");
    {
        Engine e;
        e.initialise();
        e.startControlThread(opts(500, 2));
        (void)e.runPassNow();
        std::mutex m;
        std::vector<int> order;
        std::atomic<int> wrongThread{0};
        for (int i = 0; i < 50; ++i) {
            e.postToPass([&, i] {
                if (!e.onControlThread()) { ++wrongThread; }
                std::lock_guard<std::mutex> lk(m);
                order.push_back(i);
            });
        }
        CHECK(e.runPassNow());
        {
            std::lock_guard<std::mutex> lk(m);
            CHECK(order.size() == 50u);
            bool inOrder = true;
            for (std::size_t i = 0; i < order.size(); ++i) { inOrder = inOrder && order[i] == static_cast<int>(i); }
            CHECK(inOrder);
        }
        CHECK(wrongThread.load() == 0);
        // runOnPass, free-running: on the control thread, done when it returns.
        bool onPass = false;
        int value = 0;
        CHECK(e.runOnPass([&] {
            onPass = e.onControlThread();
            value = 17;
        }));
        CHECK(onPass);
        CHECK(value == 17);
        std::printf("      free-running: 50 posts ran in order on the control thread; runOnPass ran there and was done on return\n");
        e.stopControlThread();
    }
    {
        // Stepped (and no thread): the caller's own thread, the pass out of the way.
        Engine e;
        e.initialise();
        e.startControlThread(opts(500, 2, true));
        bool onCaller = false;
        CHECK(e.runOnPass([&] { onCaller = !e.onControlThread(); }));
        CHECK(onCaller);
        e.stopControlThread();
        bool inline_ = false;
        CHECK(e.runOnPass([&] { inline_ = true; }));
        CHECK(inline_);
        std::printf("      stepped and with no thread: runOnPass ran on the caller's thread\n");
    }
    {
        // NEVER AFTER IT HAS RETURNED: the thread is held, the caller waits, the thread is told to stop - the work does not run.
        Engine e;
        e.initialise();
        e.startControlThread(opts(500, 2));
        (void)e.runPassNow();
        A::park(e, true);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        std::atomic<int> ran{0};
        std::atomic<int> result{-1};
        std::thread caller([&] { result = e.runOnPass([&] { ++ran; }) ? 1 : 0; });
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        CHECK(result.load() == -1);                  // still waiting
        e.stopControlThread();                       // un-parks, and the loop ends before another pass
        caller.join();
        std::printf("      the thread stopped while the caller waited: runOnPass returned %d, the work ran %d times\n", result.load(),
                    ran.load());
        CHECK(result.load() == 0);
        CHECK(ran.load() == 0);
    }
}

// F: the proxy apply ---------------------------------------------------------------------------------------------------------
std::atomic<int> g_unmaps{0};
void unmapHook() { ++g_unmaps; }

void testProxyApply() {
    std::printf("  F: a front end blocked in stepPass answers the unload handshake\n");
    A::setPluginDir(&pluginDir);
    A::setUnmapHook(&unmapHook);
    Engine e;
    e.initialise();
    std::atomic<int> applied{0};
    std::atomic<int> appliedOnThisThread{0};
    const std::thread::id me = std::this_thread::get_id();
    // This thread plays the window: it installs the applier (and so is "the front end's thread").
    e.setPluginUiApplier([&](const cascade::core::PluginDirective&) {
        ++applied;
        if (std::this_thread::get_id() == me) { ++appliedOnThisThread; }
    });
    e.startControlThread(opts(500, 2, /*stepped=*/true));
    g_unmaps = 0;
    e.submitCommand(cmd::makeText(FOXAPI_OP_PLUGIN_RESCAN, std::string()).c);
    const auto t0 = Clock::now();
    e.stepPass(PassPart::Front);                 // the pass rescans: it releases the plugin UI and waits for us to say we did
    const double ms = msSince(t0);
    std::printf("      the front part with a rescan in it: %.1f ms (the handshake's bound is %d ms); the applier ran %d times, %d of them here; unmaps %d\n",
                ms, static_cast<int>(Engine::kPluginUiHandshake.count()), applied.load(), appliedOnThisThread.load(), g_unmaps.load());
    CHECK(ms < 150.0);
    CHECK(applied.load() >= 1);
    CHECK(appliedOnThisThread.load() == applied.load());    // every apply was made by the thread that was waiting
    CHECK(g_unmaps.load() == 1);                            // the release was acknowledged, so the modules were unmapped
    e.stepPass(PassPart::Mute);
    e.stepPass(PassPart::Back);
    // The same in a runPassNow.
    e.submitCommand(cmd::makeText(FOXAPI_OP_PLUGIN_RESCAN, std::string()).c);
    const auto t1 = Clock::now();
    CHECK(e.runPassNow());
    std::printf("      the same inside runPassNow: %.1f ms; unmaps %d\n", msSince(t1), g_unmaps.load());
    CHECK(msSince(t1) < 150.0);
    CHECK(g_unmaps.load() == 2);
    e.stopControlThread();
    e.setPluginUiApplier({});
    A::setUnmapHook(nullptr);
}

// G: the clock ---------------------------------------------------------------------------------------------------------------
void testClock() {
    std::printf("  G: the engine's own clock\n");
    Engine e;
    e.initialise();
    const double t0 = e.clockS();
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    const double t1 = e.clockS();
    CHECK(t1 - t0 >= 0.050 && t1 - t0 < 5.0);
    double other = -1.0;
    std::thread([&] { other = e.clockS(); }).join();
    CHECK(other >= t1);
    std::printf("      0 -> %.3f s in 60 ms, readable from another thread, no front end\n", t1 - t0);
}

// H: the watchdog ------------------------------------------------------------------------------------------------------------
unsigned hangFiles(const std::filesystem::path& dir) {
    unsigned n = 0;
    std::error_code ec;
    for (const auto& f : std::filesystem::directory_iterator(dir, ec)) {
        if (f.path().filename().string().rfind("hang-", 0) == 0) { ++n; }
    }
    return n;
}

void testWatchdog() {
    std::printf("  H: the control watchdog\n");
    const std::filesystem::path dir = g_scratch / "hangs";
    std::filesystem::create_directories(dir);
    {
        // The switch: a stalled pass writes a report while the directory is set, and none once the front end has cleared it.
        Engine e;
        e.initialise();
        ControlThreadOptions o = opts(5, 1);
        o.watchdogReportDir = dir.string();
        o.watchdogThresholdMs = 300;
        e.startControlThread(o);
        CHECK(A::watchdogDir(e) == dir.string());
        CHECK(e.runPassNow());
        A::stall(e, 1400);
        e.submitCommand(cmd::makeNum(FOXAPI_OP_SET_VOLUME, 0.5));
        std::this_thread::sleep_for(std::chrono::milliseconds(1700));
        const unsigned withDir = hangFiles(dir);
        std::printf("      a pass stalled 1.4 s against a 300 ms threshold, directory set: %u report file(s)\n", withDir);
        CHECK(withDir == 1u);
        e.setControlWatchdogReportDir(std::string());
        CHECK(A::watchdogDir(e).empty());
        A::stall(e, 1400);
        e.submitCommand(cmd::makeNum(FOXAPI_OP_SET_VOLUME, 0.51));
        std::this_thread::sleep_for(std::chrono::milliseconds(1700));
        const unsigned afterOff = hangFiles(dir);
        std::printf("      the same stall with the directory cleared (the diagnostics switch off): %u report file(s) in all\n", afterOff);
        CHECK(afterOff == withDir);
        e.stopControlThread();
    }
    {
        // A stepped thread that idles while its front end draws is not a hung pass.
        Engine e;
        e.initialise();
        ControlThreadOptions o = opts(5, 1, true);
        o.watchdogReportDir = dir.string();
        o.watchdogThresholdMs = 700;
        const unsigned before = hangFiles(dir);
        e.startControlThread(o);
        e.stepPass(PassPart::Front);
        e.stepPass(PassPart::Mute);
        e.stepPass(PassPart::Back);
        std::this_thread::sleep_for(std::chrono::milliseconds(2200));   // the front end draws for 3x the threshold
        e.stepPass(PassPart::Front);
        e.stepPass(PassPart::Mute);
        e.stepPass(PassPart::Back);
        const unsigned after = hangFiles(dir);
        std::printf("      stepped, idle 2.2 s against a 700 ms threshold: %u new report file(s)\n", after - before);
        CHECK(after == before);
        e.stopControlThread();
    }
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_pass_control\n");
    isolate();
    testWake();
    testPassFigures();
    testPeriodRate();
    testFloor();
    testRunPassNow();
    testStepped();
    testSteppedRestart();
    testWaitBeat();
    testPostedWork();
    testProxyApply();
    testClock();
    testWatchdog();
    std::error_code ec;
    std::filesystem::remove_all(g_scratch, ec);
    return testSummary("test_pass_control");
}
