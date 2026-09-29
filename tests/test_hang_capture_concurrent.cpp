// test_hang_capture_concurrent.cpp - two hang captures at once each get the
// stack they asked for (the Opus review of 47e991e, hang_watchdog.cpp).
//
// THE BUG. On Linux a thread's stack is taken by signalling the thread and
// letting a handler, running on that thread, unwind it and hand the frames
// back. The frames were handed back through ONE process-wide buffer and ONE
// semaphore, on the stated assumption that only one capture is ever in
// flight. Since stage 3b there are two watchdogs - the GUI's and the engine
// control thread's (engine-stage3.md section 10, OPEN 8) - and both can be
// capturing at once. Then watchdog A's wait can be answered by the thread B
// signalled (A's report shows B's target's stack under A's target's id), a
// handler can overwrite the buffer while the other side copies it, and a late
// answer from a timed-out thread can satisfy the next wait.
//
// THE TEST. Two target threads, parked at very different depths - ALPHA three
// calls deep, BETA forty (the capture keeps at most 48 frames) - so a stack
// can be told apart by its length alone, with no symbols needed. First each is
// captured alone, to show the shapes are what the test says (a test that
// could not tell them apart would pass by seeing nothing). Then two capture
// threads, started together, capture ALPHA and BETA respectively as fast as
// they can for a second and a half; every capture must have its own target's
// shape, and none may be lost to a timeout. Last, a LATE ANSWER: a third
// thread, GAMMA (forty deep, like BETA), has the capture signal blocked, so a
// capture of it times out; it is then unblocked while ALPHA is being captured
// in a loop, and answers the request that timed out long ago - an answer that
// must not be taken for any ALPHA capture's.
//
// Windows takes each stack with SuspendThread/GetThreadContext into a local
// CONTEXT, with nothing shared between captures, so this is a Linux test; on
// any other platform it says so and checks nothing.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>

#if defined(__linux__)
#include <csignal>
#include <pthread.h>
#endif

#include "core/hang_watchdog.hpp"
#include "test_check.hpp"

using cascade::core::HangWatchdog;

