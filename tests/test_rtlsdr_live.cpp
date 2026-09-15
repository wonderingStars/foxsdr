// THE LIVE RADIO. Everything else in this driver's suite is proved against a
// fake, and a fake can only ever prove that the driver does what its author
// thought the silicon wanted.
//
// What this file adds is the question a fake cannot be asked: with a real
// dongle on the bus, do samples arrive at the rate the radio says it is
// running at, do they look like a signal rather than a flat line or a
// saturated one, does a rate change on a LIVE stream deliver the new rate,
// and does stop() come back promptly while samples are still flowing? Those
// four are where every native driver this product has replaced actually went
// wrong.
//
// A MACHINE WITH NO DONGLE IS A SUPPORTED CONFIGURATION. This test says so in
// one line and passes - but it says it out loud, because a test that quietly
// does nothing and reports success is worse than no test. The same applies
// when the dongle is present but already in use: that is contention on this
// machine, not a defect, and it is named as such.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "core/diag_log.hpp"
#include "source/rtlsdr_source.hpp"
#include "test_check.hpp"

using cascade::source::NativeDeviceInfo;
using cascade::source::RtlSdrSource;

// THE CONSOLE CHECK, which main.cpp will expose as --rtlsdr-check. Declared
// here rather than in a header because it has exactly one call site in the
// application and one here; exercising it from this file is what stops it
// being wired in for the first time on a user's machine.
int rtlsdrCheckMain(int argc, char** argv);

namespace {

constexpr double kStreamSeconds = 5.0;
// The counted total must be within this of rate x seconds. Two per cent is
// generous for a USB device measured with a wall clock over five seconds and
// tight enough that a stream running at the wrong rate cannot pass.
constexpr double kRateTolerance = 0.02;

struct StreamResult {
    std::size_t samples = 0;
    double seconds = 0.0;
    double meanMag = 0.0;
    double peakMag = 0.0;
    bool faulted = false;
};

// Reads for `seconds` and reports what came back. Deliberately does NOT sleep
// between reads: read() blocks (bounded) on an empty ring, so a busy loop
// here would be measuring the test rather than the radio.
StreamResult streamFor(RtlSdrSource& src, double seconds) {
    StreamResult r;
    std::vector<std::complex<float>> buf(65536);
    double sum = 0.0;
    const auto t0 = std::chrono::steady_clock::now();
    const auto until = t0 + std::chrono::milliseconds(static_cast<int>(seconds * 1000.0));
    while (std::chrono::steady_clock::now() < until) {
        const std::size_t got = src.read(buf.data(), buf.size());
        for (std::size_t i = 0; i < got; ++i) {
            const double m = std::abs(buf[i]);
            sum += m;
            if (m > r.peakMag) { r.peakMag = m; }
        }
        r.samples += got;
        if (src.faulted()) {
            r.faulted = true;
            break;
        }
    }
    r.seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    r.meanMag = r.samples > 0 ? sum / static_cast<double>(r.samples) : 0.0;
    return r;
}

// One open-to-close cycle. Returns true when the dongle was actually opened;
// false means it is in use by something else on this machine, which is
// contention rather than a defect - and the caller must not then interpret
// anything that follows as evidence about this driver.
bool oneCycle(const std::string& args, int cycle) {
    RtlSdrSource src;
    if (!src.open(args)) {
        std::printf("  cycle %d: the dongle would not open (%s).\n"
                    "  The transport opens a radio EXCLUSIVELY, so this is what a second\n"
                    "  program holding it looks like - contention on this bench, not a\n"
                    "  defect in this driver. Re-run with nothing else using the dongle.\n",
                    cycle, src.lastError());
        return false;
    }
    std::printf("  cycle %d: opened %s, tuner %s\n", cycle, src.name(),
                src.tunerName().c_str());

    // THE USB STRINGS CAME BACK, which no fake can prove and which one dongle
    // on a bench can. Every RTL-SDR carries a manufacturer string, and this
    // driver's whole recognition of an RTL-SDR Blog V4 - and therefore the
    // crystal its tuner's PLL is referenced to - rests on reading it. From
    // 0.93.0 to 0.96.3 it read NOTHING on every dongle on every machine: the
    // GET_DESCRIPTOR was asking for 256 bytes and a descriptor's length is one
    // byte, so the device answered with zero bytes and no error at all. Six
    // V4-specific features were dead in the field while the fake said they
    // worked, because the fake answered a request no device answers. An empty
    // manufacturer string here is that defect, back.
    {
        bool sawStrings = false;
        for (const std::string& line : cascade::core::DiagLog::instance().ringSnapshot()) {
            if (line.find("usb strings manufacturer \"\"") != std::string::npos) {
                std::printf("  cycle %d: the dongle's USB strings read back EMPTY: %s\n", cycle,
                            line.c_str());
            } else if (line.find("usb strings manufacturer \"") != std::string::npos) {
                sawStrings = true;
                std::printf("  cycle %d: %s\n", cycle, line.c_str());
            }
        }
        CHECK(sawStrings);
    }

    CHECK(src.setSampleRateHz(2400000.0));
    CHECK(src.setCenterFrequencyHz(100000000.0));
    // A manual gain the whole way, so the numbers below are about the radio
    // and not about an AGC hunting.
    CHECK(src.setGainDb("TUNER", 29.7));
    const double rate1 = src.sampleRateHz();
    std::printf("  cycle %d: %.4f MS/s, %.4f MHz, TUNER %.1f dB\n", cycle, rate1 / 1e6,
                src.centerFrequencyHz() / 1e6, src.gainDb("TUNER"));
    CHECK(src.start());
    CHECK(src.running());

    const StreamResult a = streamFor(src, kStreamSeconds);
    const double expectA = rate1 * a.seconds;
    std::printf("  cycle %d: %10zu samples in %.2f s (expected %.0f, %+.2f%%), "
                "mean %.4f peak %.4f\n",
                cycle, a.samples, a.seconds, expectA,
                100.0 * (static_cast<double>(a.samples) - expectA) / expectA, a.meanMag,
                a.peakMag);
    CHECK(!a.faulted);
    CHECK(std::fabs(static_cast<double>(a.samples) - expectA) <= expectA * kRateTolerance);
    // Not a flat line (a dead ADC reads a constant) and not pinned at full
    // scale (a broken gain path). Both look like "it worked" to a counter.
    CHECK(a.meanMag > 0.001);
    CHECK(a.meanMag < 0.95);
    CHECK(a.peakMag > 0.0);

    // THE RATE CHANGE ON A LIVE STREAM, which is where the Soapy path used to
    // kill the process. 1.0 MS/s is not one of the standard rates, so it is
    // coerced to the nearest - 1.024 MS/s - and the count is judged against
    // what the radio SAYS it is running at, not against what was asked for.
    CHECK(src.setSampleRateHz(1000000.0));
    const double rate2 = src.sampleRateHz();
    std::printf("  cycle %d: asked 1.0000 MS/s, running %.4f MS/s\n", cycle, rate2 / 1e6);
    CHECK(rate2 > 0.0);
    CHECK(src.running());
    const StreamResult b = streamFor(src, kStreamSeconds);
    const double expectB = rate2 * b.seconds;
    std::printf("  cycle %d: %10zu samples in %.2f s (expected %.0f, %+.2f%%), "
                "mean %.4f peak %.4f\n",
                cycle, b.samples, b.seconds, expectB,
                100.0 * (static_cast<double>(b.samples) - expectB) / expectB, b.meanMag,
                b.peakMag);
    CHECK(!b.faulted);
    CHECK(std::fabs(static_cast<double>(b.samples) - expectB) <= expectB * kRateTolerance);
    CHECK(b.meanMag > 0.001);

    // A retune and the gain controls, live.
    CHECK(src.setCenterFrequencyHz(1090000000.0));
    CHECK_NEAR(src.centerFrequencyHz(), 1090000000.0, 1.0);
    CHECK(src.setGainDb("LNA", 16.6));
    CHECK_NEAR(src.gainDb("LNA"), 16.6, 0.05);
    CHECK(src.setGainDb("MIXER", 8.8));
    CHECK(src.setGainDb("VGA", 16.3));
    CHECK(src.setAutoGain(true));
    CHECK(src.autoGain());
    CHECK(src.setAutoGain(false));
    CHECK(!src.autoGain());
    CHECK(!src.faulted());

    // STOP WHILE SAMPLES ARE STILL FLOWING, and time it. This is the one the
    // shutdown budget depends on: a reader thread that does not leave
    // promptly is a teardown that outlives its own watchdog.
    const auto t0 = std::chrono::steady_clock::now();
    src.stop();
    const double stopMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("  cycle %d: stop() while streaming returned in %.1f ms\n", cycle, stopMs);
    CHECK(!src.running());
    CHECK(stopMs < 500.0);
    CHECK(!src.deviceDead());

    const std::string health = src.streamHealthLine();
    if (!health.empty()) { std::printf("  cycle %d: %s\n", cycle, health.c_str()); }

    src.closeDevice();
    CHECK(!src.isOpen());
    return true;
}

}  // namespace

