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
//   K  THE FRAME'S KICK (docs/engine-stage4.md 6.9, the capped-tail cure): a window's frame wakes the pass (Engine::frameKick), so while
//      frames flow the pass runs once a frame and its timer never fires; the timer is the fallback when they stop. K1 frames pace the pass
//      (a pass a kick, started within a millisecond of it, none by the timer - and the control: with the grace extension off the 16 ms timer
//      fires first, every frame); K2 the frames stop and the timer takes over within the grace, then holds the period; K3 the 2 ms floor holds
//      against kicks as fast as a thread can make them (and the control: floor off is a flood); K4 the kick gap drops what a pass cannot
//      use; K5 a kick is not a submit (no lag stamped), a kicked pass drains the remote controls, and a headless / stepped / absent
//      engine is untouched
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
    static void passStarting(void (*f)()) { Engine::testHooks_.passStarting = f; }
    static bool hasPassTimer(Engine& e) { return e.passTimer_.load() != nullptr && e.passWakeEvent_.load() != nullptr; }
    static unsigned timerResolutionRequested(Engine& e) { return e.timerResolutionApplied_; }
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

// A3: the pass keeps its own clock -------------------------------------------------------------------------------------------------
// The control loop waits on a high-resolution waitable timer and an event, and asks the process for NO finer system tick (the capped-tail fix:
// a process-wide 1 ms request made the window's frame-cap wait overshoot by ~2 ms, 12x as often; and Windows 11 does not honour the request
// for a minimized or occluded window at all). Elsewhere the loop is the condition variable's and there is nothing to ask.
void testOwnClock() {
    std::printf("  A3: the pass waits on its own timer and requests no process timer resolution\n");
    Engine e;
    e.initialise();
    e.startControlThread(opts(16, 2));
#if defined(_WIN32)
    CHECK(A::hasPassTimer(e));
    CHECK(A::timerResolutionRequested(e) == 0u);
    std::printf("      high-resolution timer + wake event made: %d; timeBeginPeriod requested: %u ms\n", A::hasPassTimer(e) ? 1 : 0,
                A::timerResolutionRequested(e));
#endif
    CHECK(e.runPassNow());
    e.stopControlThread();
    // A restart keeps working on the same handles (they live as long as the engine).
    e.startControlThread(opts(16, 2));
    CHECK(e.runPassNow());
    e.stopControlThread();
}

// A4: the pass's start times, from the loop's own hook ------------------------------------------------------------------------------
std::mutex g_startsMutex;
std::vector<Clock::time_point> g_starts;
void recordPassStart() {
    const auto now = Clock::now();
    std::lock_guard<std::mutex> lk(g_startsMutex);
    g_starts.push_back(now);
}
std::vector<Clock::time_point> takeStarts() {
    std::lock_guard<std::mutex> lk(g_startsMutex);
    std::vector<Clock::time_point> v;
    v.swap(g_starts);
    return v;
}

// The start-to-start gaps of the passes two submits make (see testFloorBetweenStarts), on an engine already running (period 1500 ms, floor 2 ms).
std::vector<double> floorGaps(Engine& e, int pairs) {
    A::passStarting(&recordPassStart);
    (void)takeStarts();
    std::vector<double> gaps;
    for (int i = 0; i < pairs; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));      // idle: the loop is in its period wait, past any floor
        (void)takeStarts();
        e.submitCommand(cmd::makeNum(FOXAPI_OP_SET_VOLUME, volumeOf(2 * i)));
        // The second submit as soon as the first one's pass has begun (both made before it would fold into ONE pass): its wake is then
        // inside the floor of the pass that is running.
        for (int spin = 0; spin < 3000000; ++spin) {
            std::lock_guard<std::mutex> lk(g_startsMutex);
            if (!g_starts.empty()) { break; }
        }
        e.submitCommand(cmd::makeNum(FOXAPI_OP_SET_VOLUME, volumeOf(2 * i + 1)));
        const float want = volumeOf(2 * i + 1);
        (void)spinFor([&] { return A::publishedVolume(e) == want; }, 3000);
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
        const auto s = takeStarts();
        for (std::size_t k = 1; k < s.size(); ++k) {
            gaps.push_back(std::chrono::duration<double, std::milli>(s[k] - s[k - 1]).count());
        }
    }
    A::passStarting(nullptr);
    std::sort(gaps.begin(), gaps.end());
    return gaps;
}

