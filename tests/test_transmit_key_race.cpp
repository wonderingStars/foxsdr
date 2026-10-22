// test_transmit_key_race.cpp - a key-up can never be undone, and a press
// made before a page went away never keys the radio after it - with the key
// on the engine API (engine stage 4, API 0.2's THE KEY).
//
// Until stage 4 the Transmit page handed the engine a per-frame request
// through a latest-value slot, and the hazards this file pinned were the
// slot's: a key-up landing between the control side's read and its write
// (A-C), a frozen window's old request re-keying the radio (D), a press
// after the TX thread's own key-down being lost (E), and a press, a close and
// a reopen inside one pump latching the radio on the reopen (F; the Opus
// review of 47e991e). Since stage 4 there is no slot: the page SUBMITS its key
// on the window's own LOCAL session (gui::txKeyCommands), in queue order, and
// the engine's key rules decide. The same hazards, held again end to end -
// a real Engine, its API host, the real core::Transmitter and its TX thread,
// and a radio that is only a ledger:
//
//   A  PTT pressed and let go within one frame (both in the batches the next
//      pass applies, in order): never keyed, every round.
//   B  LATCH pressed, then the page closes, before a pass: never keyed.
//   C  THE HAMMER: the control thread free-running while the front end keys
//      down and up kHammer times; after every PTT 0 the key is up by the
//      control side's second pass after it, and stays up.
//   D  THE FROZEN WINDOW: the front end stops (no heartbeat, no PTT) with the
//      PTT held: released at the keep-alive (1 s, from its last heartbeat),
//      and never keyed again while the control thread runs on.
//   E  A PRESS AFTER THE TX THREAD PLAYED THE KEY DOWN ITSELF: latched,
//      released, the tail played; a new press (past the 1 s re-arm) latches
//      and keys, every round.
//   F  A PRESS, A CLOSE AND A REOPEN BEFORE THE CONTROL SIDE RUNS: nothing
//      latched on the reopen and the radio never starts - a page close ends
//      any pending press.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "core/transmitter.hpp"
#include "engine/engine.hpp"
#include "engine/fox_engine_api.hpp"
#include "gui/transmit_page.hpp"
#include "net/web_control.hpp"
#include "test_check.hpp"
#include "tx_front_end.hpp"
#include "web_seat.hpp"

using cascade::core::Transmitter;
using cascade::core::TxInput;
using cascade::engine::Engine;

