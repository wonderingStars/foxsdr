// SNR measurement tap (src/core/snr_tap.hpp): accuracy against a known SNR, the history ring, concurrent reads, allocation.
// Channel-power readings are generated: a noise floor at -60 dBFS and signal + noise readings with a known SNR, with the scatter
// of a one-pole power meter (about 0.5 dB standard deviation). No recording.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <cstdio>
#include <random>
#include <thread>

#include "alloc_probe.hpp"
#include "core/snr_tap.hpp"
#include "test_check.hpp"

using cascade::core::meas::SnrSample;
using cascade::core::meas::SnrTap;

namespace {
double totalFor(double floorDb, double snrDb, double jitterDb) {
    return 10.0 * std::log10(std::pow(10.0, floorDb / 10.0) * (1.0 + std::pow(10.0, snrDb / 10.0))) + jitterDb;
}

void testAccuracy() {
    std::mt19937 rng(5);
    std::normal_distribution<double> jit(0.0, 0.5);
    for (double snr : {0.0, 3.0, 10.0, 20.0, 30.0, 40.0}) {
        SnrTap tap(512);
        double t = 0;
        // 15 s of noise only (the floor), then 10 s of signal; 20 readings per second; the gate opens with the signal.
        for (int i = 0; i < 300; ++i, t += 0.05) { tap.push(-60.0 + jit(rng), t, false); }
        for (int i = 0; i < 200; ++i, t += 0.05) { tap.push(totalFor(-60.0, snr, jit(rng)), t, true); }
        const double mean = tap.meanSnrDb(5.0);
        SnrSample s;
        CHECK(tap.latest(s));
        std::printf("MEAS true SNR %.0f dB: mean over the last 5 s %.2f dB (error %+.2f), floor held %.2f dB\n", snr, mean, mean - snr, s.floorDb);
        CHECK_NEAR(mean, snr, 1.5);
        CHECK_NEAR(s.floorDb, -60.0, 1.5);
    }
    // No signal at all: SNR reads NaN or about 0, never a big positive number.
    SnrTap quiet(64);
    double t = 0;
    for (int i = 0; i < 400; ++i, t += 0.05) { quiet.push(-60.0 + jit(rng), t, false); }
    const double m = quiet.meanSnrDb(5.0);
    std::printf("MEAS noise only for 20 s: mean SNR %.2f dB (NaN means no sample was above the floor)\n", m);
    CHECK(std::isnan(m) || m < 3.0);
    // Before the floor exists (under half a second): NaN SNR.
    SnrTap fresh(64);
    fresh.push(-40.0, 0.0);
    SnrSample s0;
    CHECK(fresh.latest(s0) && std::isnan(s0.snrDb) && std::isnan(s0.floorDb));
}

void testRing() {
    SnrTap tap(8);
    CHECK(tap.capacity() == 8);
    SnrSample out[16];
    CHECK(tap.history(out, 16) == 0);
    SnrSample one;
    CHECK(!tap.latest(one));
    for (int i = 0; i < 5; ++i) { tap.push(-50.0 + i, i * 0.1); }
    CHECK(tap.history(out, 16) == 5);
    CHECK(out[0].totalDb == -50.0 && out[4].totalDb == -46.0);  // oldest first
    for (int i = 5; i < 30; ++i) { tap.push(-50.0 + i, i * 0.1); }
    const std::size_t n = tap.history(out, 16);
    CHECK(n == 7);  // capacity - 1 (one slot is the one being written)
    bool ordered = true;
    for (std::size_t i = 1; i < n; ++i) { ordered = ordered && out[i].totalDb == out[i - 1].totalDb + 1.0; }
    CHECK(ordered && out[n - 1].totalDb == -50.0 + 29);
    CHECK(tap.history(out, 3) == 3 && out[2].totalDb == -50.0 + 29);  // the newest three
    CHECK(tap.history(out, 0) == 0);
    CHECK(tap.count() == 30);
    SnrTap big(1000);
    CHECK(big.capacity() == 1024);  // rounded up to a power of two
}

void testConcurrentReads() {
    SnrTap tap(64);
    std::atomic<bool> stop{false};
    std::atomic<long> reads{0}, bad{0};
    std::thread reader([&] {
        SnrSample out[63];
        while (!stop.load()) {
            const std::size_t n = tap.history(out, 63);
            for (std::size_t i = 0; i < n; ++i) {
                // Every pushed sample has timeS = k and totalDb = -k: one sample's fields must agree, and ages must be consecutive.
                if (out[i].totalDb != -out[i].timeS) { ++bad; }
                if (i > 0 && out[i].timeS != out[i - 1].timeS + 1.0) { ++bad; }
            }
            ++reads;
        }
    });
    for (long k = 0; k < 2000000; ++k) { tap.push(-static_cast<double>(k), static_cast<double>(k)); }
    stop = true;
    reader.join();
    std::printf("MEAS concurrent reader: %ld history copies while 2,000,000 samples were pushed, %ld inconsistent samples\n", reads.load(), bad.load());
    CHECK(bad.load() == 0);
    CHECK(reads.load() > 100);
}

void testAllocationAndBasics() {
    SnrTap tap(256);
    std::mt19937 rng(1);
    std::normal_distribution<double> jit(0.0, 0.5);
    double t = 0;
    for (int i = 0; i < 100; ++i, t += 0.05) { tap.push(-60.0 + jit(rng), t); }
    long news = 0;
    {
        AllocProbe p;
        for (int i = 0; i < 2000; ++i, t += 0.05) { tap.push(-60.0 + jit(rng), t); }  // 100 s of pushes
        news = p.news();
    }
    std::printf("MEAS operator new in 2000 pushes (100 s at 20 per second): %ld (the floor tracker's deque, about one per second here)\n", news);
    CHECK(news <= 300);
    long newsRing = 0;
    SnrSample out[64];
    {
        AllocProbe p;
        tap.latest(out[0]);
        tap.history(out, 64);
        newsRing = p.news();
    }
    CHECK(newsRing == 0);
    // NaN and infinite readings are carried, not fatal.
    tap.push(std::nan(""), t);
    tap.push(INFINITY, t + 0.05);
    tap.push(-200.0, t + 0.1);
    SnrSample s;
    CHECK(tap.latest(s));
    // A clock that goes backwards starts a new floor (the tracker's rule) and does not crash.
    tap.push(-60.0, 0.0);
    tap.reset();
    CHECK(true);
}
}  // namespace

int main() {
    testAccuracy();
    testRing();
    testConcurrentReads();
    testAllocationAndBasics();
    return testSummary("test_snr_tap");
}
