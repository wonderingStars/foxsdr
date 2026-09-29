// test_transmit_key_race.cpp - a key-up from the front end can never be undone
// by the control side, with the pump on a thread of its own (the stage 3b
// shape; docs/engine-stage3.md OPEN 7 (b), and the 3b-pre-end review's HIGH
// finding).
//
// THE RACE. Engine::pumpTransmitter (the control side) reads the Transmit
// page's newest request out of the latest-value slot, then writes the key it
// computed from it - setLatched, setPttHeld. Engine::submitTransmitPageKey
// (the front end) applies a KEY-UP at once, with its own stores. If the front
// end's key-up lands between the control side's read and its write, the write
// puts back what the older request said: the PTT held again, or the latch
// closed again (restarting its one-minute failsafe too) - and the TX thread
// keeps transmitting until the next pump, or for kKeyAliveWait if the control
// thread stalls right after that write.
//
// THE INTERLEAVING IS FORCED, not hoped for: the engine calls a test hook at
// exactly that point (Engine::txKeyInterleaveForTest_) - with the key-down
// read and computed, not yet written - and each round's key-down is one the
// control side has not acted on yet, as in 3b, where the front end presses
// and lets go between two pumps. The hook lets the
// front-end thread submit its key-up and waits for it to finish - or, when the
// key-up cannot get in while the control side is between read and write (which
// is the fix), for a bounded 200 ms, after which the control side carries on
// and the key-up lands after it.
//   A  PTT held; the page, still live, lets go of the PTT in the window - after
//      the pump and the key-up, the PTT is not held and the radio goes quiet.
//      Every one of kRounds rounds.
//   B  LATCHED; the page closes in the window - the latch is open and the radio
//      goes quiet. Every round.
//   C  THE HAMMER: the control side pumps on its own thread without pause (a
//      yield at the interleave point, as a descheduled thread would take),
//      while the front end keys down and up kHammer times; after every key-up
//      the key must stay up across the control side's next two pumps.
//   D  THE STALE REQUEST: the front end stops with the PTT held (a frozen
//      window) while the control side pumps on. The TX thread releases the
//      key at kKeyAliveWait; after that the control side must not key the
//      radio again from the same old request, pump after pump.
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
#include "engine/tx_page_key.hpp"
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

// The hook's state: the control side, at the interleave point, lets the front
// end go and waits (bounded) for it to be done.
struct Gate {
    std::atomic<bool> go{false};
    std::atomic<bool> done{false};
    std::atomic<int> waitedOut{0};
};

void gateHook(void* arg) {
    Gate* g = static_cast<Gate*>(arg);
    g->go.store(true);
    const auto t0 = std::chrono::steady_clock::now();
    while (!g->done.load()) {
        if (std::chrono::steady_clock::now() - t0 > std::chrono::milliseconds(200)) {
            ++g->waitedOut;   // the key-up could not get in: it lands after the write
            return;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
}

void yieldHook(void*) { std::this_thread::yield(); }

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
    static bool pttHeld(Engine& e) { return e.transmitter_.pttHeld(); }
    static bool latched(Engine& e) { return e.transmitter_.latched(); }
    static bool transmitting(Engine& e) { return e.transmitter_.transmitting(); }
    static void hook(Engine& e, void (*fn)(void*), void* arg) {
        e.txKeyInterleaveForTest_ = fn;
        e.txKeyInterleaveArgForTest_ = arg;
    }
};
}  // namespace cascade::gui