// The lag from a submit to the start of the pass it wakes, on an engine already running (period 1500 ms: only the wake can start it).
std::vector<double> wakeLags(Engine& e, int submits) {
    A::passStarting(&recordPassStart);
    std::vector<double> lags;
    for (int i = 0; i < submits; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
        (void)takeStarts();
        const auto t0 = Clock::now();
        e.submitCommand(cmd::makeNum(FOXAPI_OP_SET_VOLUME, volumeOf(i)));
        double ms = -1.0;
        for (int spin = 0; spin < 3000000 && ms < 0.0; ++spin) {
            std::lock_guard<std::mutex> lk(g_startsMutex);
            if (!g_starts.empty()) { ms = std::chrono::duration<double, std::milli>(g_starts.front() - t0).count(); }
        }
        CHECK(ms >= 0.0);
        if (ms >= 0.0) { lags.push_back(ms); }
    }
    A::passStarting(nullptr);
    std::sort(lags.begin(), lags.end());
    return lags;
}

void checkFloorGaps(const std::vector<double>& gaps) {
    CHECK(gaps.size() >= 36u);   // nearly every pair made two passes (the second submit landed after the first pass had begun)
    if (!gaps.empty()) {
        std::printf("      %zu pairs that made two passes: start-to-start min %.3f ms, median %.3f ms, max %.3f ms (floor 2 ms)\n", gaps.size(),
                    gaps.front(), median(gaps), gaps.back());
        CHECK(gaps.front() >= 2.0);
        CHECK(median(gaps) < 4.0);    // it is the floor that spaces them, not a coarse tick (~15 ms) after it
    }
}

void checkWakeLags(const std::vector<double>& lags) {
    CHECK(lags.size() >= 50u);
    if (!lags.empty()) {
        std::printf("      submit to pass start, %zu submits: median %.3f ms, p90 %.3f ms, max %.3f ms\n", lags.size(), median(lags),
                    lags[lags.size() * 9 / 10], lags.back());
        CHECK(median(lags) < 1.0);
    }
}

// THE FLOOR on the precise timer: two submits no time apart start two passes at least `floor` apart (a coarse wait would make the 2 ms floor
// ~15 ms; a missing one would make the second pass follow the first at once).
void testFloorBetweenStarts() {
    std::printf("  A4: two submits no time apart: the passes they start are at least the 2 ms floor apart\n");
    Engine e;
    e.initialise();
    e.startControlThread(opts(1500, 2));
    CHECK(e.runPassNow());
    const std::vector<double> gaps = floorGaps(e, 40);
    e.stopControlThread();
    checkFloorGaps(gaps);
}

// THE WAKE: the lag from a submit to the start of the pass it wakes (the event, not the 1500 ms period or a timer tick).
void testWakeToStart() {
    std::printf("  A5: a submit starts a pass in well under a millisecond\n");
    Engine e;
    e.initialise();
    e.startControlThread(opts(1500, 2));
    CHECK(e.runPassNow());
    const std::vector<double> lags = wakeLags(e, 60);
    e.stopControlThread();
    checkWakeLags(lags);
}

