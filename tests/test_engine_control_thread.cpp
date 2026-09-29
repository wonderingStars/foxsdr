// test_engine_control_thread.cpp - the engine pumping itself on a control
// thread, with a front end on another (stage 3b, docs/engine-stage3.md section
// 10, step 2; OPEN 8).
//
//   A  commands submitted by the front end are applied by the control thread
//      - the front end never pumps
//   B  THE TRANSMIT PATH, both halves on their own threads: the front end's
//      key-down is keyed by the control side; its key-up goes quiet within a
//      block; and a front end that stops (a frozen window) with the PTT held is
//      released at kKeyAliveWait while the control thread runs on
//   C  THE CONTROL THREAD PARKED with the key down (a test-only hook: no pass,
//      no tick) while the front end stays alive: the TX thread unkeys within
//      kKeyAliveWait of the last tick plus a block - and the control thread's
//      own watchdog saw the gap
//   D  OPEN 8: a bounded wait on the control thread (a plugin rescan) pauses
//      the control thread's watchdog and NOT the host's; the same wait made on
//      the front end's thread (3a) still pauses the host's
//   E  stopping the control thread with the key down: quiet by the time the
//      stop returns, well inside kKeyAliveWait; no pass after it
//   E2 stopped while the control thread is inside a long wait with the key
//      down: the radio is quiet within a block of the stop being asked for,
//      long before the join returns - the key-up does not wait for the join
//   F  an engine destroyed with its control thread running stops it cleanly
//   G  shutdownQuiesce() stops a running control thread before anything else
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "core/app_commands.hpp"
#include "core/transmitter.hpp"
#include "engine/engine.hpp"
#include "engine/engine_host.hpp"
#include "engine/tx_page_key.hpp"
#include "test_check.hpp"

using cascade::core::Transmitter;
using cascade::core::TxInput;
using cascade::engine::Engine;
using Clock = std::chrono::steady_clock;

namespace {

void setEnv(const char* n, const std::string& v) {
#if defined(_WIN32)
    ::SetEnvironmentVariableA(n, v.c_str());
    _putenv_s(n, v.c_str());
#else
    ::setenv(n, v.c_str(), 1);
#endif
}

void isolate() {
#if defined(_WIN32)
    const int pid = _getpid();
    char buf[MAX_PATH];
    GetTempPathA(MAX_PATH, buf);
    const std::string tmp = std::string(buf) + "foxsdr_control_thread_" + std::to_string(pid);
#else
    const int pid = static_cast<int>(getpid());
    const std::string tmp = "/tmp/foxsdr_control_thread_" + std::to_string(pid);
#endif
    for (const char* v : {"LOCALAPPDATA", "APPDATA", "USERPROFILE", "HOME", "XDG_CONFIG_HOME",
                          "XDG_STATE_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"}) {
        setEnv(v, tmp);
    }
    setEnv("FOXSDR_TELEMETRY_URL", "http://127.0.0.1:9");
}

class RecordingSink : public cascade::source::IqSink {
public:
    bool start() override {
        running_ = true;
        return true;
    }
    void stop() override { running_ = false; }
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
    const char* name() const override { return "test radio"; }
    const char* lastError() const override { return ""; }

private:
    std::atomic<bool> running_{false};
};

// A headless front end that counts the pauses asked of ITS watchdog.
struct Host : cascade::engine::EngineHost {
    std::atomic<int> pauses{0};
    void pauseWatchdog() override { ++pauses; }
};

double msSince(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

template <typename F>
double waitFor(F done, int ms) {
    const auto t0 = Clock::now();
    while (!done() && msSince(t0) < ms) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
    return done() ? msSince(t0) : -1.0;
}

const double kAlive = static_cast<double>(Transmitter::kKeyAliveWait.count());
constexpr double kSlackMs = 150.0;   // a block (10 ms) and a loaded machine's scheduling

}  // namespace

namespace cascade::gui {
struct AppWindowTestAccess {
    static RecordingSink* installSink(Engine& e) {
        auto sink = std::make_unique<RecordingSink>();
        RecordingSink* raw = sink.get();
        e.transmitter_.setSink(std::move(sink));
        e.transmitter_.setInput(TxInput::Tone);
        return raw;
    }
    static bool transmitting(Engine& e) { return e.transmitter_.transmitting(); }
    static std::uint64_t passes(Engine& e) { return e.controlPasses_.load(); }
    static void park(Engine& e, bool on) { e.controlParkForTest_.store(on); }
    static void stall(Engine& e, int ms) { e.controlStallForTestMs_.store(ms); }
    static bool inStall(Engine& e) { return e.controlInStallForTest_.load(); }
    static double worstGapMs(Engine& e) { return e.controlWatchdog_.worstGapMs(); }
    static unsigned controlPauses(Engine& e) { return e.controlWatchdog_.pausesTaken(); }
};
}  // namespace cascade::gui

using A = cascade::gui::AppWindowTestAccess;

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_engine_control_thread\n");
    isolate();