namespace {

void setEnv(const char* name, const std::string& value) {
#if defined(_WIN32)
    _putenv_s(name, value.c_str());
    SetEnvironmentVariableA(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

void isolate() {
#if defined(_WIN32)
    const int pid = _getpid();
    char buf[MAX_PATH];
    GetTempPathA(MAX_PATH, buf);
    const std::string tmp = std::string(buf) + "foxsdr_tx_key_race_" + std::to_string(pid);
#else
    const int pid = static_cast<int>(getpid());
    const std::string tmp = "/tmp/foxsdr_tx_key_race_" + std::to_string(pid);
#endif
    for (const char* v : {"LOCALAPPDATA", "APPDATA", "USERPROFILE", "HOME", "XDG_CONFIG_HOME",
                          "XDG_STATE_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"}) {
        setEnv(v, tmp);
    }
    setEnv("FOXSDR_TELEMETRY_URL", "http://127.0.0.1:9");
}

// A radio that is only a ledger (test_transmit_dead_man.cpp's shape).
class RecordingSink : public cascade::source::IqSink {
public:
    bool start() override {
        ++starts;
        running_ = true;
        return true;
    }
    void stop() override { running_ = false; }
    void finish() override { stop(); }
    bool running() const override { return running_; }
    double sampleRateHz() const override { return rate_; }
    bool setSampleRateHz(double hz) override { rate_ = hz; return true; }
    double centerFrequencyHz() const override { return freq_; }
    bool setCenterFrequencyHz(double hz) override { freq_ = hz; return true; }
    bool frequencyRangeHz(double& lo, double& hi) const override {
        lo = 70.0e6; hi = 6.0e9; return true;
    }
    bool sampleRateRangeHz(double& lo, double& hi) const override {
        lo = 2.083e6; hi = 61.44e6; return true;
    }
    bool gainRangeDb(double& lo, double& hi) const override { lo = -89.75; hi = 0.0; return true; }
    double gainDb() const override { return gain_; }
    bool setGainDb(double db) override { gain_ = db; return true; }
    std::size_t write(const std::complex<float>* /*samples*/, std::size_t n) override {
        return running_ ? n : 0;
    }
    bool faulted() const override { return false; }
    const char* name() const override { return "test radio"; }
    const char* lastError() const override { return ""; }

    std::atomic<int> starts{0};

private:
    double rate_ = 2.5e6;
    double freq_ = 146.52e6;
    double gain_ = 0.0;
    std::atomic<bool> running_{false};
};

bool waitQuiet(const RecordingSink* s, int ms) {
    const auto t0 = std::chrono::steady_clock::now();
    while (s->running() && std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(ms)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return !s->running();
}

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
    static bool latched(Engine& e) { return e.transmitter_.latched(); }
    static bool transmitting(Engine& e) { return e.transmitter_.transmitting(); }
    static std::uint64_t passes(Engine& e) { return e.controlPasses_.load(); }
};
}  // namespace cascade::gui

using Access = cascade::gui::AppWindowTestAccess;

using tx_test::FrontEnd;

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_transmit_key_race\n");
    isolate();
    constexpr int kRounds = 8;

    // A and B: a key-down and its key-up inside one pass, the engine pumped
    // by hand (stage 3a's shape: nothing between the frames but the pass).
    for (int latch = 0; latch < 2; ++latch) {
        Engine e;
        e.initialise();
        RecordingSink* raw = Access::installSink(e);
        FrontEnd fe(e);
        int keyed = 0;
        for (int round = 0; round < kRounds; ++round) {
            fe.frame(true, false, false);
            e.pump();
            if (latch == 0) {
                fe.frame(true, true, false);   // PTT down...
                fe.frame(true, false, false);  // ...and up, before the pass
            } else {
                fe.frame(true, false, true);   // LATCH pressed...
                fe.frame(false, false, false); // ...and the page closes, before the pass
            }
            e.pump();
            if (!waitQuiet(raw, 50) || Access::transmitting(e) || Access::latched(e)) { ++keyed; }
            e.pump();
            std::this_thread::sleep_for(std::chrono::milliseconds(latch == 0 ? 5 : 1100));  // B: past the re-arm
        }
        std::printf("%s: %d rounds, keyed after the pass in %d, radio started %d times\n",
                    latch == 0 ? "A (PTT down and up in one frame)" : "B (LATCH pressed, page closed)", kRounds,
                    keyed, raw->starts.load());
        CHECK(keyed == 0);
        CHECK(raw->starts.load() == 0);
    }

    // C: the hammer - the control thread free-running.
    {
        Engine e;
        e.initialise();
        RecordingSink* raw = Access::installSink(e);
        cascade::engine::ControlThreadOptions opts;
        opts.period = std::chrono::milliseconds(1);
        opts.watchdogThresholdMs = 5000;
        e.startControlThread(opts);
        FrontEnd fe(e);
        constexpr int kHammer = 1000;
        int undone = 0;
        for (int i = 0; i < kHammer; ++i) {
            fe.frame(true, true, false);
            const std::uint64_t p0 = Access::passes(e);
            while (Access::passes(e) < p0 + 1) { std::this_thread::yield(); }
            fe.frame(true, false, false);
            const std::uint64_t p1 = Access::passes(e);
            while (Access::passes(e) < p1 + 2) { std::this_thread::yield(); }
            // Up by the second pass after the PTT 0, and it stays up.
            if (Access::transmitting(e)) { ++undone; }
        }
        e.stopControlThread();
        std::printf("C: %d key-down/key-up cycles against a free-running control thread: still keyed two passes "
                    "after the key-up %d times\n",
                    kHammer, undone);
        CHECK(undone == 0);
        CHECK(waitQuiet(raw, 150));
    }

    // D: the frozen window.
    {
        Engine e;
        e.initialise();
        RecordingSink* raw = Access::installSink(e);
        cascade::engine::ControlThreadOptions opts;
        opts.period = std::chrono::milliseconds(5);
        opts.watchdogThresholdMs = 5000;
        e.startControlThread(opts);
        FrontEnd fe(e);
        // A key request is judged against the PUBLISHED state: until a pass
        // has published the transmitter open, a PTT is refused NO_DEVICE.
        {
            const std::uint64_t p0 = Access::passes(e);
            while (Access::passes(e) < p0 + 2) { std::this_thread::yield(); }
        }
        fe.frame(true, true, false);
        CHECK([&] {
            const auto t0 = std::chrono::steady_clock::now();
            while (!raw->running() && std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(500)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            return raw->running();
        }());
        // The clock starts at the LAST HEARTBEAT, the event the keep-alive
        // counts from; the frame that made it is the front end's last.
        const auto lastBeat = std::chrono::steady_clock::now();
        fe.frame(true, true, false);
        const bool released = [&] {
            const auto t0 = std::chrono::steady_clock::now();
            while (raw->running() && std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(3000)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            return !raw->running();
        }();
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - lastBeat).count();
        const int startsAtRelease = raw->starts.load();
        std::this_thread::sleep_for(std::chrono::milliseconds(300));   // the control side runs on, alone
        std::printf("D: front end frozen with the PTT held: released %.3f ms after its last heartbeat; radio started "
                    "%d more times in the next 300 ms\n",
                    ms, raw->starts.load() - startsAtRelease);
        CHECK(released);
        // The keep-alive is 1000 ms, judged once a pass (5 ms here) and by the
        // TX thread once a 10 ms block; 150 ms covers both and a scheduler
        // tick of Windows' 15.6 ms several times over.
        //
        // The LOWER bound needs no millisecond of slack for truncation. Both
        // judges - TxKeyRules::tick and the TX thread's front-end check - work
        // in whole milliseconds, a stamp and a "now" each the FLOOR of
        // steady_clock (the host's steadyMs() and the Transmitter's nowMs()
        // are the same expression), and release only when
        // floor(now) - floor(stamp) > 1000, that is >= 1001 whole ms. Since
        // floor(stamp) > stamp - 1, that is now - stamp > 1000 ms of the real
        // clock, and the stamp is taken after lastBeat (the heartbeat is the
        // first thing frame() does) while ms is read after the release was
        // seen: ms > 1000 always. A strict ">" over two floors cannot lose the
        // millisecond a ">=" would (test_transmit_dead_man's case 6 allows
        // 1 ms; it is not needed here). The value printed above shows the
        // margin: 40 consecutive runs measured 1004.2 to 1031.9 ms.
        CHECK(ms >= 1000.0 && ms <= 1150.0);
        CHECK(raw->starts.load() == startsAtRelease);
        e.stopControlThread();
    }

    // E: a press after a key-up the TX thread played down itself.
    {
        Engine e;
        e.initialise();
        RecordingSink* raw = Access::installSink(e);
        FrontEnd fe(e);
        // One pass first: a key request is judged against the PUBLISHED
        // state, and until a pass has published it no transmitter is open.
        fe.frame(true, false, false);
        e.pump();
        int kept = 0;
        constexpr int kRoundsE = 3;
        for (int round = 0; round < kRoundsE; ++round) {
            fe.frame(true, false, true);  // LATCH on
            e.pump();
            fe.frame(true, false, false);
            e.pump();
            CHECK(Access::latched(e) && raw->running());
            fe.frame(true, false, true);  // shown latched: this press releases
            e.pump();
            CHECK(waitQuiet(raw, 150));   // the TX thread plays the key down and ends
            std::this_thread::sleep_for(std::chrono::milliseconds(FOXAPI_LATCH_REARM_MS + 50));
            fe.frame(true, false, false);
            e.pump();
            fe.frame(true, false, true);  // pressed again: latches
            e.pump();
            fe.frame(true, false, false);
            e.pump();
            if (Access::latched(e) && Access::transmitting(e) && raw->running()) { ++kept; }
            fe.frame(true, false, true);  // ...and off again, for the next round
            e.pump();
            CHECK(waitQuiet(raw, 150));
            std::this_thread::sleep_for(std::chrono::milliseconds(FOXAPI_LATCH_REARM_MS + 50));
            fe.frame(true, false, false);
            e.pump();
        }
        std::printf("E: a LATCH press after a key-up the TX thread played down itself: kept and keyed in %d of %d "
                    "rounds\n",
                    kept, kRoundsE);
        CHECK(kept == kRoundsE);
    }

    // F: a press, a close and a reopen before the control side runs.
    {
        Engine e;
        e.initialise();
        RecordingSink* raw = Access::installSink(e);
        FrontEnd fe(e);
        fe.frame(true, false, false);
        e.pump();
        int latchedOnReopen = 0;
        constexpr int kRoundsF = 3;
        for (int round = 0; round < kRoundsF; ++round) {
            fe.frame(true, false, true);  // LATCH pressed on the open page...
            for (int closes = 0; closes <= round % 2; ++closes) {
                fe.frame(false, false, false);  // ...the page closes...
                fe.frame(true, false, false);   // ...and opens again, no press since
            }
            e.pump();  // the control side runs only now
            e.pump();
            if (Access::latched(e)) { ++latchedOnReopen; }
            std::this_thread::sleep_for(std::chrono::milliseconds(FOXAPI_LATCH_REARM_MS + 50));
        }
        std::printf("F: LATCH press, page closed and reopened before the control side ran: latched on the reopen "
                    "in %d of %d rounds, radio started %d times\n",
                    latchedOnReopen, kRoundsF, raw->starts.load());
        CHECK(latchedOnReopen == 0);
        CHECK(raw->starts.load() == 0);
        // And a press made on the REOPENED page is still a press: it latches.
        fe.frame(true, false, true);
        e.pump();
        fe.frame(true, false, false);
        e.pump();
        CHECK(Access::latched(e) && raw->running());
        fe.frame(false, false, false);
        e.pump();
        CHECK(waitQuiet(raw, 150));
    }

    // G: THE HAMMER (C's), with the browser's session in the path - the pump move's step P1 (F3), re-pointed in stage 5d from the engine's
    // pull of the servers' queues to the web server's engine session. The engine applies the session's commands in its own pass, so the key
    // rules are reached by the control thread alone; two producer threads flood the session (PTT true and false, RUN 0, volume) and the
    // server's session closes and opens again (the server stopping and starting), all the while the front end presses and lets go as in C.
    // No local session has consented to a remote key, so every browser PTT is refused and the front end's key-up stays up - C's invariant,
    // unchanged by the noise - and the rules were entered by no other thread (ApiHost::offPassKeyTouches).
    // tests/test_remote_control_pull.cpp holds the cases where the browser DOES key.
    {
        Engine e;
        e.initialise();
        RecordingSink* raw = Access::installSink(e);
        web_test::WebSeat seat(e);
        std::atomic<std::uint64_t> handed{0};
        cascade::engine::ControlThreadOptions opts;
        opts.period = std::chrono::milliseconds(1);
        opts.watchdogThresholdMs = 5000;
        e.startControlThread(opts);
        FrontEnd fe(e);
        std::atomic<bool> noise{true};
        std::vector<std::thread> producers;
        for (int t = 0; t < 2; ++t) {
            producers.emplace_back([&, t] {
                std::mt19937 rng(7u + static_cast<unsigned>(t));
                while (noise.load()) {
                    cascade::net::ControlRequest r;
                    switch (rng() % 4) {
                        case 0: r.transmitPtt = true; break;
                        case 1: r.transmitPtt = false; break;
                        case 2: r.running = false; break;
                        default: r.volume = static_cast<double>(rng() % 100) / 100.0; break;
                    }
                    seat.beat();
                    if (seat.push(r)) { handed.fetch_add(1); }
                    std::this_thread::sleep_for(std::chrono::microseconds(100 + rng() % 400));
                }
            });
        }
        std::thread flip([&] {
            while (noise.load()) {
                if (seat.isOpen()) {
                    seat.close();
                } else {
                    (void)seat.open(e);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(11));
            }
        });
        constexpr int kHammerG = 300;
        int undone = 0;
        for (int i = 0; i < kHammerG; ++i) {
            fe.frame(true, true, false);
            const std::uint64_t p0 = Access::passes(e);
            while (Access::passes(e) < p0 + 1) { std::this_thread::yield(); }
            fe.frame(true, false, false);
            const std::uint64_t p1 = Access::passes(e);
            while (Access::passes(e) < p1 + 2) { std::this_thread::yield(); }
            if (Access::transmitting(e)) { ++undone; }
        }
        noise.store(false);
        for (std::thread& t : producers) { t.join(); }
        flip.join();
        const std::uint64_t touches = e.api().offPassKeyTouches();
        e.stopControlThread();
        std::printf("G: %d key-down/key-up cycles with %llu requests handed to a flooded web session: still keyed two passes after "
                    "the key-up %d times; key-rule entries from other threads: %llu\n",
                    kHammerG, static_cast<unsigned long long>(handed.load()), undone,
                    static_cast<unsigned long long>(touches));
        CHECK(undone == 0);
        CHECK(touches == 0u);
        CHECK(handed.load() > 50u);
        CHECK(waitQuiet(raw, 150));
    }

    return testSummary("test_transmit_key_race");
}