using Access = cascade::gui::AppWindowTestAccess;

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_transmit_key_race\n");
    isolate();
    constexpr int kRounds = 8;

    // A and B: the forced interleaving, kRounds rounds each.
    for (int latch = 0; latch < 2; ++latch) {
        Engine e;
        e.initialise();
        RecordingSink* raw = Access::installSink(e);
        cascade::gui::TxPageRequest r;
        std::uint64_t seq = 0;
        int undone = 0, keyedAfter = 0, waitedOut = 0;
        std::atomic<int> hookMissed{0};
        for (int round = 0; round < kRounds; ++round) {
            // Key down, from the front end: into the slot, for the control
            // side's next pump to act on...
            r.pageLive = true;
            r.pttHeld = latch == 0;
            if (latch != 0) { ++r.latchPressCount; }
            r.frameSeq = ++seq;
            e.submitTransmitPageKey(r);

            // ...and the key-up the front end makes WHILE that pump is between
            // reading the key-down and writing it.
            cascade::gui::TxPageRequest up = r;
            if (latch == 0) {
                up.pttHeld = false;    // A: PTT let go, page still live
            } else {
                up.pageLive = false;   // B: the page closes with the LATCH on
            }
            up.frameSeq = ++seq;

            Gate gate;
            Access::hook(e, &gateHook, &gate);
            std::thread frontEnd([&] {
                const auto t0 = std::chrono::steady_clock::now();
                while (!gate.go.load() && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(2)) {
                    std::this_thread::yield();
                }
                if (!gate.go.load()) { ++hookMissed; }
                e.submitTransmitPageKey(up);
                gate.done.store(true);
            });
            std::thread control([&] { e.pumpTransmitter(); });
            control.join();
            frontEnd.join();
            Access::hook(e, nullptr, nullptr);
            waitedOut += gate.waitedOut.load();

            const bool stillAsked = latch == 0 ? Access::pttHeld(e) : Access::latched(e);
            if (stillAsked) { ++undone; }
            if (!waitQuiet(raw, 150)) { ++keyedAfter; }
            e.pumpTransmitter();   // the control side catches up, as it would
            CHECK(waitQuiet(raw, 150));
            r = up;
            r.pageLive = true;
        }
        std::printf("%s: %d rounds, key-up undone in %d, radio still keyed 150 ms after the key-up in %d "
                    "(key-up held off until the write in %d)\n",
                    latch == 0 ? "A (PTT let go)" : "B (page closed, latched)", kRounds, undone, keyedAfter,
                    waitedOut);
        CHECK(hookMissed.load() == 0);   // every round really was interleaved
        CHECK(undone == 0);
        CHECK(keyedAfter == 0);
    }

    // C: the hammer.
    {
        Engine e;
        e.initialise();
        RecordingSink* raw = Access::installSink(e);
        Access::hook(e, &yieldHook, nullptr);
        std::atomic<bool> stop{false};
        std::atomic<std::uint64_t> pumps{0};
        std::thread control([&] {
            while (!stop.load()) {
                e.pumpTransmitter();
                ++pumps;
            }
        });
        constexpr int kHammer = 3000;
        cascade::gui::TxPageRequest r;
        r.pageLive = true;
        std::uint64_t seq = 0;
        int undone = 0;
        for (int i = 0; i < kHammer; ++i) {
            r.pttHeld = true;
            r.frameSeq = ++seq;
            e.submitTransmitPageKey(r);
            // Let the control side apply it (or not - either way the key-up
            // below must stick).
            const std::uint64_t p0 = pumps.load();
            while (pumps.load() < p0 + 1) { std::this_thread::yield(); }
            r.pttHeld = false;
            r.frameSeq = ++seq;
            e.submitTransmitPageKey(r);
            const std::uint64_t p1 = pumps.load();
            bool seen = false;
            while (pumps.load() < p1 + 2) {
                if (Access::pttHeld(e)) { seen = true; }
                std::this_thread::yield();
            }
            if (seen || Access::pttHeld(e)) { ++undone; }
        }
        stop.store(true);
        control.join();
        Access::hook(e, nullptr, nullptr);
        e.pumpTransmitter();
        std::printf("C: %d key-down/key-up cycles against a free-running control thread: key-up undone "
                    "%d times\n",
                    kHammer, undone);
        CHECK(undone == 0);
        CHECK(waitQuiet(raw, 150));
    }

    // D: the stale request.
    {
        Engine e;
        e.initialise();
        RecordingSink* raw = Access::installSink(e);
        e.pumpTransmitter(/*pageLive=*/true, /*latchPressed=*/false, /*pttHeld=*/true);
        CHECK(raw->running());
        const auto t0 = std::chrono::steady_clock::now();
        while (raw->running() && std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(3000)) {
            e.pumpTransmitter();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        CHECK(!raw->running());
        const int startsAtRelease = raw->starts.load();
        for (int i = 0; i < 60; ++i) {   // 300 ms more of the control side, alone
            e.pumpTransmitter();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        std::printf("D: front end frozen with the PTT held: released; radio started %d more times in the "
                    "next 60 pumps\n",
                    raw->starts.load() - startsAtRelease);
        CHECK(raw->starts.load() == startsAtRelease);
        CHECK(!Access::transmitting(e));
        // A new frame from the front end with the PTT still held keys it again:
        // the window is back, and the hand on the PTT is a real one.
        e.pumpTransmitter(/*pageLive=*/true, /*latchPressed=*/false, /*pttHeld=*/true);
        std::printf("   the front end back, PTT held: transmitting=%d\n", Access::transmitting(e) ? 1 : 0);
        CHECK(Access::transmitting(e));
        e.pumpTransmitter(/*pageLive=*/false, /*latchPressed=*/false, /*pttHeld=*/false);
        CHECK(waitQuiet(raw, 150));
    }

    return testSummary("test_transmit_key_race");
}