// A6: THE MINIMIZED CASE. Windows 11 does not honour a process's timer-resolution request when its window is minimized, occluded or
// otherwise invisible (the documented remark of timeBeginPeriod): the system default applies to it, 15.6 ms. The OS's own switch for that
// is the process power-throttling policy IGNORE_TIMER_RESOLUTION; this test turns it ON for the test process, which is exactly the
// state of a hidden window, and holds the pass to its contract in it: the period (a 16 ms wait ending on the second 15.6 ms tick reads
// 35 a second), the 2 ms floor (~15 ms on the tick) and the wake. A pass waiting on its own high-resolution timer does not depend on the
// resolution at all; the old design (timeBeginPeriod(1) and a condition variable's timeout) fails the first two here.
void testMinimizedImmunity() {
    std::printf("  A6: the pass keeps its rate, its floor and its wake while the OS ignores the process's timer-resolution request\n");
#if defined(_WIN32)
    struct ThrottleState { ULONG Version, ControlMask, StateMask; };
    constexpr ULONG kIgnoreTimerResolution = 0x4;   // PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION
    constexpr int kProcessPowerThrottling = 4;      // ProcessPowerThrottling
    auto setIgnore = [&](bool on) {
        ThrottleState st{1, kIgnoreTimerResolution, on ? kIgnoreTimerResolution : 0u};
        return ::SetProcessInformation(::GetCurrentProcess(), static_cast<PROCESS_INFORMATION_CLASS>(kProcessPowerThrottling), &st, sizeof(st)) != FALSE;
    };
    if (!setIgnore(true)) {
        std::printf("      SKIPPED: this Windows has no per-process timer-resolution throttle (error %lu)\n", static_cast<unsigned long>(::GetLastError()));
        return;
    }
    {
        Engine e;
        e.initialise();
        e.startControlThread(opts(16, 2));
        const std::uint64_t p0 = e.passesEnded();
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        const std::uint64_t n = e.passesEnded() - p0;
        std::printf("      no submits for 1.5 s: %llu passes (%.1f a second)\n", static_cast<unsigned long long>(n), static_cast<double>(n) / 1.5);
        CHECK(n >= 80u);    // 53 a second or more; the throttled old design reads ~35 a second (52 here)
        e.stopControlThread();
    }
    {
        Engine e;
        e.initialise();
        e.startControlThread(opts(1500, 2));
        CHECK(e.runPassNow());
        const std::vector<double> gaps = floorGaps(e, 40);
        const std::vector<double> lags = wakeLags(e, 60);
        e.stopControlThread();
        checkFloorGaps(gaps);
        checkWakeLags(lags);
    }
    (void)setIgnore(false);
#else
    std::printf("      SKIPPED: Windows only\n");
#endif
}

// K: the frame's kick ------------------------------------------------------------------------------------------------------------
// A stand-in for a window's frame loop: calls frameKick() every `intervalMs` (0: as fast as it can), timed by SPINNING (a sleep on Windows is a
// 15.6 ms tick), and keeps each call's time so a pass's start can be paired with the kick that made it.
struct Kicker {
    Engine& e;
    std::atomic<bool> go{true};
    std::vector<Clock::time_point> times;   // the kicker's alone until stop() has joined it
    std::thread t;
    Kicker(Engine& eng, double intervalMs) : e(eng) {
        times.reserve(200000);
        t = std::thread([this, intervalMs] {
            auto next = Clock::now();
            while (go.load()) {
                if (intervalMs > 0.0) {
                    while (go.load() && Clock::now() < next) { std::this_thread::yield(); }
                    next += std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double, std::milli>(intervalMs));
                    if (!go.load()) { break; }
                }
                if (times.size() < times.capacity()) { times.push_back(Clock::now()); }
                e.frameKick();
            }
        });
    }
    void stop() {
        go = false;
        if (t.joinable()) { t.join(); }
    }
    ~Kicker() { stop(); }
};

ControlThreadOptions kickOpts(int periodMs, int floorMs, int graceMs, int gapMs) {
    ControlThreadOptions o = opts(periodMs, floorMs);
    o.kickGrace = std::chrono::milliseconds(graceMs);
    o.kickGap = std::chrono::milliseconds(gapMs);
    return o;
}

double msBetween(Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); }

// For each kick, the time to the first pass start at or after it (a kick with another kick before that start was dropped, and is skipped).
std::vector<double> kickToStart(const std::vector<Clock::time_point>& kicks, const std::vector<Clock::time_point>& starts) {
    std::vector<double> lags;
    std::size_t si = 0;
    for (std::size_t ki = 0; ki < kicks.size(); ++ki) {
        while (si < starts.size() && starts[si] < kicks[ki]) { ++si; }
        if (si >= starts.size()) { break; }
        if (ki + 1 < kicks.size() && starts[si] > kicks[ki + 1]) { continue; }
        lags.push_back(msBetween(kicks[ki], starts[si]));
    }
    std::sort(lags.begin(), lags.end());
    return lags;
}

Engine::PassStats statsDiff(const Engine::PassStats& a, const Engine::PassStats& b) {
    Engine::PassStats d;
    d.passes = a.passes - b.passes;
    d.wakes = a.wakes - b.wakes;
    d.lagCount = a.lagCount - b.lagCount;
    d.kicks = a.kicks - b.kicks;
    d.kicksDropped = a.kicksDropped - b.kicksDropped;
    d.kickedPasses = a.kickedPasses - b.kickedPasses;
    d.timedPasses = a.timedPasses - b.timedPasses;
    return d;
}

