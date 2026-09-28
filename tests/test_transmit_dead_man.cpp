// test_transmit_dead_man.cpp - the Transmit page's dead-man's handle, through
// the Engine's own pumpTransmitter (engine extraction stage 3a/3b-pre,
// docs/engine-stage3.md OPEN 7).
//
// tests/test_transmit_page.cpp proves WHERE the key's application lives (a
// source-text scan: transmitter_.setLatched/setPttHeld/tick() are lines
// inside Engine::pumpTransmitter and nowhere in app_window.cpp). It cannot
// tell a correct pumpTransmitter from one that still contains those three
// lines but runs them wrong - `if (pageLive) transmitter_.tick();` still
// matches its "tick() is a line in pumpTransmitter" check, because the text
// is still there. THIS test drives a real Engine with a fake sink behind its
// transmitter_ and reads what actually happened to the sink, which is what
// tells the two apart: with the mutant, closing the Transmit page (pageLive
// false) stops calling tick() forever, so the key that was set to "released"
// by txPageKey is never carried out, and the radio stays keyed until the TX
// thread's own kKeyAliveWait timeout eventually notices tick() stopped -
// which is not IMMEDIATE (docs/engine-stage3.md OPEN 7's own words: "a
// frozen WINDOW no longer stops tick()", though here it is a page closing
// while the window runs fine).
//
// THE RULE THIS FILE HOLDS: closing the Transmit page - which is what
// pageLive=false MEANS, the page not drawn this frame - releases the key
// within the SAME pump that reports it closed, whether the key was a LATCH
// or a held PTT. See core/transmitter.hpp for why "not at once" is not good
// enough: every dead-man's handle in this product is a bound on how long a
// stopped hand can keep a radio on the air, and the ordinary path (the page
// really did close, the window is running fine) has no excuse to wait for
// the emergency bound.
//
// Also covers, as a regression net through the SAME Engine-owned
// transmitter_ rather than a standalone one (tests/test_transmitter.cpp
// covers the mechanism itself in full): the remote PTT's own hold - keyRemote()
// buys kRemotePttHoldMs and no more, unaffected by pumpTransmitter never being
// told about a remote key at all.
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

#include "core/transmitter.hpp"
#include "engine/engine.hpp"
#include "test_check.hpp"

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

// The friend the Engine names for its tests (engine.hpp). Only used here to
// reach transmitter_ directly - installing the fake sink, reading
// transmitting(), and driving keyRemote()/tick() for the remote-hold check,
// which pumpTransmitter never touches (a remote key is not a page control).
// Every other assertion goes through Engine's PUBLIC pumpTransmitter, which
// is the surface the mutant lives in.
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
    static void keyRemote(Engine& e) { e.transmitter_.keyRemote(); }
    static void tick(Engine& e) { e.transmitter_.tick(); }
};

}  // namespace cascade::gui

using Access = cascade::gui::AppWindowTestAccess;

int main() {
    std::printf("test_transmit_dead_man\n");
    isolate();

    // =========================================================================
    // 1. LATCHED, THEN THE PAGE CLOSES -> released within ONE pump.
    // =========================================================================
    {
        Engine e;
        e.initialise();
        RecordingSink* raw = Access::installSink(e);

        // Page live, LATCH pressed once: latches on and keys.
        e.pumpTransmitter(/*pageLive=*/true, /*latchPressed=*/true, /*pttHeld=*/false);
        CHECK(Access::latched(e));
        CHECK(Access::transmitting(e));
        CHECK(raw->running());
        std::printf("[1a] latched and keyed: transmitting=%d running=%d\n",
                    Access::transmitting(e), raw->running());

        // The page closes - ONE pump reporting pageLive=false, nothing else.
        e.pumpTransmitter(/*pageLive=*/false, /*latchPressed=*/false, /*pttHeld=*/false);
        std::printf("[1b] one pump after the page closed: transmitting=%d running=%d "
                    "latched=%d\n",
                    Access::transmitting(e), raw->running(), Access::latched(e));
        CHECK(!Access::latched(e));
        CHECK(!Access::transmitting(e));
        CHECK(!raw->running());
        // STOPPED AT ONCE, not the emergency path: a page closing in the
        // ordinary way is not a fault, so this is finish() (a clean end),
        // never the raw stop() a fault or the frozen-window handle uses.
        CHECK(raw->finishes.load() >= 1);
    }

    // =========================================================================
    // 2. PTT HELD, THEN THE PAGE CLOSES -> released within ONE pump.
    // =========================================================================
    {
        Engine e;
        e.initialise();
        RecordingSink* raw = Access::installSink(e);

        e.pumpTransmitter(/*pageLive=*/true, /*latchPressed=*/false, /*pttHeld=*/true);
        CHECK(Access::transmitting(e));
        CHECK(raw->running());
        std::printf("[2a] PTT held and keyed: transmitting=%d running=%d\n",
                    Access::transmitting(e), raw->running());

        e.pumpTransmitter(/*pageLive=*/false, /*latchPressed=*/false, /*pttHeld=*/false);
        std::printf("[2b] one pump after the page closed: transmitting=%d running=%d\n",
                    Access::transmitting(e), raw->running());
        CHECK(!Access::transmitting(e));
        CHECK(!raw->running());
    }

    // =========================================================================
    // 3. THE REMOTE KEY'S OWN HOLD, through the Engine-owned transmitter_:
    //    keyRemote() buys kRemotePttHoldMs and no more. pumpTransmitter is
    //    never told about a remote key (its three parameters are pageLive,
    //    latchPressed, pttHeld - none of them "remote"), so this checks the
    //    hold survives being owned by an Engine rather than a standalone
    //    Transmitter (the mechanism itself is tests/test_transmitter.cpp's).
    // =========================================================================
    {
        Engine e;
        e.initialise();
        RecordingSink* raw = Access::installSink(e);

        Access::keyRemote(e);
        Access::tick(e);
        CHECK(Access::transmitting(e));
        CHECK(raw->running());

        // Nothing refreshes it. Past the hold, a tick lets it go - well
        // before the frozen-window bound this is not (kRemotePttHoldMs is
        // 2000 ms here; kKeyAliveWait, the frozen-window bound, is 1000 ms -
        // the remote hold is the LONGER of the two on purpose, see
        // transmitter.hpp: a remote hand has a network's latency to answer
        // through, a local one does not).
        std::this_thread::sleep_for(Transmitter::kRemotePttHoldMs + std::chrono::milliseconds(300));
        Access::tick(e);
        std::printf("[3] remote hold elapsed: transmitting=%d running=%d\n",
                    Access::transmitting(e), raw->running());
        CHECK(!Access::transmitting(e));
        CHECK(!raw->running());
    }

    return testSummary("test_transmit_dead_man");
}
