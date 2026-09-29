// test_transmit_dead_man.cpp - the Transmit page's dead-man's handles, end to
// end through a real Engine (engine stage 4, API 0.2's THE KEY; before it,
// stage 3a/3b-pre's pumpTransmitter, docs/engine-stage3.md OPEN 7).
//
// tests/test_tx_key_rules.cpp proves the rules on their own, against an
// injected clock; tests/test_engine_api_transmit.cpp proves them through the
// table against the reference's own safety suite. THIS file drives what the
// window does - a LOCAL session, its heartbeat, gui::txKeyCommands - into a
// real Engine with a fake sink behind its transmitter_, and reads what
// actually happened to the SINK, which is what tells "the rules decided to
// release" from "the radio stopped":
//
//   1  LATCHED, then the page closes: released by the pass that applies the
//      close - with its tail (finish()), a clean end, not a cut.
//   2  PTT held, then the page closes: released by that pass.
//   3  THE WEB REMOTE'S HOLD: a remote PTT buys FOXAPI_PTT_HOLD_MS and
//      no more, however often the browser polls.
//   4  KEY-UP: a PTT 0 is quiet within a block or two of the pass that
//      applies it; and a control side that STOPS passing (late, or stuck)
//      cannot hold the radio on - the TX thread lets go at kKeyAliveWait
//      from the last pass, although the front end beats on.
//   5  THE LATCH IS EDGE-TRIGGERED: one press latches once however many
//      passes see it; a press on a key shown latched releases; a press
//      inside the 1 s re-arm after an end latches nothing.
//   6  THE FRONT END STOPS, the control side passes on: released at the 1 s
//      keep-alive, counted from the front end's last heartbeat.
//   7  THE REMOTE KEY'S STANDING CONDITION is the local operator's consent:
//      withdrawn, or its session closed, or its session silent past the
//      keep-alive - each releases a remote key held.
//   8  AND THE OTHER WAY: front end beating, control side passing - a latch
//      stays keyed well past every 1 s bound (the heartbeat is what tells
//      the TX thread the front end is alive; a latch is never re-asserted).
//
// Timing: every clock starts at the event the product measures from (the
// pass that applies a command, the last heartbeat, the last pass), taken
// BEFORE that event, so a lower bound holds exactly; the upper bounds allow
// 150 ms - a pass here is by hand every 5 ms and the TX thread judges once a
// 10 ms block, and 150 ms is several of Windows' 15.6 ms scheduler ticks.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <functional>
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
#include "engine/fox_engine_api.hpp"
#include "test_check.hpp"
#include "tx_front_end.hpp"

using cascade::core::Transmitter;
using cascade::core::TxInput;
using cascade::engine::Engine;
using tx_test::FrontEnd;

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
#else
    const int pid = static_cast<int>(getpid());
#endif
    const std::string s =
        (std::string("foxsdr_tx_deadman_") + std::to_string(pid));
    std::string tmp;
#if defined(_WIN32)
    char buf[MAX_PATH];
    GetTempPathA(MAX_PATH, buf);
    tmp = std::string(buf) + s;
#else
    tmp = "/tmp/" + s;
#endif
    for (const char* v : {"LOCALAPPDATA", "APPDATA", "USERPROFILE", "HOME", "XDG_CONFIG_HOME",
                          "XDG_STATE_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"}) {
        setEnv(v, tmp);
    }
    setEnv("FOXSDR_TELEMETRY_URL", "http://127.0.0.1:9");
}

// A radio that is only a ledger - the same minimal shape as
// test_transmitter.cpp's RecordingSink, just the pieces this file reads.
class RecordingSink : public cascade::source::IqSink {
public:
    bool start() override {
        ++starts;
        running_ = true;
        return true;
    }
    void stop() override {
        if (running_) { ++stops; }
        running_ = false;
    }
    void finish() override {
        if (running_) { ++finishes; }
        stop();
    }
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
        if (!running_) { return 0; }
        samples_ += n;
        return n;
    }
    bool faulted() const override { return false; }
    const char* name() const override { return "test radio"; }
    const char* lastError() const override { return error_.c_str(); }

    std::atomic<int> starts{0};
    std::atomic<int> stops{0};
    std::atomic<int> finishes{0};

private:
    double rate_ = 2.5e6;
    double freq_ = 146.52e6;
    double gain_ = 0.0;
    std::atomic<bool> running_{false};
    std::atomic<std::size_t> samples_{0};
    std::string error_;
};

}  // namespace

