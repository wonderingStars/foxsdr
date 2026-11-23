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
// A FOURTH PHASE follows the three cycles: the handle is invalidated again, and when the re-open is in flight a stop is requested. The tool prints how long the stop took,
// whether the radio is then faulted and dead with the sentence that says it was being reconnected, whether start() is refused with it, and whether a FRESH open of the same
// radio afterwards streams (what picking its row again does).
//
// Usage: rtl_reopen_hw      Exit 0 = three of three recovered and the fourth phase shown; 1 = a cycle did not recover or the fourth phase was not shown; 2 = no RTL-SDR bound to
//                           WinUSB; 3 = the radio would not open or start.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <complex>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include <cstdlib>

#include "core/diag_log.hpp"
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
    // The driver's own log lines (the stream account, the re-open's steps with the process clock) go to a folder named by RTL_REOPEN_LOGDIR, when set.
    if (const char* logDir = std::getenv("RTL_REOPEN_LOGDIR")) { cascade::core::DiagLog::instance().configure(logDir, true); }
    const auto found = cascade::source::enumerateRtlSdr();
    if (found.empty()) {
        std::printf("no RTL-SDR bound to WinUSB\n");
        return 2;
    }
    // THE LOCATION the driver computes for each listed radio (decision S11: it must name the PORT, not only the hub). Read-only; it contains no serial, and is printed as is.
    for (const cascade::usb::UsbDeviceInfo& d : cascade::usb::enumerateWinUsb(cascade::source::rtlSdrUsbIds())) { std::printf("location of a listed radio: \"%s\"\n", d.location.c_str()); }
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
    std::printf("%d of 3 cycles recovered\n", recovered);
    if (recovered != 3) {
        src.stop();
        src.closeDevice();
        return 1;
    }

    // PHASE 4 (decision F1 of the review): A STOP THAT LANDS WHILE THE RADIO IS BEING REOPENED. The handle is invalidated, the tool waits for the re-open to be in
    // flight (the radio is closed and being brought back), requests the stop, and then asks the source what it is: faulted and dead with the sentence that says it was
    // being reconnected, closed (no stream), and a following start() refused with those words. Then the radio is opened afresh (what picking its row again does) and
    // must stream. The re-open of a listed radio takes under a second, so the stop may arrive after it has finished; that attempt is reported as "too late" and tried
    // again (up to five times) - only a stop that really landed inside a re-open counts.
    bool stopPhaseOk = false;
    for (int attempt = 1; attempt <= 5 && !stopPhaseOk; ++attempt) {
        if (!src.running() && !src.faulted() && !src.start()) {
            std::printf("phase 4, attempt %d: the radio would not start again: %s\n", attempt, src.lastError());
            break;
        }
        (void)readFor(src, 500);
        if (!src.invalidateTransportForTest()) {
            std::printf("phase 4, attempt %d: the transport could not be invalidated\n", attempt);
            break;
        }
        const auto t0 = Clock::now();
        bool inFlight = false;
        while (msBetween(t0, Clock::now()) < 3000) {
            if (src.reopeningForTest()) {
                inFlight = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
        const long long toReopen = msBetween(t0, Clock::now());
        const auto tStop = Clock::now();
        src.stop();
        const long long stopMs = msBetween(tStop, Clock::now());
        const bool faulted = src.faulted();
        const bool dead = src.deviceDead();
        const std::string said = src.lastError();
        const bool right = said == cascade::source::kReopenStoppedSentence;
        const bool refused = !src.start();
        const std::string saidAfter = src.lastError();
        std::printf("phase 4, attempt %d: re-open in flight after %lld ms: %s; stop took %lld ms; faulted %d dead %d; words: \"%s\"; start() refused %d, words after: \"%s\"\n", attempt, toReopen,
                    inFlight ? "yes" : "no", stopMs, faulted ? 1 : 0, dead ? 1 : 0, said.c_str(), refused ? 1 : 0, saidAfter.c_str());
        if (!inFlight || !faulted) {
            std::printf("phase 4, attempt %d: too late (the re-open had finished before the stop landed); trying again\n", attempt);
            if (!faulted) { continue; }
        }
        if (inFlight && faulted && dead && right && refused && saidAfter == cascade::source::kReopenStoppedSentence) {
            // the radio is closed: a FRESH open of the same radio (what picking the row again does) streams
            src.closeDevice();
            RtlSdrSource fresh;
            if (!fresh.open(found[0].args)) {
                std::printf("phase 4: the fresh open failed: %s\n", fresh.lastError());
                return 1;
            }
            fresh.setSampleRateHz(2000000.0);
            fresh.setCenterFrequencyHz(98000000.0);
            if (!fresh.start()) {
                std::printf("phase 4: the fresh open would not start: %s\n", fresh.lastError());
                return 1;
            }
            const std::size_t n = readFor(fresh, 1000);
            std::printf("phase 4: a fresh open after the stop: %zu samples in one second at %.3f MHz, %.4f MS/s, faulted %d\n", n, fresh.centerFrequencyHz() / 1e6, fresh.sampleRateHz() / 1e6, fresh.faulted() ? 1 : 0);
            fresh.stop();
            fresh.closeDevice();
            stopPhaseOk = n > 0 && !fresh.faulted();
            if (!stopPhaseOk) { return 1; }
            std::printf("phase 4: ok\n");
            return 0;
        }
        std::printf("phase 4, attempt %d: the state after the stop was not the expected one\n", attempt);
        break;
    }
    src.stop();
    src.closeDevice();
    std::printf("phase 4: NOT shown (%s)\n", stopPhaseOk ? "ok" : "no stop landed inside a re-open, or the state was wrong");
    return 1;
}
