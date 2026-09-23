// Tests for core/patch_radio.hpp - several radios running at once, each on its
// own thread, each speaker writing its own output (0.99.17).
//
// THE SIGNALS ARE CHOSEN SO THE RADIOS CANNOT BE CONFUSED. Two signal
// generators, each carrying two carriers a fixed distance apart inside one
// channel: AM demodulation of two carriers gives their BEAT, so radio A's
// speaker hears 1 kHz and radio B's hears 2.5 kHz. "Which radio reached which
// speaker" is then answered by which tone is in which output, measured by
// correlation at the frequency each was made at - nothing is compared against a
// constant taken from the implementation.
//
// The rest is what a real patch depends on: each output receives sound at 48
// kHz in real time (the generator is paced by the wall clock, a device by
// itself), a spectrum is published with the carriers where they belong, and
// stop() returns promptly and stops everything.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/patch_radio.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

#include "core/patch_graph.hpp"
#include "core/patch_plan.hpp"
#include "core/patch_runner.hpp"
#include "source/siggen_source.hpp"
#include "test_check.hpp"

using namespace cascade::core::patch;

namespace {

constexpr double kTwoPi = 6.28318530717958647692;
constexpr double kRate = 2.0e6;

// A speaker's output that keeps what it is given. write() runs on the radio's
// reader thread; the test reads only after stop(), so no lock is needed.
class Capture final : public AudioDest {
public:
    void write(const float* s, std::size_t n) override {
        note(s, n);
        got.insert(got.end(), s, s + n);
    }
    std::string describe() const override { return "capture"; }
    std::string error() const override { return {}; }
    std::vector<float> got;
};

double toneAmplitude(const std::vector<float>& x, std::size_t from, std::size_t n, double hz) {
    double re = 0.0;
    double im = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double t = kTwoPi * hz * static_cast<double>(i) / kOutRateHz;
        re += static_cast<double>(x[from + i]) * std::cos(t);
        im += static_cast<double>(x[from + i]) * std::sin(t);
    }
    return 2.0 * std::sqrt(re * re + im * im) / static_cast<double>(n);
}

std::unique_ptr<cascade::source::SigGenSource> generator(double centreHz, double beatHz) {
    auto g = std::make_unique<cascade::source::SigGenSource>(kRate);
    g->setCenterFrequencyHz(centreHz);
    g->sigGen().setNoiseFloorDb(-300.0f);
    g->sigGen().setTone(0, 300000.0, 0.0f);            // the carrier
    g->sigGen().setTone(1, 300000.0 + beatHz, -6.0f);  // a sideband beatHz above it
    return g;
}

}  // namespace