namespace cascade::gui {

// The friend the Engine names for its tests (engine.hpp): only to install
// the fake sink and read the transmitter's own state.
struct AppWindowTestAccess {
    static RecordingSink* installSink(Engine& e) {
        auto sink = std::make_unique<RecordingSink>();
        RecordingSink* raw = sink.get();
        e.transmitter_.setSink(std::move(sink));
        e.transmitter_.setInput(TxInput::Tone);
        return raw;
    }
    static bool transmitting(Engine& e) { return e.transmitter_.transmitting(); }
    static bool latched(Engine& e) { return e.transmitter_.latched(); }
    static bool remoteKeyed(Engine& e) { return e.transmitter_.remoteKeyed(); }
};

}  // namespace cascade::gui

using Access = cascade::gui::AppWindowTestAccess;

namespace {

using Clock = std::chrono::steady_clock;

double msSince(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

// Runs `step` every 5 ms until the radio stops or `limitMs` passes.
bool untilQuiet(const RecordingSink* raw, int limitMs, const std::function<void()>& step) {
    const auto t0 = Clock::now();
    while (raw->running() && Clock::now() - t0 < std::chrono::milliseconds(limitMs)) {
        step();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return !raw->running();
}

bool waitQuiet(const RecordingSink* raw, int limitMs) {
    return untilQuiet(raw, limitMs, [] {});
}

// An Engine with its fake radio, one pass run so a transmitter is PUBLISHED
// open (a key request is judged against the published state).
struct Rig {
    Rig() {
        e.initialise();
        raw = Access::installSink(e);
        fe.frame(true, false, false);
        e.pump();
    }
    Engine e;
    RecordingSink* raw = nullptr;
    FrontEnd fe{e};
};

bool ok(const FoxCommandResult& r) { return r.status == FOXAPI_OK && (r.flags & FOXAPI_RESULT_REFUSED) == 0u; }

// The browser page's PTT, as the web server's control request reaches the
// engine (Engine::applyCommand's FOXAPI_OP_TX_PTT).
// The page polls its status (the web remote's keep-alive) before and while
// it presses.
FoxCommandResult webPtt(Engine& e, bool on) {
    e.api().webRemoteBeat();
    return e.applyCommand(cascade::core::cmd::makeInt(FOXAPI_OP_TX_PTT, on ? 1 : 0));
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_transmit_dead_man\n");
    isolate();

    // =========================================================================
    // 1. LATCHED, THEN THE PAGE CLOSES -> released by ONE pass.
    // =========================================================================
    {
        Rig r;
        r.fe.frame(true, false, true);  // LATCH pressed
        r.e.pump();
        CHECK(Access::latched(r.e));
        CHECK(r.raw->running());
        r.fe.frame(true, false, false);  // the state now shows it latched, mine
        r.e.pump();
        r.fe.frame(false, false, false);  // the page closes
        r.e.pump();                       // ONE pass
        std::printf("[1] latched, page closed, one pass: latched=%d transmitting=%d\n", Access::latched(r.e),
                    Access::transmitting(r.e));
        CHECK(!Access::latched(r.e));
        CHECK(!Access::transmitting(r.e));
        CHECK(waitQuiet(r.raw, 150));
        // A page closing in the ordinary way is not a fault: finish() (a
        // clean end, the tail played), never the raw stop() of a fault.
        CHECK(r.raw->finishes.load() >= 1);
    }

    // =========================================================================
    // 2. PTT HELD, THEN THE PAGE CLOSES -> released by ONE pass.
    // =========================================================================
    {
        Rig r;
        r.fe.frame(true, true, false);
        r.e.pump();
        CHECK(Access::transmitting(r.e));
        CHECK(r.raw->running());
        r.fe.frame(false, false, false);
        r.e.pump();
        std::printf("[2] PTT held, page closed, one pass: transmitting=%d\n", Access::transmitting(r.e));
        CHECK(!Access::transmitting(r.e));
        CHECK(waitQuiet(r.raw, 150));
    }

    // =========================================================================
    // 3. THE WEB REMOTE'S HOLD: a remote PTT buys FOXAPI_PTT_HOLD_MS and
    //    no more - the browser keeps polling (its keep-alive), the local
    //    operator keeps consenting, nothing re-asserts the PTT.
    // =========================================================================
    {
        Rig r;
        r.fe.frame(true, false, false, true, 1);  // "Allow remote transmit"
        r.e.pump();
        const auto t0 = Clock::now();  // before the apply: the hold counts from it
        CHECK(ok(webPtt(r.e, true)));
        r.e.pump();
        CHECK(Access::remoteKeyed(r.e));
        CHECK(r.raw->running());
        const bool released = untilQuiet(r.raw, 3000, [&] {
            r.e.api().webRemoteBeat();
            r.fe.frame(true, false, false);
            r.e.pump();
        });
        const double ms = msSince(t0);
        std::printf("[3] remote PTT, not re-asserted: released %.0f ms after the press\n", ms);
        CHECK(released);
        CHECK(ms >= static_cast<double>(FOXAPI_PTT_HOLD_MS));
        CHECK(ms <= static_cast<double>(FOXAPI_PTT_HOLD_MS) + 150.0);
    }

    // =========================================================================
    // 4a. KEY-UP IS QUIET within a block or two of the pass that applies it,
    //     with its tail.
    // =========================================================================
    {
        Rig r;
        r.fe.frame(true, true, false);
        r.e.pump();
        CHECK(r.raw->running());
        const int finishes = r.raw->finishes.load();
        r.fe.frame(true, false, false);  // PTT let go
        const auto t0 = Clock::now();
        r.e.pump();
        const bool quiet = waitQuiet(r.raw, 1500);
        const double ms = msSince(t0);
        std::printf("[4a] PTT let go: radio quiet %.0f ms after the pass\n", ms);
        CHECK(quiet);
        CHECK(ms < 150.0);  // a block or two, never the 1000 ms bound
        CHECK(r.raw->finishes.load() > finishes);
    }

    // =========================================================================
    // 4b. THE CONTROL SIDE STOPS PASSING, the front end beats on with the PTT
    //     held: the TX thread lets go at kKeyAliveWait from the LAST PASS.
    // =========================================================================
    {
        Rig r;
        r.fe.frame(true, true, false);
        r.e.pump();
        CHECK(r.raw->running());
        r.fe.frame(true, true, false);
        const auto lastPass = Clock::now();  // before it: the stamp is taken inside
        r.e.pump();
        const bool released = untilQuiet(r.raw, 3000, [&] { r.fe.frame(true, true, false); });
        const double ms = msSince(lastPass);
        std::printf("[4b] no pass since, front end beating: released %.0f ms after the last pass\n", ms);
        CHECK(released);
        // Stamps are whole milliseconds, so the bound's own rounding is 1 ms.
        CHECK(ms >= static_cast<double>(Transmitter::kKeyAliveWait.count()) - 1.0);
        CHECK(ms <= static_cast<double>(Transmitter::kKeyAliveWait.count()) + 150.0);
    }

    // =========================================================================
    // 5. THE LATCH IS EDGE-TRIGGERED.
    // =========================================================================
    {
        Rig r;
        // One press, then several passes: on once, not on-and-off.
        r.fe.frame(true, false, true);
        r.e.pump();
        r.e.pump();
        r.e.pump();
        std::printf("[5a] one press, three passes: latched=%d\n", Access::latched(r.e));
        CHECK(Access::latched(r.e));
        CHECK(r.raw->running());
        // A press on the key shown latched releases it.
        r.fe.frame(true, false, false);
        r.fe.frame(true, false, true);
        r.e.pump();
        std::printf("[5b] press on a key shown latched: latched=%d\n", Access::latched(r.e));
        CHECK(!Access::latched(r.e));
        CHECK(waitQuiet(r.raw, 150));
        // Inside the 1 s re-arm a press latches nothing...
        const int starts = r.raw->starts.load();
        r.fe.frame(true, false, false);
        r.fe.frame(true, false, true);
        r.e.pump();
        std::printf("[5c] press inside the re-arm: latched=%d\n", Access::latched(r.e));
        CHECK(!Access::latched(r.e));
        CHECK(r.raw->starts.load() == starts);
        // ...and past it, one does.
        std::this_thread::sleep_for(std::chrono::milliseconds(FOXAPI_LATCH_REARM_MS + 50));
        r.fe.frame(true, false, false);
        r.e.pump();
        r.fe.frame(true, false, true);
        r.e.pump();
        std::printf("[5d] press past the re-arm: latched=%d\n", Access::latched(r.e));
        CHECK(Access::latched(r.e));
        r.fe.frame(false, false, false);
        r.e.pump();
        CHECK(waitQuiet(r.raw, 150));
    }

    // =========================================================================
    // 6. THE FRONT END STOPS, the control side passes on: a held PTT is
    //    released at the keep-alive, counted from the LAST HEARTBEAT.
    // =========================================================================
    {
        Rig r;
        r.fe.frame(true, true, false);
        r.e.pump();
        CHECK(r.raw->running());
        const auto lastBeat = Clock::now();  // before it
        r.fe.frame(true, true, false);       // the front end's last frame
        const bool released = untilQuiet(r.raw, 3000, [&] { r.e.pump(); });
        const double ms = msSince(lastBeat);
        std::printf("[6] front end silent, control passing: released %.0f ms after its last heartbeat\n", ms);
        CHECK(released);
        CHECK(ms >= static_cast<double>(FOXAPI_KEEPALIVE_MS) - 1.0);
        CHECK(ms <= static_cast<double>(FOXAPI_KEEPALIVE_MS) + 150.0);
    }

    // =========================================================================
    // 7. THE REMOTE KEY'S STANDING CONDITION: the local operator's consent.
    // =========================================================================
    {
        // Without it, the browser's PTT is refused and nothing keys.
        Rig r;
        const FoxCommandResult refused = webPtt(r.e, true);
        r.e.pump();
        std::printf("[7a] no consent: status %d, remote keyed=%d\n", refused.status, Access::remoteKeyed(r.e));
        CHECK(refused.status == FOXAPI_DENIED);
        CHECK(!Access::remoteKeyed(r.e));
        CHECK(r.raw->starts.load() == 0);
    }
    for (int how = 0; how < 3; ++how) {
        static const char* const kHow[] = {"consent withdrawn", "consenting session closed",
                                           "consenting session silent"};
        Rig r;
        r.fe.frame(true, false, false, true, 1);
        r.e.pump();
        CHECK(ok(webPtt(r.e, true)));
        r.e.pump();
        CHECK(Access::remoteKeyed(r.e));
        CHECK(r.raw->running());
        auto t0 = Clock::now();
        bool released = false;
        if (how == 0) {
            r.fe.frame(true, false, false, true, 0);
            r.e.pump();
            released = !Access::remoteKeyed(r.e);
        } else if (how == 1) {
            r.fe.close();
            r.e.pump();
            released = !Access::remoteKeyed(r.e);
        } else {
            // The browser re-asserts and polls; the desk goes quiet. Its last
            // heartbeat was the arming frame's - the clock starts before the
            // one below, which is now its last.
            t0 = Clock::now();
            r.fe.beat();
            released = untilQuiet(r.raw, 3000, [&] {
                r.e.api().webRemoteBeat();
                (void)webPtt(r.e, true);
                r.e.pump();
            });
        }
        const double ms = msSince(t0);
        std::printf("[7%c] %s: remote keyed=%d after %.0f ms\n", 'b' + how, kHow[how], Access::remoteKeyed(r.e), ms);
        CHECK(released);
        CHECK(!Access::remoteKeyed(r.e));
        CHECK(waitQuiet(r.raw, 150));
        if (how == 2) {
            CHECK(ms >= static_cast<double>(FOXAPI_KEEPALIVE_MS) - 1.0);
            CHECK(ms <= static_cast<double>(FOXAPI_KEEPALIVE_MS) + 150.0);
        }
    }

    // =========================================================================
    // 8. BOTH ALIVE: a latch held for 2.5 s (past the keep-alive and the TX
    //    thread's own kKeyAliveWait, twice) never lets go, and the radio
    //    never restarts.
    // =========================================================================
    {
        Rig r;
        r.fe.frame(true, false, true);
        r.e.pump();
        CHECK(Access::latched(r.e));
        const auto t0 = Clock::now();
        bool dropped = false;
        while (Clock::now() - t0 < std::chrono::milliseconds(2500)) {
            r.fe.frame(true, false, false);
            r.e.pump();
            if (!r.raw->running()) { dropped = true; }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        std::printf("[8] both alive for 2.5 s: latched=%d radio dropped=%d starts=%d\n", Access::latched(r.e),
                    dropped ? 1 : 0, r.raw->starts.load());
        CHECK(Access::latched(r.e));
        CHECK(!dropped);
        CHECK(r.raw->starts.load() == 1);
        r.fe.frame(false, false, false);
        r.e.pump();
        CHECK(waitQuiet(r.raw, 150));
    }

    return testSummary("test_transmit_dead_man");
}
