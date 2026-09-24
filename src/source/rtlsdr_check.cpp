// --rtlsdr-check: does the native RTL-SDR driver work on THIS machine, with
// THIS dongle, right now?
//
// WHY A CONSOLE CHECK AND NOT JUST A TEST. The suite proves the register
// sequences against a fake; it cannot prove that a user's dongle is bound to
// WinUSB, that it enumerates, that its tuner answers, or that samples arrive.
// Those are the four things that actually go wrong in the field, and the only
// honest answer to "is my radio working" is a run that opens it and reports
// what happened. --soapy-check exists for exactly this reason on the other
// path; this is its counterpart, and it prints the same kind of evidence:
// what was found, what it is, and how many samples came back.
//
// Exit 0 when a device was opened and delivered samples, 1 otherwise, so it
// is usable from a script as well as readable by a person.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "source/rtlsdr_source.hpp"

namespace {

constexpr double kCheckSeconds = 3.0;
constexpr double kCheckFreqHz = 100000000.0;
constexpr double kCheckRateHz = 2400000.0;

}  // namespace

int rtlsdrCheckMain(int argc, char** argv) {
    // One optional argument: the args string of the device to open
    // ("serial=00000001"). With none, the first device found.
    std::string args;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (a == nullptr) { continue; }
        if (std::strncmp(a, "--rtlsdr-check", 14) == 0) { continue; }
        if (a[0] != '-') { args = a; }
    }

    std::printf("FoxSDR native RTL-SDR check\n");
    const std::vector<cascade::source::NativeDeviceInfo> found =
        cascade::source::enumerateRtlSdr();
    std::printf("devices bound to WinUSB: %zu\n", found.size());
    for (std::size_t i = 0; i < found.size(); ++i) {
        std::printf("  [%zu] %s   (%s)\n", i, found[i].label.c_str(), found[i].args.c_str());
    }
    if (found.empty()) {
        // The commonest cause by a wide margin, so it is said rather than
        // left for the user to guess at.
        std::printf("FAIL no RTL-SDR is bound to WinUSB.\n"
                    "     A dongle running the DVB-T driver (or no driver at all) cannot be\n"
                    "     opened natively. Bind its interface to WinUSB with Zadig and run\n"
                    "     this again.\n");
        return 1;
    }
    if (args.empty()) { args = found[0].args; }

    cascade::source::RtlSdrSource src;
    if (!src.open(args)) {
        std::printf("FAIL could not open %s: %s\n", args.c_str(), src.lastError());
        return 1;
    }
    std::printf("opened: %s\n", src.name());
    std::printf("tuner: %s\n", src.tunerName().c_str());
    double lo = 0.0;
    double hi = 0.0;
    if (src.frequencyRangeHz(lo, hi)) {
        std::printf("tuning range: %.3f - %.3f MHz\n", lo / 1e6, hi / 1e6);
    }
    for (const cascade::source::GainInfo& g : src.gains()) {
        std::printf("gain %-6s %.1f .. %.1f dB (now %.1f)\n", g.name.c_str(), g.minDb, g.maxDb,
                    src.gainDb(g.name));
    }

    if (!src.setSampleRateHz(kCheckRateHz)) {
        std::printf("FAIL sample rate: %s\n", src.lastError());
        return 1;
    }
    if (!src.setCenterFrequencyHz(kCheckFreqHz)) {
        std::printf("FAIL tune: %s\n", src.lastError());
        return 1;
    }
    std::printf("rate: %.4f MS/s (asked %.4f)\n", src.sampleRateHz() / 1e6, kCheckRateHz / 1e6);
    std::printf("centre: %.4f MHz\n", src.centerFrequencyHz() / 1e6);

    if (!src.start()) {
        std::printf("FAIL start: %s\n", src.lastError());
        return 1;
    }
    std::vector<std::complex<float>> buf(65536);
    std::size_t total = 0;
    double sumMag = 0.0;
    double peak = 0.0;
    const auto until = std::chrono::steady_clock::now() +
                       std::chrono::milliseconds(static_cast<int>(kCheckSeconds * 1000.0));
    while (std::chrono::steady_clock::now() < until) {
        const std::size_t got = src.read(buf.data(), buf.size());
        for (std::size_t i = 0; i < got; ++i) {
            const double m = std::abs(buf[i]);
            sumMag += m;
            if (m > peak) { peak = m; }
        }
        total += got;
        if (src.faulted()) {
            std::printf("FAIL the radio faulted while streaming: %s\n", src.lastError());
            src.stop();
            return 1;
        }
    }
    src.stop();

    const double expected = src.sampleRateHz() * kCheckSeconds;
    const double mean = total > 0 ? sumMag / static_cast<double>(total) : 0.0;
    std::printf("streamed %zu samples in %.1f s (expected about %.0f, %.1f%%)\n", total,
                kCheckSeconds, expected,
                expected > 0.0 ? 100.0 * static_cast<double>(total) / expected : 0.0);
    std::printf("mean magnitude %.4f, peak %.4f\n", mean, peak);
    const std::string health = src.streamHealthLine();
    if (!health.empty()) { std::printf("%s\n", health.c_str()); }
    std::printf("bias tee: %s\n", src.biasT() ? "on" : "off");

    // The three ways this can be a failure while still having "worked":
    // nothing arrived, or what arrived is a flat line (a dead ADC reads a
    // constant), or it is pinned at full scale (a broken gain path).
    if (total == 0) {
        std::printf("FAIL no samples arrived\n");
        return 1;
    }
    if (mean <= 0.0005) {
        std::printf("FAIL the samples are silent - the ADC is not delivering signal\n");
        return 1;
    }
    if (mean >= 0.95) {
        std::printf("FAIL the samples are saturated - the gain path is stuck at full scale\n");
        return 1;
    }
    if (static_cast<double>(total) < expected * 0.5) {
        std::printf("FAIL less than half the expected samples arrived\n");
        return 1;
    }
    std::printf("PASS\n");
    return 0;
}