// K1: 60 Hz frames pace the pass. period 16 ms, the grace 24: the frames come every 16.667 ms - LONGER than the period, so a pass paced by its
// own 16 ms timer would start just before every kick; paced by the kicks it starts at them and the timer never fires.
//
// THE FLAKE THIS HAD (6.9, "K1's flake"): the first kick lands while the loop is still on its plain 16 ms timer and is DROPPED by the gap
// (it is within 8 ms of the timer's pass); the wait that was already running kept its old, unextended deadline, so the timer fired again just
// before the next kick, which was dropped again, and so on for as many frames as the two phases took to drift apart (up to 17 timer passes
// of 121, a third of solo runs failing). That was the ENGINE's, not the sampling's: the kicker was on time (longest gap between kicks 16.7-17.2
// ms) in every failing run. The engine now looks again when the timer runs out (controlLoop), and this case holds it: at most the one timer
// pass that was already due when the first kick came.
//
// THE RERUN GUARD (the house pattern of test_engine_api_merge): a KICKER that itself stalls past the grace (a gap between its kicks over 24 ms
// - a desk that took its core away) hands the timer the right to fire, so that attempt says nothing about the pacing and is run again, up to
// three times, loudly. A run whose kicker was on time is never excused: its counts are held.
void testKickPaces() {
    std::printf("  K1: a frame every 16.667 ms paces the pass: one pass a kick, started by the kick, none by the timer\n");
    const int framesMs = 2000;
    for (const bool graceOn : {true, false}) {
        for (int attempt = 0; attempt < 3; ++attempt) {
            Engine e;
            e.initialise();
            A::passStarting(&recordPassStart);   // (set before the thread exists and cleared after it is joined: the hook is a plain pointer the loop reads)
            e.startControlThread(kickOpts(16, 2, graceOn ? 24 : 0, 8));
            CHECK(e.runPassNow());
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
            const Engine::PassStats before = e.passStats();
            std::size_t nKicks = 0;
            std::vector<Clock::time_point> kickTimes;
            {
                Kicker k(e, 1000.0 / 60.0);
                std::this_thread::sleep_for(std::chrono::milliseconds(framesMs));
                k.stop();
                nKicks = k.times.size();
                kickTimes = k.times;
            }
            const Engine::PassStats d = statsDiff(e.passStats(), before);
            e.stopControlThread();
            A::passStarting(nullptr);
            const std::vector<Clock::time_point> starts = takeStarts();
            const double perSecond = static_cast<double>(d.passes) / (framesMs / 1000.0);
            double longest = 0.0;
            for (std::size_t i = 1; i < kickTimes.size(); ++i) { longest = std::max(longest, msBetween(kickTimes[i - 1], kickTimes[i])); }
            std::printf("      grace %s: %zu kicks, %llu passes (%.1f a second), %llu started by a kick, %llu by the timer, %llu kicks dropped; the kicker's longest gap %.2f ms\n",
                        graceOn ? "24 ms" : "OFF  ", nKicks, static_cast<unsigned long long>(d.passes), perSecond,
                        static_cast<unsigned long long>(d.kickedPasses), static_cast<unsigned long long>(d.timedPasses),
                        static_cast<unsigned long long>(d.kicksDropped), longest);
            if (graceOn && longest > 24.0 && attempt < 2) {
                std::printf("      RERUN: the kicker stalled past the grace (%.2f ms), so the timer was entitled to fire; this attempt says nothing\n", longest);
                continue;
            }
            if (graceOn) {
                CHECK(longest <= 24.0);                                 // (three stalled attempts in a row is a desk that cannot run this case)
                CHECK(nKicks >= 100u);
                CHECK(d.kicks >= nKicks * 95u / 100u);                  // the gap drops nothing here: a pass starts at each kick, 16 ms before the next
                CHECK(d.kickedPasses >= nKicks * 95u / 100u);           // a pass a kick
                CHECK(d.timedPasses * 20u <= d.passes);                 // and the timer started at most one in twenty
                CHECK(d.timedPasses <= 2u);                             // in fact the one that was already due when the first kick came
                CHECK(d.passes <= nKicks + 6u);                         // nothing else started a pass
                CHECK(perSecond >= 55.0 && perSecond <= 65.0);          // ~60 passes a second
                const std::vector<double> lags = kickToStart(kickTimes, starts);
                CHECK(lags.size() >= nKicks * 9u / 10u);
                if (!lags.empty()) {
                    std::printf("      kick to the start of its pass, %zu kicks: median %.3f ms, p90 %.3f ms, max %.3f ms\n", lags.size(), median(lags),
                                lags[lags.size() * 9 / 10], lags.back());
                    CHECK(median(lags) < 1.0);
                }
            } else {
                // The control, and what the grace is for: without it the 16 ms timer fires before every 16.667 ms kick, and the kick lands inside
                // the gap of the pass the timer just started (dropped) - the pass is timer-paced, exactly the wake the cure removes.
                CHECK(d.timedPasses * 10u >= d.passes * 8u);
            }
            break;
        }
    }
}

