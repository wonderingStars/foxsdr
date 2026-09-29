// test_pa_stream_list_guard.cpp - PortAudio's stream-list lock gives up only on
// a HOLDER that has stopped, never on a queue of healthy ones
// (sink/pa_init.hpp, "AND WHY IT CANNOT HANG ANYBODY").
//
// The guard's rule, as the header states it: a waiter goes ahead without the
// lock after kStreamListWaitMs because "a holder that has kept it that long is
// past the list". That is true of ONE holder that has kept it that long - a
// close hung inside the host API, which is after its list work. It is NOT true
// of a waiter that has waited that long behind several holders one after
// another, each of them healthy and each of them still inside its own list
// change when the waiter gives up. The guard measured the second (the waiter's
// own wait, one try_lock_for), so enough contention made two threads change
// PortAudio's unlocked list at once - which is what
// tests/test_soundcard_source.cpp's eight-thread stream-list probe saw
// ("1 found another inside the list") on Windows under a parallel ctest load,
// where each of the fake's short holds is a sleep the scheduler stretches.
//
//   A  a queue of healthy holders, each well inside kStreamListWaitMs, back to
//      back for longer than it: a waiter must NOT go ahead while one of them
//      holds the lock - it waits its turn (40 rounds)
//   B  one holder that stops (holds past kStreamListWaitMs): the waiter still
//      goes ahead after about kStreamListWaitMs, counted - the bound that makes
//      a hung close harmless is kept
//   C  and a waiter is never kept waiting without end: behind holders that
//      never stop coming, it goes ahead at kStreamListStarveMs, counted
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

#include "sink/pa_init.hpp"
#include "test_check.hpp"

using cascade::sink::PaStreamListGuard;
using Clock = std::chrono::steady_clock;

namespace {

double msSince(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

struct Outcome {
    bool held = false;
    bool holderInside = false;   // a holder was inside the lock when the waiter went ahead
    double waitedMs = 0.0;
};

// `holds` holds of `holdMs` each, back to back, on one thread; a waiter that
// starts once the first is taken. Returns what the waiter got.
Outcome race(int holds, int holdMs) {
    std::atomic<bool> inside{false};
    std::atomic<bool> first{false};
    std::atomic<bool> stop{false};
    std::thread holder([&] {
        for (int i = 0; i < holds && !stop.load(); ++i) {
            PaStreamListGuard g;
            inside.store(true);
            first.store(true);
            std::this_thread::sleep_for(std::chrono::milliseconds(holdMs));
            inside.store(false);
        }
    });
    while (!first.load()) { std::this_thread::yield(); }
    Outcome o;
    const auto t0 = Clock::now();
    {
        PaStreamListGuard g;
        o.waitedMs = msSince(t0);
        o.held = g.held();
        o.holderInside = inside.load();
        stop.store(true);
    }
    holder.join();
    return o;
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_pa_stream_list_guard\n");
    const double kWait = static_cast<double>(cascade::sink::kStreamListWaitMs.count());
    const double kStarve = static_cast<double>(cascade::sink::kStreamListStarveMs.count());

    // A: healthy holders, 60 ms each, back to back for 480 ms - twice
    // kStreamListWaitMs and a little. kRoundsA rounds. A waiter that wins the
    // lock in a gap between two holds has had a legitimate turn; what may
    // never happen is a waiter going ahead WITHOUT the lock while a holder is
    // inside. (The waiter wins the gap in most rounds on Linux, so one round
    // proves little: the count over all of them is the measure.)
    constexpr int kRoundsA = 40;
    int wentAheadInside = 0, notHeld = 0, wonGap = 0;
    const std::uint64_t beforeA = cascade::sink::paStreamListWaitsAbandoned();
    for (int round = 0; round < kRoundsA; ++round) {
        const Outcome o = race(8, 60);
        if (!o.held) { ++notHeld; }
        if (o.holderInside) { ++wentAheadInside; }
        if (o.held) { ++wonGap; }
    }
    std::printf("A: %d rounds behind healthy 60 ms holders: took its turn %d, went ahead WITHOUT the lock %d "
                "(with a holder inside %d), abandoned waits %llu\n",
                kRoundsA, wonGap, notHeld, wentAheadInside,
                static_cast<unsigned long long>(cascade::sink::paStreamListWaitsAbandoned() - beforeA));
    CHECK(notHeld == 0);
    CHECK(wentAheadInside == 0);

    // B: one holder that stops for 1 s.
    {
        const std::uint64_t before = cascade::sink::paStreamListWaitsAbandoned();
        const Outcome o = race(1, 1000);
        std::printf("B: behind one holder that stopped: held=%d, went ahead after %.0f ms (bound %.0f)\n",
                    o.held ? 1 : 0, o.waitedMs, kWait);
        CHECK(!o.held);
        CHECK(o.waitedMs >= kWait - 20.0 && o.waitedMs <= kWait + 200.0);
        CHECK(cascade::sink::paStreamListWaitsAbandoned() == before + 1);
    }

    // C: holders that never stop coming (200 ms each, for far longer than the
    // starvation bound).
    {
        const std::uint64_t before = cascade::sink::paStreamListWaitsAbandoned();
        const Outcome o = race(30, 200);   // 6 s of them
        std::printf("C: behind holders that never stop coming: held=%d, went ahead after %.0f ms (bound %.0f)\n",
                    o.held ? 1 : 0, o.waitedMs, kStarve);
        // Either it won a gap (a legitimate turn) or it gave up at the bound -
        // never later than the bound.
        CHECK(o.waitedMs <= kStarve + 400.0);
        CHECK(o.held || o.waitedMs >= kStarve - 20.0);
        CHECK(o.held || cascade::sink::paStreamListWaitsAbandoned() == before + 1);
    }

    return testSummary("test_pa_stream_list_guard");
}