    cascade::engine::ControlThreadOptions opts;
    opts.period = std::chrono::milliseconds(10);
    opts.watchdogThresholdMs = 5000;   // nothing here should be reported as a hang

    {
        Host host;
        Engine e(host);
        e.initialise();
        RecordingSink* raw = A::installSink(e);
        e.startControlThread(opts);
        CHECK(e.controlThreadRunning());

        // A: a command from the front end, applied by the control thread.
        e.submitCommand(cascade::core::cmd::makeNum(FOXAPI_OP_SET_VOLUME, 0.33));
        const double applied = waitFor([&] { return e.configSnapshot().volume == 0.33f; }, 1000);
        std::printf("A: volume submitted from the front end: applied and published after %.0f ms (%llu passes)\n",
                    applied, static_cast<unsigned long long>(A::passes(e)));
        CHECK(applied >= 0.0);

        // B: the transmit path.
        cascade::gui::TxPageRequest r;
        r.pageLive = true;
        std::uint64_t seq = 0;
        auto frame = [&](bool ptt) {
            r.pttHeld = ptt;
            r.frameSeq = ++seq;
            e.submitTransmitPageKey(r);
        };
        frame(true);
        const double keyed = waitFor([&] { return raw->running(); }, 500);
        std::printf("B: PTT pressed on the front end: keyed by the control thread after %.0f ms\n", keyed);
        CHECK(keyed >= 0.0);
        for (int i = 0; i < 20; ++i) {   // a live front end, 16 ms frames
            frame(true);
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
        CHECK(raw->running());
        frame(false);
        const double quiet = waitFor([&] { return !raw->running(); }, 1000);
        std::printf("   PTT let go: quiet after %.0f ms\n", quiet);
        CHECK(quiet >= 0.0 && quiet < 100.0);
        frame(true);
        CHECK(waitFor([&] { return raw->running(); }, 500) >= 0.0);
        const auto lastFrame = Clock::now();   // ...and the front end freezes, PTT held
        const std::uint64_t passesAtFreeze = A::passes(e);
        const double released = waitFor([&] { return !raw->running(); }, 3000) >= 0.0 ? msSince(lastFrame) : -1.0;
        std::printf("   front end frozen, PTT held: released %.0f ms after its last frame (bound %.0f + %.0f); "
                    "%llu control passes meanwhile\n",
                    released, kAlive, kSlackMs, static_cast<unsigned long long>(A::passes(e) - passesAtFreeze));
        CHECK(released >= kAlive - 20.0 && released <= kAlive + kSlackMs);
        CHECK(A::passes(e) - passesAtFreeze > 50u);
        frame(false);

        // C: the control thread parked, the front end alive, the key down.
        // The front end draws its frames throughout, as a window does (a
        // press is re-asserted every frame, so one landing in the very pass
        // that tidies up B's release is simply applied by the next).
        std::atomic<bool> frontStop{false};
        std::thread frontEnd([&] {
            while (!frontStop.load()) {
                frame(true);
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
            }
        });
        CHECK(waitFor([&] { return raw->running(); }, 500) >= 0.0);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        CHECK(raw->running());
        A::park(e, true);
        const auto parkedAt = Clock::now();
        const double unkeyed = waitFor([&] { return !raw->running(); }, 3000) >= 0.0 ? msSince(parkedAt) : -1.0;
        std::printf("C: control thread parked with the key down, front end alive: unkeyed %.0f ms after the park "
                    "(bound %.0f + a block + slack)\n",
                    unkeyed, kAlive);
        CHECK(unkeyed >= 0.0 && unkeyed <= kAlive + kSlackMs);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        CHECK(!raw->running());   // and it stays unkeyed while parked
        A::park(e, false);
        frontStop.store(true);
        frontEnd.join();
        frame(false);
        CHECK(waitFor([&] { return A::worstGapMs(e) >= kAlive; }, 500) >= 0.0);
        std::printf("   the control thread's watchdog: worst gap %.0f ms\n", A::worstGapMs(e));
        CHECK(waitFor([&] { return !A::transmitting(e); }, 500) >= 0.0);

        // D: OPEN 8.
        const int hostBefore = host.pauses.load();
        const unsigned controlBefore = A::controlPauses(e);
        e.submitCommand(cascade::core::cmd::make(FOXAPI_OP_PLUGIN_RESCAN));
        CHECK(waitFor([&] { return A::controlPauses(e) > controlBefore; }, 1000) >= 0.0);
        std::printf("D: a plugin rescan on the control thread: control watchdog paused %u time(s), host's %d\n",
                    A::controlPauses(e) - controlBefore, host.pauses.load() - hostBefore);
        CHECK(host.pauses.load() == hostBefore);

        // E: stopped with the key down.
        frame(true);
        CHECK(waitFor([&] { return raw->running(); }, 500) >= 0.0);
        const auto t0 = Clock::now();
        e.stopControlThread();
        const double stopMs = msSince(t0);
        const bool quietAtReturn = !raw->running();
        const std::uint64_t passesAtStop = A::passes(e);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        std::printf("E: stopped with the key down: returned after %.0f ms, radio quiet=%d, passes after=%llu\n",
                    stopMs, quietAtReturn ? 1 : 0, static_cast<unsigned long long>(A::passes(e) - passesAtStop));
        CHECK(!e.controlThreadRunning());
        CHECK(quietAtReturn);
        CHECK(stopMs < kAlive / 2.0);
        CHECK(A::passes(e) == passesAtStop);
        CHECK(!A::transmitting(e));

        // D, second half: with no control thread (3a), the same wait pauses
        // the host's watchdog.
        const unsigned controlAfter = A::controlPauses(e);
        (void)e.applyCommand(cascade::core::cmd::make(FOXAPI_OP_PLUGIN_RESCAN));
        std::printf("D: the same rescan on the front end's thread: host's watchdog paused %d time(s), control's %u\n",
                    host.pauses.load() - hostBefore, A::controlPauses(e) - controlAfter);
        CHECK(host.pauses.load() == hostBefore + 1);
        CHECK(A::controlPauses(e) == controlAfter);

        // E2: the stop asked for while the control thread is in a long wait.
        e.startControlThread(opts);
        frame(true);
        CHECK(waitFor([&] { return raw->running(); }, 500) >= 0.0);
        A::stall(e, 700);
        CHECK(waitFor([&] { return A::inStall(e); }, 500) >= 0.0);
        std::atomic<bool> stopped{false};
        const auto s0 = Clock::now();
        std::thread stopper([&] {
            e.stopControlThread();
            stopped.store(true);
        });
        const double quietAfter = waitFor([&] { return !raw->running(); }, 2000);
        const bool stillJoining = !stopped.load();
        stopper.join();
        std::printf("E2: stopped during a 700 ms wait with the key down: quiet after %.0f ms (join still running "
                    "then: %d), stop returned after %.0f ms\n",
                    quietAfter, stillJoining ? 1 : 0, msSince(s0));
        CHECK(quietAfter >= 0.0 && quietAfter < 100.0);
        CHECK(stillJoining);
        CHECK(!A::transmitting(e));

        e.shutdown();
    }

    // G: shutdownQuiesce stops the thread.
    {
        Engine e;
        e.initialise();
        e.startControlThread(opts);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        e.shutdownQuiesce();
        std::printf("G: after shutdownQuiesce: control thread running=%d\n", e.controlThreadRunning() ? 1 : 0);
        CHECK(!e.controlThreadRunning());
        e.shutdown();
    }

    // F: destroyed with the thread running.
    {
        Engine e;
        e.initialise();
        e.startControlThread(opts);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const auto t0 = Clock::now();
        {
            Engine* gone = &e;
            (void)gone;
        }
        e.teardown();   // what ~Engine runs for a front end that did not
        std::printf("F: torn down with the control thread running: %.0f ms, running=%d\n", msSince(t0),
                    e.controlThreadRunning() ? 1 : 0);
        CHECK(!e.controlThreadRunning());
    }

    return testSummary("test_engine_control_thread");
}