// K2: the frames stop. The last kick starts a pass; the next is the timer's, at most the grace after the kick; then the period again.
void testKickFallback() {
    std::printf("  K2: the frames stop: the timer takes over within the grace, then holds the 16 ms period\n");
    Engine e;
    e.initialise();
    A::passStarting(&recordPassStart);
    e.startControlThread(kickOpts(16, 2, 24, 8));
    CHECK(e.runPassNow());
    Clock::time_point lastKick{};
    {
        Kicker k(e, 1000.0 / 60.0);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        k.stop();
        CHECK(!k.times.empty());
        if (!k.times.empty()) { lastKick = k.times.back(); }
    }
    const Engine::PassStats before = e.passStats();
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    const Engine::PassStats d = statsDiff(e.passStats(), before);
    e.stopControlThread();
    A::passStarting(nullptr);
    const std::vector<Clock::time_point> starts = takeStarts();
    // The starts after the last kick: the pass that kick itself started (within 0.2 ms of it, left out), then the fallback's.
    std::vector<double> after;
    for (const Clock::time_point t : starts) {
        if (msBetween(lastKick, t) > 5.0) { after.push_back(msBetween(lastKick, t)); }
    }
    CHECK(after.size() >= 2u);
    if (after.size() >= 2u) {
        std::vector<double> gaps;
        for (std::size_t i = 1; i < after.size(); ++i) { gaps.push_back(after[i] - after[i - 1]); }
        std::sort(gaps.begin(), gaps.end());
        std::printf("      last kick -> the first pass the timer starts: %.2f ms (grace 24); then %zu passes in 1.5 s (%.1f a second), gap median %.2f ms\n",
                    after[0], after.size(), static_cast<double>(after.size()) / 1.5, median(gaps));
        CHECK(after[0] >= 16.0);             // not before the period, and ...
        CHECK(after[0] <= 24.0 + 12.0);      // ... not later than the grace (a high-resolution timer's lateness and a busy desk's margin)
        CHECK(after.size() >= 80u);          // ~62 a second in the 1.5 s, the same as a headless engine's (A2)
        CHECK(median(gaps) >= 15.5 && median(gaps) <= 17.5);
    }
    std::printf("      in that 1.5 s: %llu passes, %llu started by the timer, %llu by a kick\n", static_cast<unsigned long long>(d.passes),
                static_cast<unsigned long long>(d.timedPasses), static_cast<unsigned long long>(d.kickedPasses));
    CHECK(d.timedPasses + 2u >= d.passes);
    CHECK(d.kickedPasses == 0u);
}

// K3: the floor holds against kicks as fast as a thread can make them.
std::uint64_t kickFloodPasses(int floorMs, int floodMs, std::vector<double>* gapsOut = nullptr) {
    Engine e;
    e.initialise();
    A::passStarting(&recordPassStart);
    e.startControlThread(kickOpts(1000, floorMs, 24, 0));   // gap off: only the floor bounds the kicks
    (void)e.runPassNow();   // (exempt from the floor: its start is not one of the gaps)
    (void)takeStarts();
    const std::uint64_t before = e.passesEnded();
    {
        Kicker k(e, 0.0);
        std::this_thread::sleep_for(std::chrono::milliseconds(floodMs));
        k.stop();
    }
    const std::uint64_t passes = e.passesEnded() - before;
    e.stopControlThread();
    A::passStarting(nullptr);
    const std::vector<Clock::time_point> starts = takeStarts();
    if (gapsOut != nullptr) {
        for (std::size_t i = 1; i < starts.size(); ++i) { gapsOut->push_back(msBetween(starts[i - 1], starts[i])); }
        std::sort(gapsOut->begin(), gapsOut->end());
    }
    return passes;
}

