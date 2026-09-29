// test_transmit_liveness.cpp - the transmitter's two liveness stamps and the
// remote key's hold, each enforced by the TX thread itself (engine/stage3b-pre,
// docs/engine-stage3.md OPEN 7).
//
// WHY TWO STAMPS. Until this round one stamp, stamped by tick(), stood for
// "the window is alive", because tick() ran in the window's frame loop. Stage
// 3b moves tick() to the control thread, and then a frozen WINDOW no longer
// stops it - the dead-man's handle would silently become "a frozen control
// thread". So the TX thread now watches two:
//   - the CONTROL stamp (tick()): stale for kKeyAliveWait, ANY key opens -
//     nothing is left to apply a release;
//   - the FRONT-END stamp (frontEndAlive(), one atomic store a frame from the
//     GUI thread): stale for kKeyAliveWait while a LOCAL key (PTT or LATCH) is
//     asserted, the key opens - the hand on that key is on a window that has
//     stopped. A REMOTE key does not depend on the window, and is not dropped
//     by it;
// and the remote key's own hold (kRemotePttHoldMs) is enforced by the TX thread
// too, not by tick(), so it holds whoever is or is not ticking.
//
//   A  control ticking, front end stops, PTT held -> released within
//      kKeyAliveWait of the last front-end stamp, plus a block and slack
//   B  the same with the LATCH
//   C  control ticking, front end never stamped, REMOTE keyed once -> still
//      keyed well past kKeyAliveWait, released at the 2 s hold
//   D  front end alive, control stops, PTT held -> released within
//      kKeyAliveWait (the control stamp still rules)
//   E  both alive, PTT held for 1.5 s -> never released (no false alarm)
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <complex>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>

#include "core/transmitter.hpp"
#include "source/tx_sink.hpp"
#include "test_check.hpp"

using cascade::core::Transmitter;
using cascade::core::TxInput;
using Clock = std::chrono::steady_clock;

namespace {

// A radio that is only a ledger.
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

double msSince(Clock::time_point t) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}

// Runs for `ms`, ticking the control side every 5 ms when `control`, stamping
// the front end when `front`. Returns the time (ms from `from`) the radio was
// first seen silent, or -1 if it never was.
double watch(Transmitter& tx, RecordingSink* raw, bool control, bool front, int ms, Clock::time_point from) {
    const auto until = Clock::now() + std::chrono::milliseconds(ms);
    while (Clock::now() < until) {
        if (control) { tx.tick(); }
        if (front) { tx.frontEndAlive(); }
        if (!raw->running()) { return msSince(from); }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return raw->running() ? -1.0 : msSince(from);
}

struct Rig {
    Transmitter tx;
    RecordingSink* raw = nullptr;
    Rig() {
        auto sink = std::make_unique<RecordingSink>();
        raw = sink.get();
        tx.setSink(std::move(sink));
        tx.setInput(TxInput::Tone);
    }
};

// One block (10 ms at 48 kHz) plus scheduling slack for a loaded CI machine.
constexpr double kSlackMs = 150.0;
const double kAlive = static_cast<double>(Transmitter::kKeyAliveWait.count());
const double kHold = static_cast<double>(Transmitter::kRemotePttHoldMs.count());

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_transmit_liveness\n");

    // --- A and B: the control ticks on, the front end stops, a LOCAL key --------
    for (int latch = 0; latch < 2; ++latch) {
        Rig r;
        if (latch != 0) {
            r.tx.setLatched(true);
        } else {
            r.tx.setPttHeld(true);
        }
        r.tx.frontEndAlive();
        r.tx.tick();
        CHECK(r.tx.transmitting());
        CHECK(r.raw->running());
        (void)watch(r.tx, r.raw, true, true, 300, Clock::now());   // a live window, for a while
        CHECK(r.raw->running());
        const auto lastFront = Clock::now();
        r.tx.frontEndAlive();
        // The window freezes; the control side does not.
        const double at = watch(r.tx, r.raw, true, false, 3000, lastFront);
        std::printf("%s: %s held, front end stopped - released %.0f ms after its last stamp "
                    "(bound %.0f + %.0f)\n",
                    latch != 0 ? "B" : "A", latch != 0 ? "LATCH" : "PTT", at, kAlive, kSlackMs);
        CHECK(at >= kAlive - 20.0);             // not before the bound...
        CHECK(at >= 0.0 && at <= kAlive + kSlackMs);   // ...and within it (plus a block)
        r.tx.tick();
        CHECK(!r.tx.transmitting());
        const std::string why = r.tx.lastAutoUnkeyReason();
        std::printf("   \"%s\"\n", why.c_str());
        CHECK(why.find("window") != std::string::npos);
    }

    // --- C: the control ticks on, the front end never stamps, a REMOTE key ------
    {
        Rig r;
        const auto t0 = Clock::now();
        r.tx.keyRemote();
        r.tx.tick();
        CHECK(r.tx.transmitting());
        // A remote key is not the window's: its going quiet does not drop it.
        const double early = watch(r.tx, r.raw, true, false, 1500, t0);
        CHECK(early < 0.0);
        const double at = watch(r.tx, r.raw, true, false, 2000, t0);
        std::printf("C: remote keyed once, front end silent - released %.0f ms after the "
                    "assertion (hold %.0f)\n",
                    at, kHold);
        CHECK(at >= kHold - 20.0 && at <= kHold + kSlackMs);
        r.tx.tick();
        CHECK(!r.tx.transmitting());
        CHECK(!r.tx.remoteKeyed());
        CHECK(r.tx.lastAutoUnkeyReason().find("remote") != std::string::npos);
    }

    // --- D: the front end alive, the control stops, a LOCAL key ---------------------
    {
        Rig r;
        r.tx.setPttHeld(true);
        r.tx.frontEndAlive();
        r.tx.tick();
        CHECK(r.tx.transmitting());
        const auto lastTick = Clock::now();
        const double at = watch(r.tx, r.raw, false, true, 3000, lastTick);
        std::printf("D: PTT held, control stopped - released %.0f ms after the last tick\n", at);
        CHECK(at >= kAlive - 20.0 && at <= kAlive + kSlackMs);
    }

    // --- E: both alive: no false alarm ------------------------------------------------
    {
        Rig r;
        r.tx.setPttHeld(true);
        r.tx.frontEndAlive();
        r.tx.tick();
        const double at = watch(r.tx, r.raw, true, true, 1500, Clock::now());
        std::printf("E: both alive for 1.5 s - %s\n", at < 0.0 ? "still keyed" : "RELEASED");
        CHECK(at < 0.0);
        CHECK(r.tx.transmitting());
        r.tx.setPttHeld(false);
        r.tx.tick();
    }

    return testSummary("test_transmit_liveness");
}