int main() {
    const std::vector<NativeDeviceInfo> found = cascade::source::enumerateRtlSdr();
    std::printf("live check: %zu RTL-SDR(s) bound to WinUSB\n", found.size());
    for (const NativeDeviceInfo& d : found) {
        std::printf("  %s  (%s)\n", d.label.c_str(), d.args.c_str());
    }
    if (found.empty()) {
        std::printf("SKIPPED: no RTL-SDR is bound to WinUSB on this machine, so nothing here\n"
                    "can be measured. The register sequences are still proven by\n"
                    "test_rtlsdr_source against the fake transport; what is NOT proven on\n"
                    "this machine is that samples arrive from real silicon at the rate the\n"
                    "driver claims. Bind a dongle to WinUSB with Zadig and run this again.\n");
        return testSummary("test_rtlsdr_live");
    }

    // TWICE IN ONE PROCESS. Opening a radio a second time after a clean close
    // is the case a driver that leaks a handle, a thread or a pipe fails on,
    // and it is what a user does every time they switch source and switch
    // back.
    bool opened = false;
    for (int cycle = 1; cycle <= 2; ++cycle) {
        if (!oneCycle(found[0].args, cycle)) { break; }
        opened = true;
    }

    // ...and the console check itself, run rather than assumed. It is what a
    // user with a silent radio will be asked to run, so it has to work on the
    // day it is wired in. Skipped - and said - when the dongle was in use
    // above, because an exit code from a run that could not open a radio says
    // nothing about the check.
    if (!opened) {
        std::printf("rtlsdrCheckMain NOT RUN: the dongle was in use by something else on this\n"
                    "machine, so nothing above measured this driver either.\n");
        return testSummary("test_rtlsdr_live");
    }
    std::printf("---- rtlsdrCheckMain ----\n");
    char arg0[] = "--rtlsdr-check";
    char* argv[] = {arg0, nullptr};
    const int rc = rtlsdrCheckMain(1, argv);
    std::printf("---- rtlsdrCheckMain exit %d ----\n", rc);
    CHECK(rc == 0);

    return testSummary("test_rtlsdr_live");
}