void testKickFloor() {
    std::printf("  K3: the 2 ms floor holds against kicks as fast as a thread can make them\n");
    const int floodMs = 1000;
    std::vector<double> gaps;
    const std::uint64_t dflt = kickFloodPasses(2, floodMs, &gaps);
    const std::uint64_t on = kickFloodPasses(40, floodMs);
    const std::uint64_t off = kickFloodPasses(0, floodMs);
    std::printf("      one second of kicks flat out: floor 2 ms -> %llu passes (bound %d), floor 40 ms -> %llu (bound %d), floor off -> %llu\n",
                static_cast<unsigned long long>(dflt), floodMs / 2, static_cast<unsigned long long>(on), floodMs / 40, static_cast<unsigned long long>(off));
    CHECK(dflt <= static_cast<std::uint64_t>(floodMs / 2) + 20u);
    CHECK(dflt >= 100u);
    CHECK(on <= static_cast<std::uint64_t>(floodMs / 40) + 4u);
    CHECK(on >= 5u);
    CHECK(off > on * 3u);
    CHECK(gaps.size() >= 100u);
    if (!gaps.empty()) {
        std::printf("      start-to-start at the 2 ms floor: min %.3f ms, median %.3f ms\n", gaps.front(), median(gaps));
        // (The hook runs a few microseconds after the loop stamps the start the floor is measured from, and that lag jitters: 1.95, not 2.)
        CHECK(gaps.front() >= 1.95);
        CHECK(median(gaps) >= 1.99 && median(gaps) < 4.0);
    }
}

// K4: the gap drops a kick a pass cannot use: a window at 1 000 frames a second asks for ~125 passes, not 500.
void testKickGap() {
    std::printf("  K4: a kick within the gap of a pass's start is dropped (8 ms: ~125 passes a second at most)\n");
    std::uint64_t passes[2] = {}, dropped[2] = {};
    for (int i = 0; i < 2; ++i) {
        Engine e;
        e.initialise();
        e.startControlThread(kickOpts(1000, 2, 24, i == 0 ? 8 : 0));
        (void)e.runPassNow();
        const Engine::PassStats before = e.passStats();
        {
            Kicker k(e, 0.0);
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            k.stop();
        }
        const Engine::PassStats d = statsDiff(e.passStats(), before);
        e.stopControlThread();
        passes[i] = d.passes;
        dropped[i] = d.kicksDropped;
    }
    std::printf("      one second flat out: gap 8 ms -> %llu passes (%llu kicks dropped); gap off -> %llu passes\n",
                static_cast<unsigned long long>(passes[0]), static_cast<unsigned long long>(dropped[0]), static_cast<unsigned long long>(passes[1]));
    CHECK(passes[0] <= 1000u / 8u + 6u);
    CHECK(passes[0] >= 60u);
    CHECK(dropped[0] > 1000u);
    CHECK(passes[1] > passes[0] * 2u);
    CHECK(dropped[1] == 0u);
}

