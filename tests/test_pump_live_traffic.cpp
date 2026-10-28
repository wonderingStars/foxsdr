// test_pump_live_traffic.cpp - THE UNLOAD HANDSHAKE AND THE ATTACH-ORDERING WAIT UNDER LIVE TRAFFIC, MEASURED (the pump move's step P5;
// docs/engine-stage4.md, "PUMP MOVE, STEPS P4 AND P5").
//
// Step P3 built the handshake (the pass posts a directive and waits, bounded, for the window's frame to apply it) and left two cases open
// until the pass actually ran on its own thread beside a framing window: THE ATTACH-ORDERING SOFT WAIT (a decoder's create() follows the
// window's attach(), so refreshPluginRunner waits for the window to apply the directive before it builds the runner; a wait that runs out
// is a warning and the decoders are built without it - so every wait that ENDS APPLIED is an exact ordering, and the count of waits that
// ran out is the count of overtakes), and THE TRANSMIT STALL (a pass blocked in the handshake is a pass that does not tick the key; the
// bound is 250 ms against the 1 s control stamp). Nothing below asserts a number a machine decides, except the safety bounds themselves;
// each case PRINTS what it measured, and the figures are recorded in docs/engine-stage4.md.
//
//   L1  THE WAIT'S COST AT THREE FRAME CADENCES (flat out, 60 Hz, 20 Hz) on a real AppWindow with the pass free-running at 4 ms: waits made,
//       mean and longest, none ran out
//   L2  A FRAME THAT STALLS (150 ms, inside the 250 ms bound) while a rescan waits: the wait is the stall, nothing ran out
//   L3  THE TRANSMIT KEY ACROSS A HANDSHAKE: the key is held by the window's own session (a heartbeat and TX_PTT every frame) while plugin
//       rescans happen - first with the window framing, then with the window silent for 400 ms (the wait runs out at its bound, ONCE, the
//       key's keep-alive never lapses): the radio is never unkeyed by the handshake, and the pass's worst gap stays far inside the control
//       stamp (the TX thread's own dead-man, 1 s)
//   L4  A WINDOW THAT NEVER FRAMES AGAIN costs the pass one bound, then nothing (the silence rule), and a shutdown with it parked returns
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "core/transmitter.hpp"
#include "engine/engine.hpp"
#include "engine_api_fake_radio.hpp"
#include "gui/app_window.hpp"
#include "test_host.hpp"
#include "pump3_support.hpp"
#include "source/tx_sink.hpp"
#include "test_check.hpp"

#define CHECK_EQ(a, b) CHECK((a) == (b))

using cascade::gui::AppWindow;
using Access = cascade::gui::AppWindowTestAccess;
using namespace pump3;
using Clock = std::chrono::steady_clock;