namespace {

std::atomic<bool> g_stop{false};
std::atomic<int> g_parked{0};
volatile std::uint64_t g_sink = 0;

#if defined(__GNUC__)
#define TEST_NOINLINE __attribute__((noinline))
#elif defined(_MSC_VER)
#define TEST_NOINLINE __declspec(noinline)
#else
#define TEST_NOINLINE
#endif

// Recurses `depth` calls, then spins until told to stop. The work after the
// recursive call keeps every frame on the stack (no tail call).
TEST_NOINLINE std::uint64_t parkAt(int depth, std::atomic<unsigned long>* tid) {
    if (depth > 0) {
        const std::uint64_t r = parkAt(depth - 1, tid);
        g_sink = g_sink + r + static_cast<std::uint64_t>(depth);
        return r + 1;
    }
    tid->store(HangWatchdog::currentThreadIdForTest());
    g_parked.fetch_add(1);
    std::uint64_t spins = 0;
    while (!g_stop.load(std::memory_order_relaxed)) {
        ++spins;
        g_sink = g_sink + spins;
    }
    return spins;
}

constexpr int kAlphaDepth = 3;
constexpr int kBetaDepth = 40;
// Frame counts that can only be the one or the other: ALPHA is its three
// calls plus the thread's start frames (well under 20); BETA is forty calls
// plus the same, capped at the capture's 48 (well over 35).
bool looksAlpha(int n) { return n > 0 && n < 20; }
bool looksBeta(int n) { return n > 35; }

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_hang_capture_concurrent\n");
#if !defined(__linux__)
    std::printf("not a Linux build: stacks are captured per call here (nothing shared), nothing to check\n");
    return testSummary("test_hang_capture_concurrent");
#else
    std::atomic<unsigned long> alphaTid{0};
    std::atomic<unsigned long> betaTid{0};
    std::thread alpha([&] { parkAt(kAlphaDepth, &alphaTid); });
    std::thread beta([&] { parkAt(kBetaDepth, &betaTid); });
    while (g_parked.load() < 2) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }

    // 1. Alone: the shapes the test tells them apart by.
    std::uintptr_t frames[64];
    const int a0 = HangWatchdog::captureThreadStackForTest(alphaTid.load(), frames, 64);
    const int b0 = HangWatchdog::captureThreadStackForTest(betaTid.load(), frames, 64);
    std::printf("alone: ALPHA %d frames, BETA %d frames\n", a0, b0);
    CHECK(looksAlpha(a0));
    CHECK(looksBeta(b0));

    // 2. Together.
    struct Tally {
        int captures = 0;
        int wrongShape = 0;
        int lost = 0;
    };
    Tally ta, tb;
    std::atomic<int> ready{0};
    auto hammer = [&](unsigned long tid, bool wantAlpha, Tally& t) {
        std::uintptr_t f[64];
        ready.fetch_add(1);
        while (ready.load() < 2) { std::this_thread::yield(); }
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
        while (std::chrono::steady_clock::now() < end) {
            const int n = HangWatchdog::captureThreadStackForTest(tid, f, 64);
            ++t.captures;
            if (n == 0) {
                ++t.lost;
            } else if (wantAlpha ? !looksAlpha(n) : !looksBeta(n)) {
                ++t.wrongShape;
            }
        }
    };
    std::thread ca([&] { hammer(alphaTid.load(), true, ta); });
    std::thread cb([&] { hammer(betaTid.load(), false, tb); });
    ca.join();
    cb.join();
    std::printf("together: ALPHA %d captures, %d with the wrong stack, %d lost; BETA %d captures, %d with the "
                "wrong stack, %d lost\n",
                ta.captures, ta.wrongShape, ta.lost, tb.captures, tb.wrongShape, tb.lost);
    CHECK(ta.captures > 50 && tb.captures > 50);   // they really did overlap, many times
    CHECK(ta.wrongShape == 0);
    CHECK(tb.wrongShape == 0);
    CHECK(ta.lost == 0);
    CHECK(tb.lost == 0);

    // 3. A late answer, eight times over (each round has a narrow window in
    // which a wrong answer can land, so one round alone is not a test).
    constexpr int kLateRounds = 8;
    Tally tl;
    int gammaAnswered = 0;
    long long gammaWorstMs = 0;
    for (int round = 0; round < kLateRounds; ++round) {
        std::atomic<unsigned long> gammaTid{0};
        std::atomic<bool> unblock{false};
        std::atomic<bool> done{false};
        std::thread gamma([&] {
            sigset_t set;
            sigemptyset(&set);
            sigaddset(&set, HangWatchdog::captureSignalForTest());
            pthread_sigmask(SIG_BLOCK, &set, nullptr);
            gammaTid.store(HangWatchdog::currentThreadIdForTest());
            // Park forty deep with the signal blocked; the park lets go of
            // the block from inside, when told - and the pending signal runs
            // there and then, answering a request that timed out long ago.
            struct Park {
                static std::uint64_t at(int depth, std::atomic<bool>* go, std::atomic<bool>* stop,
                                        const sigset_t* s) {
                    if (depth > 0) {
                        const std::uint64_t r = at(depth - 1, go, stop, s);
                        g_sink = g_sink + r;
                        return r + 1;
                    }
                    while (!go->load()) { g_sink = g_sink + 1; }
                    pthread_sigmask(SIG_UNBLOCK, s, nullptr);
                    std::uint64_t spins = 0;
                    while (!stop->load(std::memory_order_relaxed)) { g_sink = g_sink + ++spins; }
                    return spins;
                }
            };
            Park::at(kBetaDepth, &unblock, &done, &set);
        });
        while (gammaTid.load() == 0) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        const auto g0 = std::chrono::steady_clock::now();
        const int gn = HangWatchdog::captureThreadStackForTest(gammaTid.load(), frames, 64);
        const long long gMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now() - g0).count();
        if (gn != 0) { ++gammaAnswered; }
        if (gMs > gammaWorstMs) { gammaWorstMs = gMs; }
        std::atomic<bool> hammering{false};
        std::thread cl([&] {
            std::uintptr_t f[64];
            const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(150);
            while (std::chrono::steady_clock::now() < end) {
                hammering.store(true);
                const int n = HangWatchdog::captureThreadStackForTest(alphaTid.load(), f, 64);
                ++tl.captures;
                if (n == 0) {
                    ++tl.lost;
                } else if (!looksAlpha(n)) {
                    ++tl.wrongShape;
                }
            }
        });
        while (!hammering.load()) { std::this_thread::yield(); }
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        unblock.store(true);   // GAMMA answers its long-dead request, mid-hammer
        cl.join();
        done.store(true);
        gamma.join();
    }
    std::printf("late: GAMMA (signal blocked) answered %d of %d captures (worst %lld ms); ALPHA %d captures while "
                "GAMMA answered late, %d with the wrong stack, %d lost\n",
                gammaAnswered, kLateRounds, gammaWorstMs, tl.captures, tl.wrongShape, tl.lost);
    CHECK(gammaAnswered == 0);
    CHECK(gammaWorstMs < 1000);   // 200 ms, and at most one more 200 ms
    CHECK(tl.captures > 50);
    CHECK(tl.wrongShape == 0);
    CHECK(tl.lost == 0);
    g_stop.store(true);
    alpha.join();
    beta.join();
    return testSummary("test_hang_capture_concurrent");
#endif
}