// K5: what a kick is not. Not a submit (no lag is stamped, so the submit-to-publish figure stays the submits'); a pass a kick starts is the
// whole pass (the remote controls' phase - the internal tier of the web and CAT sessions - runs in it); and an engine that is headless, stepped
// or without a thread is not touched. (Until stage 5d the phase's mark was the remote-control SOURCE being pulled once a pass; the pull is
// gone, and the phase's own count - ApiHost::tailPhasesForTest, once a pass whether or not the tier held anything - stands in for it.)
void testKickIsOnlyAKick() {
    std::printf("  K5: a kick is not a submit; a kicked pass runs the remote controls' phase; headless, stepped and thread-less engines are untouched\n");
    {
        Engine e;
        e.initialise();
        e.startControlThread(kickOpts(1500, 2, 24, 0));   // period 1500 ms: only a wake or a kick starts a pass
        CHECK(e.runPassNow());
        const std::uint64_t pulls0 = e.api().tailPhasesForTest();
        const Engine::PassStats before = e.passStats();
        std::size_t nKicks = 0;
        {
            Kicker k(e, 1000.0 / 60.0);
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            k.stop();
            nKicks = k.times.size();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        const Engine::PassStats d = statsDiff(e.passStats(), before);
        const std::uint64_t pulls = e.api().tailPhasesForTest() - pulls0;
        std::printf("      %zu kicks: %llu passes, the remote controls' phase run %llu times, submit-to-publish served %llu, passes started by a wake %llu\n", nKicks,
                    static_cast<unsigned long long>(d.passes), static_cast<unsigned long long>(pulls), static_cast<unsigned long long>(d.lagCount), static_cast<unsigned long long>(d.wakes));
        CHECK(nKicks >= 20u);
        CHECK(static_cast<std::size_t>(pulls) + 2u >= nKicks);   // the kicked pass is the whole pass: it runs the remote controls' phase (pumpRemoteControls)
        CHECK(pulls == d.passes);
        CHECK(d.lagCount == 0u);                                  // a kick stamps no submit's wake: nothing is "served"
        CHECK(d.wakes == 0u);
        // ... and a real submit still is one.
        e.submitCommand(cmd::makeNum(FOXAPI_OP_SET_VOLUME, 0.31));
        (void)spinFor([&] { return A::publishedVolume(e) == 0.31f; }, 3000);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        CHECK(e.passStats().lagCount - before.lagCount >= 1u);
        e.stopControlThread();
    }
    {
        // HEADLESS: nothing kicks. Paced by the timer exactly as A2 holds: ~62 a second, every pass the timer's.
        Engine e;
        e.initialise();
        e.startControlThread(kickOpts(16, 2, 24, 8));
        const Engine::PassStats before = e.passStats();
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        const Engine::PassStats d = statsDiff(e.passStats(), before);
        e.stopControlThread();
        std::printf("      headless, 1.5 s: %llu passes (%.1f a second), %llu by the timer, %llu kicks\n", static_cast<unsigned long long>(d.passes),
                    static_cast<double>(d.passes) / 1.5, static_cast<unsigned long long>(d.timedPasses), static_cast<unsigned long long>(d.kicks));
        CHECK(d.passes >= 60u && d.passes <= 130u);
        CHECK(d.timedPasses + 1u >= d.passes);
        CHECK(d.kicks == 0u && d.kickedPasses == 0u);
    }
    {
        // STEPPED: a kick is nothing (the window's stepPass calls are the passes). No thread: nothing either.
        Engine e;
        e.initialise();
        e.frameKick();   // no control thread
        CHECK(e.passStats().kicks == 0u);
        e.startControlThread(opts(16, 2, true));
        const std::uint64_t p0 = A::passes(e);
        for (int i = 0; i < 50; ++i) { e.frameKick(); }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        CHECK(A::passes(e) == p0);
        CHECK(e.passStats().kicks == 0u && e.passStats().kickedPasses == 0u);
        e.stopControlThread();
    }
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
    // PASS_CONTROL_ONLY=K1 (or K1,K2 ...) runs just those K cases: the flake protocol loops one case a hundred times, not the whole 50 s file.
    if (const char* only = std::getenv("PASS_CONTROL_ONLY"); only != nullptr && only[0] != 0) {
        const std::string want = std::string(",") + only + ",";
        const auto has = [&](const char* tag) { return want.find(std::string(",") + tag + ",") != std::string::npos; };
        if (has("K1")) { testKickPaces(); }
        if (has("K2")) { testKickFallback(); }
        if (has("K3")) { testKickFloor(); }
        if (has("K4")) { testKickGap(); }
        if (has("K5")) { testKickIsOnlyAKick(); }
        std::error_code ec0;
        std::filesystem::remove_all(g_scratch, ec0);
        return testSummary("test_pass_control (filtered)");
    }
    testWake();
    testPassFigures();
    testPeriodRate();
    testOwnClock();
    testFloorBetweenStarts();
    testWakeToStart();
    testMinimizedImmunity();
    testKickPaces();
    testKickFallback();
    testKickFloor();
    testKickGap();
    testKickIsOnlyAKick();
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
