// rtl_reopen_hw - a BENCH TOOL, not shipped, not a test: the native RTL-SDR driver's re-open on a REAL dongle, RECEIVE ONLY.
//
// Opens the first RTL-SDR through the product's own source class (RtlSdrSource::open, so the re-open is armed exactly as in the product) at 98.0 MHz and 2.0 MS/s
// (the driver coerces the rate to the nearest it supports; the figure printed is the radio's), and prints the samples per second each second. After 5 s it calls the
// test seam that makes the transport's handle fail the way a re-enumerated device's does (RtlSdrSource::invalidateTransportForTest: every later call on the handle
// answers "invalid handle", nothing is freed under the reader), and prints whether, and after how many milliseconds, samples flow again, and whether the centre
// frequency, the sample rate and the tuner gain read back as before. Three times. Exit 0 only if all three recover.
//
// The window's forgiveness is shortened to 2 s through the driver's test seam so three cycles inside half a minute are not stopped by the two-per-window cap
// (the product's 60 s is untouched). Nothing is transmitted; no other radio is touched. The log and settings go where APPDATA and LOCALAPPDATA point: run it with
// both set to a scratch folder.
//
// Usage: rtl_reopen_hw      Exit 0 = three of three recovered; 1 = a cycle did not recover; 2 = no RTL-SDR bound to WinUSB; 3 = the radio would not open or start.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <complex>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "source/rtlsdr_source.hpp"

namespace {

using Clock = std::chrono::steady_clock;

long long msBetween(Clock::time_point a, Clock::time_point b) { return std::chrono::duration_cast<std::chrono::milliseconds>(b - a).count(); }

// Reads for `ms` and returns the samples that arrived.
std::size_t readFor(cascade::source::RtlSdrSource& src, int ms) {
    std::vector<std::complex<float>> buf(65536);
    std::size_t total = 0;
    const auto until = Clock::now() + std::chrono::milliseconds(ms);
    while (Clock::now() < until) { total += src.read(buf.data(), buf.size()); }
    return total;
}

}  // namespace

int main() {
    using cascade::source::RtlSdrSource;
    const auto found = cascade::source::enumerateRtlSdr();
    if (found.empty()) {
        std::printf("no RTL-SDR bound to WinUSB\n");
        return 2;
    }
    RtlSdrSource src;
    if (!src.open(found[0].args)) {
        std::printf("the radio would not open: %s\n", src.lastError());
        return 3;
    }
    src.setReopenBoundsForTest(5000, 250, 2000);
    if (!src.setSampleRateHz(2000000.0) || !src.setCenterFrequencyHz(98000000.0)) {
        std::printf("the radio would not take 2.0 MS/s at 98.0 MHz: %s\n", src.lastError());
        return 3;
    }
    if (!src.start()) {
        std::printf("the radio would not start: %s\n", src.lastError());
        return 3;
    }
    const double rate = src.sampleRateHz();
    const double freq = src.centerFrequencyHz();
    const double gain = src.gainDb("TUNER");
    std::printf("opened: %.4f MS/s, %.3f MHz, tuner gain %.1f dB, tuner %s\n", rate / 1e6, freq / 1e6, gain, src.tunerName().c_str());

    int recovered = 0;
    for (int cycle = 1; cycle <= 3; ++cycle) {
        // five seconds of normal streaming, one line each second
        for (int s = 1; s <= 5; ++s) {
            const std::size_t n = readFor(src, 1000);
            std::printf("cycle %d, second %d: %zu samples per second\n", cycle, s, n);
        }
        const int reopensBefore = src.reopenCount();
        const auto t0 = Clock::now();
        if (!src.invalidateTransportForTest()) {
            std::printf("cycle %d: the transport could not be invalidated\n", cycle);
            return 1;
        }
        std::printf("cycle %d: handle invalidated\n", cycle);
        // Flowing again = the radio was reopened AND a 200 ms window delivers most of what the rate promises. (The ring still holds up to 164 ms of old samples,
        // which is why a re-open is required, not just samples.)
        bool flowed = false;
        long long ms = -1;
        while (msBetween(t0, Clock::now()) < 15000) {
            const std::size_t n = readFor(src, 200);
            if (src.faulted()) { break; }
            if (src.reopenCount() > reopensBefore && static_cast<double>(n) >= rate * 0.2 * 0.8) {
                flowed = true;
                ms = msBetween(t0, Clock::now()) - 200;
                break;
            }
        }
        const bool same = src.centerFrequencyHz() == freq && src.sampleRateHz() == rate && src.gainDb("TUNER") == gain;
        if (flowed) {
            std::printf("cycle %d: samples flow again after %lld ms (re-opens %d); centre %s, rate %s, gain %s as before\n", cycle, ms, src.reopenCount(),
                        src.centerFrequencyHz() == freq ? "same" : "DIFFERENT", src.sampleRateHz() == rate ? "same" : "DIFFERENT", src.gainDb("TUNER") == gain ? "same" : "DIFFERENT");
            if (same) { ++recovered; }
        } else {
            std::printf("cycle %d: NO recovery (faulted %d): %s\n", cycle, src.faulted() ? 1 : 0, src.lastError());
            break;
        }
    }
    src.stop();
    src.closeDevice();
    std::printf("%d of 3 cycles recovered\n", recovered);
    return recovered == 3 ? 0 : 1;
}