namespace {

std::string g_fixturePath;

double msSince(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

// The unmap hook the engine calls the last moment before it unmaps the modules (a completed unload): counted.
std::atomic<std::uint64_t> g_unmaps{0};
void unmapHook() { ++g_unmaps; }
std::uint64_t g_unmapRuns() { return g_unmaps.load(); }
void setUnmapCounter() { Access::setUnmapHook(&unmapHook); }

void spinUntil(Clock::time_point end) {
    while (Clock::now() < end) { std::this_thread::yield(); }
}

// A radio's transmit sink that counts its starts and stops and says whether it is keyed.
class CountingSink : public cascade::source::IqSink {
public:
    bool start() override {
        running_ = true;
        ++starts;
        return true;
    }
    void stop() override {
        if (running_.exchange(false)) { ++stops; }
    }
    void finish() override { stop(); }
    bool running() const override { return running_; }
    double sampleRateHz() const override { return 2.5e6; }
    bool setSampleRateHz(double) override { return true; }
    double centerFrequencyHz() const override { return 146.52e6; }
    bool setCenterFrequencyHz(double) override { return true; }
    bool frequencyRangeHz(double& lo, double& hi) const override {
        lo = 70.0e6;
        hi = 6.0e9;
        return true;
    }
    bool sampleRateRangeHz(double& lo, double& hi) const override {
        lo = 2.083e6;
        hi = 61.44e6;
        return true;
    }
    bool gainRangeDb(double& lo, double& hi) const override {
        lo = -89.75;
        hi = 0.0;
        return true;
    }
    double gainDb() const override { return 0.0; }
    bool setGainDb(double) override { return true; }
    std::size_t write(const std::complex<float>*, std::size_t n) override { return running_ ? n : 0; }
    bool faulted() const override { return false; }
    const char* name() const override { return "counting sink"; }
    const char* lastError() const override { return ""; }
    std::atomic<int> starts{0};
    std::atomic<int> stops{0};

private:
    std::atomic<bool> running_{false};
};

struct Env {
    std::unique_ptr<th::TestWindow> app;
    Env() {
        app = std::make_unique<th::TestWindow>();
        Access::pinPlugins(*app);
        Access::poll(*app);
    }
    ~Env() {
        if (app != nullptr && Access::controlRunning(*app)) { Access::stopControl(*app); }
    }
    AppWindow& a() { return *app; }
};

// One rescan, asked of the pass as a front end asks it (a command), and the frames the window makes until it has completed (the
// rescan's unmap hook counted) - frames at `periodMs` (0: flat out).
struct Wait {
    double waitMs = 0.0;       // the pass's own wait for the acknowledgement, from the engine's counters
    bool ranOut = false;
    bool done = false;
};

Wait oneRescan(AppWindow& a, int periodMs, bool key = false) {
    const auto before = Access::waitStats(a);
    const std::uint64_t unmapsBefore = g_unmapRuns();
    Access::submit(a, cmd::makeText(FOXAPI_OP_PLUGIN_RESCAN, std::string()));
    const auto began = Clock::now();
    auto next = began;
    Wait w;
    while (msSince(began) < 3000.0) {
        Access::frame(a, key);
        if (g_unmapRuns() > unmapsBefore) {
            w.done = true;
            break;
        }
        if (periodMs > 0) {
            next += std::chrono::milliseconds(periodMs);
            spinUntil(next);
        } else {
            std::this_thread::yield();
        }
    }
    const auto after = Access::waitStats(a);
    w.waitMs = static_cast<double>(after.totalUs - before.totalUs) / 1000.0;
    w.ranOut = after.timeouts > before.timeouts;
    return w;
}

// ======================================================================================================================
void testWaitCost() {
    std::printf("  L1: the pass's wait for the window's acknowledgement, three frame cadences (free-running pass, 4 ms)\n");
    for (const int periodMs : {0, 16, 50}) {
        Env env;
        AppWindow& a = env.a();
        CHECK_EQ(Access::ui(a).trackCount(), 3u);
        Access::startControl(a, std::chrono::milliseconds(4));
        std::vector<double> waits;
        int ranOut = 0, notDone = 0;
        const auto base = Access::waitStats(a);
        for (int i = 0; i < 12; ++i) {
            const Wait w = oneRescan(a, periodMs);
            waits.push_back(w.waitMs);
            ranOut += w.ranOut ? 1 : 0;
            notDone += w.done ? 0 : 1;
            // frames between rescans, at the cadence, so the window is "live" and not only waited upon
            for (int f = 0; f < 5; ++f) {
                Access::frame(a);
                if (periodMs > 0) { std::this_thread::sleep_for(std::chrono::milliseconds(periodMs)); }
            }
        }
        const auto st = Access::waitStats(a);
        std::sort(waits.begin(), waits.end());
        const char* name = periodMs == 0 ? "flat out" : (periodMs == 16 ? "~60 Hz" : "~20 Hz");
        std::printf("      frames %-9s %llu waits for the acknowledgement (12 rescans, each waits for its release and its rebuild): total mean %.1f ms, per rescan median %.1f ms, "
                    "longest single wait %.1f ms, ran out %llu\n",
                    name, static_cast<unsigned long long>(st.waits - base.waits),
                    st.waits > base.waits ? static_cast<double>(st.totalUs - base.totalUs) / 1000.0 / static_cast<double>(st.waits - base.waits) : 0.0,
                    waits[waits.size() / 2], static_cast<double>(st.maxUs) / 1000.0, static_cast<unsigned long long>(st.timeouts - base.timeouts));
        CHECK_EQ(notDone, 0);
        CHECK_EQ(ranOut, 0);                                   // no wait ran out, so no decoder's create() overtook an attach()
        CHECK(st.timeouts == base.timeouts);
        CHECK(st.maxUs < 250000u);
    }
}

void testStalledFrame() {
    std::printf("  L2: a frame that stalls 150 ms while a rescan waits (the bound is %d ms)\n", static_cast<int>(Engine::kPluginUiHandshake.count()));
    Env env;
    AppWindow& a = env.a();
    Access::startControl(a, std::chrono::milliseconds(4));
    for (int i = 0; i < 5; ++i) { Access::frame(a); }
    const auto before = Access::waitStats(a);
    const std::uint64_t unmapsBefore = g_unmapRuns();
    Access::submit(a, cmd::makeText(FOXAPI_OP_PLUGIN_RESCAN, std::string()));
    // The window is inside a long frame (a slow draw): it does not apply the directive for 150 ms, then frames normally.
    const auto t0 = Clock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    while (g_unmapRuns() == unmapsBefore && msSince(t0) < 3000.0) { Access::frame(a); }
    const auto after = Access::waitStats(a);
    const double total = static_cast<double>(after.totalUs - before.totalUs) / 1000.0;
    std::printf("      the rescan completed after %.0f ms; the pass waited %.0f ms in all over %llu wait(s); ran out %llu\n", msSince(t0), total,
                static_cast<unsigned long long>(after.waits - before.waits), static_cast<unsigned long long>(after.timeouts - before.timeouts));
    CHECK(g_unmapRuns() > unmapsBefore);
    CHECK(after.timeouts == before.timeouts);
    // THE LOWER BOUND IS 150 MINUS A TIMER TICK, NOT MINUS 10 ms (found by the P6 soak: 3 runs of 50 read 137-138 ms against the old 140).
    // The wait starts when the pass takes the rescan up, and a wake can be served up to one Windows timer tick (~15.6 ms) after the submit
    // (P5 TRAP 7: the floor is the tick in practice); t0 above is taken at the submit, so the wait can begin that late and still end with the
    // window's first frame at 150 ms. What the bound proves is unchanged: the pass WAITED for the stalled frame (a pass that skipped the wait
    // reads ~0) and did not run out at the 250 ms bound (the check above and the 330 ceiling).
    CHECK(total >= 130.0 && total < 330.0);
}

void testTransmitKey() {
    std::printf("  L3: the transmit key across a handshake (the window's own session holds it)\n");
    Env env;
    AppWindow& a = env.a();
    auto sinkOwner = std::make_unique<CountingSink>();
    CountingSink* sink = sinkOwner.get();
    Access::installSink(a, std::move(sinkOwner));
    Access::startControl(a, std::chrono::milliseconds(4));
    // KEY: frames at ~100 Hz, each re-asserting the PTT.
    const auto t0 = Clock::now();
    while (!sink->running() && msSince(t0) < 2000.0) {
        Access::frame(a, true);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(sink->running());
    const int stops0 = sink->stops.load();
    // THE PASS'S OWN STARVATION, measured from outside: the longest span in which no pass finished. (The watchdog's worst gap excludes the
    // handshake's wait - it pauses the watchdog across it, on purpose - so it would say the stall never happened.)
    std::atomic<bool> sampling{true};
    std::atomic<double> longestNoPassMs{0.0};
    std::thread sampler([&] {
        std::uint64_t last = Access::passes(a);
        auto since = Clock::now();
        while (sampling.load()) {
            const std::uint64_t now = Access::passes(a);
            if (now != last) {
                last = now;
                since = Clock::now();
            } else if (msSince(since) > longestNoPassMs.load()) {
                longestNoPassMs.store(msSince(since));
            }
            std::this_thread::sleep_for(std::chrono::microseconds(300));
        }
    });
    auto frames = [&](int ms) {
        const auto b = Clock::now();
        while (msSince(b) < ms) {
            Access::frame(a, true);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    };
    frames(200);
    // 1. a rescan with the window framing at 100 Hz
    const Wait w1 = oneRescan(a, 10, /*key=*/true);
    frames(100);
    std::printf("      window framing at 100 Hz: the rescan done=%d, the pass waited %.1f ms; key still up: %d, stops since the key went down: %d\n",
                w1.done ? 1 : 0, w1.waitMs, sink->running() ? 1 : 0, sink->stops.load() - stops0);
    CHECK(w1.done);
    CHECK(sink->running());
    CHECK_EQ(sink->stops.load() - stops0, 0);
    // 2. a rescan with the window silent for 400 ms (more than the bound): the wait runs out ONCE, the modules stay mapped, and the key's
    // own keep-alive (a heartbeat and PTT every frame, last made just before the silence) is 1 s, so it does not lapse.
    const auto before = Access::waitStats(a);
    const std::uint64_t unmapsBefore = g_unmapRuns();
    Access::frame(a, true);                                         // the last frame before the silence: re-asserts PTT and heartbeats
    Access::submit(a, cmd::makeText(FOXAPI_OP_PLUGIN_RESCAN, std::string()));
    const auto silenceStart = Clock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    const bool keyedDuringSilence = sink->running();
    const bool unmappedDuringSilence = g_unmapRuns() > unmapsBefore;
    frames(60);                                                     // the window returns: the retry completes at its first frames
    const auto t1 = Clock::now();
    while (g_unmapRuns() == unmapsBefore && msSince(t1) < 3000.0) {
        Access::frame(a, true);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const auto after = Access::waitStats(a);
    sampling = false;
    sampler.join();
    const double worstGap = longestNoPassMs.load();
    std::printf("      window silent 400 ms: the pass waited %.0f ms in %llu wait(s), %llu ran out; modules unmapped during the silence: %d; "
                "key up through it: %d; unmapped %.0f ms after the silence began\n",
                static_cast<double>(after.totalUs - before.totalUs) / 1000.0, static_cast<unsigned long long>(after.waits - before.waits),
                static_cast<unsigned long long>(after.timeouts - before.timeouts), unmappedDuringSilence ? 1 : 0, keyedDuringSilence ? 1 : 0,
                msSince(silenceStart));
    std::printf("      the longest span in which no pass finished, over the whole case: %.0f ms (the control stamp is 1000 ms)\n", worstGap);
    CHECK(!unmappedDuringSilence);                                  // the modules stayed mapped while the window did not answer
    CHECK(keyedDuringSilence);
    CHECK(sink->running());
    CHECK_EQ(sink->stops.load() - stops0, 0);                       // the radio was never unkeyed by a handshake
    CHECK(after.timeouts - before.timeouts == 1u);                  // one bound, once
    CHECK(worstGap < 600.0);                                        // well inside the 1 s control stamp
    CHECK(g_unmapRuns() > unmapsBefore);                            // ...and the unload completed once the window framed again
    // let go
    Access::submit(a, cmd::QueuedCommand{cmd::makeInt(FOXAPI_OP_TX_PTT, 0), {}});
    for (int i = 0; i < 40; ++i) {
        Access::frame(a, false);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

void testNeverFramesAgain() {
    std::printf("  L4: a window that never frames again\n");
    Env env;
    AppWindow& a = env.a();
    Access::startControl(a, std::chrono::milliseconds(4));
    for (int i = 0; i < 5; ++i) { Access::frame(a); }
    const auto before = Access::waitStats(a);
    const std::uint64_t unmapsBefore = g_unmapRuns();
    // Four refreshes with the window silent: the first costs the bound, the rest nothing (the silence rule).
    for (int i = 0; i < 4; ++i) { Access::submit(a, cmd::makeText(FOXAPI_OP_PLUGIN_RESCAN, std::string())); }
    const auto t0 = Clock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    const auto after = Access::waitStats(a);
    std::printf("      700 ms with the window silent and four rescans asked: %llu wait(s) of %.0f ms in all, %llu ran out, %llu skipped by the rule; "
                "modules unmapped: %d; the pass kept running (%llu passes)\n",
                static_cast<unsigned long long>(after.waits - before.waits), static_cast<double>(after.totalUs - before.totalUs) / 1000.0,
                static_cast<unsigned long long>(after.timeouts - before.timeouts), static_cast<unsigned long long>(after.silenceSkips - before.silenceSkips),
                g_unmapRuns() > unmapsBefore ? 1 : 0, static_cast<unsigned long long>(Access::passes(a)));
    CHECK(after.timeouts - before.timeouts == 1u);
    CHECK(static_cast<double>(after.totalUs - before.totalUs) / 1000.0 < 330.0);
    CHECK(g_unmapRuns() == unmapsBefore);
    (void)t0;
    // The shutdown with the window parked: the control thread stops inside its own bound, whatever it is waiting on.
    const auto s0 = Clock::now();
    Access::stopControl(a);
    const double stopMs = msSince(s0);
    std::printf("      stopControlThread with the window parked: %.1f ms\n", stopMs);
    CHECK(stopMs < 1500.0);
}

// THE GL RULE AND THE HANDSHAKE'S SAFETY, from the one witness that survives an unmap: each time the fixture module was unmapped (every rescan
// above did it, and the engine's teardown does the last) it wrote what it had seen in that image - the instances still alive, the calls made
// through a handle that was not live, the destroys beside a poll, the creates and destroys on a thread other than the polling one (the window's:
// the basemap's tiles are GL textures, so a release is on the thread that owns the context), and the basemap and track-info clients still alive.
// Every line must say the module was left with nothing alive and nothing done wrongly - under live traffic, at three frame cadences, with a
// stalled frame and a silent window.
void checkUnmapWitness(const std::string& path) {
    std::ifstream in(path);
    std::string line;
    long lines = 0, bad = 0, createsTotal = 0;
    while (std::getline(in, line)) {
        long live = -1, creates = 0, destroys = 0, stale = -1, beside = -1, wrong = -1, bm = -1, info = -1;
        if (std::sscanf(line.c_str(), "unmapped live=%ld creates=%ld destroys=%ld stale=%ld beside_poll=%ld wrong_thread=%ld basemap_live=%ld info_live=%ld",
                        &live, &creates, &destroys, &stale, &beside, &wrong, &bm, &info) < 8) {
            continue;
        }
        ++lines;
        createsTotal += creates;
        if (live != 0 || stale != 0 || beside != 0 || wrong != 0 || bm != 0 || info != 0) {
            ++bad;
            std::printf("      BAD UNMAP: %s\n", line.c_str());
        }
    }
    std::printf("  GL rule / handshake safety, from the module's own witness at each unmap: %ld unmaps, %ld with anything alive or done wrongly; %ld creates in all\n",
                lines, bad, createsTotal);
    CHECK(lines >= 36 + 4);       // 3 x 12 rescans (L1), L2, the two in L3, L4's retry, and the teardown's
    CHECK_EQ(bad, 0);
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_pump_live_traffic\n");
    isolate("pump_live");
    Access::install();
    g_fixturePath = installFixture();
    setUnmapCounter();
    const std::string unmapLog = (g_scratch / "unmap.log").string();
    setEnv("FIXTURE_P3_UNMAP_LOG", unmapLog);
    testWaitCost();
    testStalledFrame();
    testTransmitKey();
    testNeverFramesAgain();
    checkUnmapWitness(unmapLog);
    return testSummary("test_pump_live_traffic");
}