int main() {
    // The patch: two radios, a channel on each at its carrier, AM, a speaker each.
    Graph g;
    const NodeId ra = g.addNode(NodeKind::Radio, "A", PortType::Iq);
    const NodeId rb = g.addNode(NodeKind::Radio, "B", PortType::Iq);
    g.mutableNode(ra)->device = "siggen";
    g.mutableNode(rb)->device = "siggen";
    const NodeId ca = g.addNode(NodeKind::Channel, "ca", PortType::Iq);
    const NodeId cb = g.addNode(NodeKind::Channel, "cb", PortType::Iq);
    const NodeId da = g.addNode(NodeKind::Demod, "AM", PortType::Iq);
    const NodeId db = g.addNode(NodeKind::Demod, "AM", PortType::Iq);
    const NodeId sa = g.addNode(NodeKind::Sink, "SA", PortType::Audio);
    const NodeId sb = g.addNode(NodeKind::Sink, "SB", PortType::Audio);
    g.mutableNode(da)->mode = 2;   // AM in the host's mode order
    g.mutableNode(db)->mode = 2;
    constexpr double kCentreA = 100.0e6;
    constexpr double kCentreB = 145.0e6;
    g.mutableNode(ca)->freqHz = kCentreA + 300000.0;
    g.mutableNode(cb)->freqHz = kCentreB + 300000.0;
    CHECK(g.connect(ra, 0, ca, 0) == Connect::Ok);
    CHECK(g.connect(rb, 0, cb, 0) == Connect::Ok);
    CHECK(g.connect(ca, 0, da, 0) == Connect::Ok);
    CHECK(g.connect(cb, 0, db, 0) == Connect::Ok);
    CHECK(g.connect(da, 0, sa, 0) == Connect::Ok);
    CHECK(g.connect(db, 0, sb, 0) == Connect::Ok);

    PatchRadio radioA(ra, generator(kCentreA, 1000.0), "Signal generator A");
    PatchRadio radioB(rb, generator(kCentreB, 2500.0), "Signal generator B");
    CHECK(radioA.rateHz() == kRate);
    CHECK(radioB.centreHz() == kCentreB);

    const std::vector<RadioInfo> radios{{ra, radioA.rateHz(), radioA.centreHz()},
                                        {rb, radioB.rateHz(), radioB.centreHz()}};
    const Plan plan = compile(g, radios);
    CHECK(plan.runnable);
    CHECK(plan.sinks.size() == 2u);

    auto capA = std::make_shared<Capture>();
    auto capB = std::make_shared<Capture>();
    const DestTable dests{{sa, capA}, {sb, capB}};
    auto setA = buildRadioSet(plan, g, ra, kRate, nullptr, nullptr, dests);
    auto setB = buildRadioSet(plan, g, rb, kRate, nullptr, nullptr, dests);
    // Each radio's set holds only its own channel and its own speaker.
    CHECK(setA->channels.size() == 1u && setA->channels[0].node == ca);
    CHECK(setB->channels.size() == 1u && setB->channels[0].node == cb);
    CHECK(setA->taps.size() == 1u && setA->taps[0].sink == sa);
    CHECK(setB->taps.size() == 1u && setB->taps[0].sink == sb);
    // A speaker with no output in the table gets no tap rather than a null one.
    CHECK(buildRadioSet(plan, g, ra, kRate, nullptr, nullptr, DestTable{})->taps.empty());
    // The signature moves when a speaker's output object changes.
    const std::string sig1 = radioSignature(plan, g, ra, kRate, nullptr, nullptr, dests);
    const DestTable other{{sa, std::make_shared<Capture>()}, {sb, capB}};
    CHECK(radioSignature(plan, g, ra, kRate, nullptr, nullptr, other) != sig1);
    CHECK(radioSignature(plan, g, rb, kRate, nullptr, nullptr, other) ==
          radioSignature(plan, g, rb, kRate, nullptr, nullptr, dests));   // B untouched

    radioA.runner().publish(setA);
    radioB.runner().publish(setB);

    std::string err;
    CHECK(radioA.start(err));
    CHECK(radioB.start(err));
    CHECK(radioA.running() && radioB.running());

    // Run for about 1.5 seconds of wall time.
    const auto t0 = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    // The spectrum: the carrier 300 kHz above centre is the strongest bin.
    std::vector<float> spec;
    std::uint64_t seq = 0;
    CHECK(radioA.spectrum(spec, seq));
    CHECK(seq > 0u);
    CHECK(spec.size() == 2048u);
    if (spec.size() == 2048u) {
        const auto peak = std::max_element(spec.begin(), spec.end()) - spec.begin();
        // fftshifted: bin 1024 is DC, each bin is kRate/2048 Hz.
        const double peakHz = (static_cast<double>(peak) - 1024.0) * kRate / 2048.0;
        std::printf("A spectrum peak at %+.0f Hz\n", peakHz);
        CHECK(std::fabs(peakHz - 300000.0) < 2.0 * kRate / 2048.0);
    }
    std::uint64_t same = seq;
    CHECK(!radioA.spectrum(spec, same) || same != seq);   // nothing newer is not reported as new

    const auto s0 = std::chrono::steady_clock::now();
    radioA.stop();
    radioB.stop();
    const double stopMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - s0).count();
    const double ranSec = std::chrono::duration<double>(s0 - t0).count();
    std::printf("ran %.2f s, stop took %.1f ms\n", ranSec, stopMs);
    CHECK(stopMs < 500.0);
    CHECK(!radioA.running() && !radioB.running());
    CHECK(radioA.fault().empty());

    // Real time: each output got about 48000 samples a second of running.
    const double expect = kOutRateHz * ranSec;
    std::printf("A got %zu, B got %zu, expected about %.0f\n", capA->got.size(), capB->got.size(),
                expect);
    CHECK(static_cast<double>(capA->got.size()) > 0.8 * expect);
    CHECK(static_cast<double>(capA->got.size()) < 1.2 * expect);
    CHECK(static_cast<double>(capB->got.size()) > 0.8 * expect);
    CHECK(static_cast<double>(capB->got.size()) < 1.2 * expect);

    // Each speaker hears ITS radio: A's beat at 1 kHz, B's at 2.5 kHz.
    constexpr std::size_t kWin = 9600;   // 0.2 s
    if (capA->got.size() > 2 * kWin && capB->got.size() > 2 * kWin) {
        const std::size_t fa = capA->got.size() - kWin - 1000;
        const std::size_t fb = capB->got.size() - kWin - 1000;
        const double a1 = toneAmplitude(capA->got, fa, kWin, 1000.0);
        const double a25 = toneAmplitude(capA->got, fa, kWin, 2500.0);
        const double b1 = toneAmplitude(capB->got, fb, kWin, 1000.0);
        const double b25 = toneAmplitude(capB->got, fb, kWin, 2500.0);
        std::printf("A: 1k=%.4f 2.5k=%.4f   B: 1k=%.4f 2.5k=%.4f\n", a1, a25, b1, b25);
        CHECK(a1 > 0.02);
        CHECK(b25 > 0.02);
        CHECK(a1 > 10.0 * a25);
        CHECK(b25 > 10.0 * b1);
    }

    // After stop nothing more arrives.
    const std::size_t after = capA->got.size();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(capA->got.size() == after);

    // stop() twice, and a destructor after stop, are harmless.
    radioA.stop();

    // A radio whose source will not start is refused with a reason.
    {
        class Dead final : public cascade::source::IqSource {
        public:
            bool start() override { return false; }
            void stop() override {}
            bool running() const override { return false; }
            bool selfPaced() const override { return true; }
            double sampleRateHz() const override { return kRate; }
            bool setSampleRateHz(double) override { return false; }
            double centerFrequencyHz() const override { return 0.0; }
            bool setCenterFrequencyHz(double) override { return false; }
            std::size_t read(std::complex<float>*, std::size_t) override { return 0; }
            const char* name() const override { return "dead"; }
            const char* lastError() const override { return "unplugged"; }
        };
        PatchRadio dead(ra, std::make_unique<Dead>(), "dead");
        std::string why;
        CHECK(!dead.start(why));
        CHECK(why == "unplugged");
        CHECK(!dead.running());
    }

    return testSummary("test_patch_radio");
}
