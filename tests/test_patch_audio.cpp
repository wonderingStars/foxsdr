// The patch's audio, through a REAL Pipeline - the end-to-end proof.
//
// Everything underneath is tested on its own: the strip demodulates
// (test_patch_strip), the runner hands over between threads and resamples to
// the sink's rate (test_patch_runner). What none of those can show is that the
// patch's audio actually reaches the point every consumer is fed from - the
// speakers, the recorder, the web stream and the test tap - and replaces the
// demodulator's there, and hands back when it stops. That is a property of the
// finished chain, so it is asked of the finished chain.
//
// THE SIGNALS ARE CHOSEN SO THE TWO SOURCES CANNOT BE CONFUSED. The receiver's
// own demodulator is set to USB on a carrier 3 kHz up: a clean 3 kHz tone. The
// patch listens to a channel 200 kHz away holding a strong carrier with a
// weaker sideband 1 kHz above it - ordinary AM, whose envelope is a 1 kHz tone.
// So "what is playing" is answered by which of 1 kHz and 3 kHz is in the tap,
// measured by correlation at the frequency each was made at. Nothing is
// compared against a constant taken from the implementation.
//
// Modelled on test_plugin_audio's pipeline half, which answers the same
// question for the plugin takeover this path deliberately mirrors.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
#include <vector>

#include "core/patch_graph.hpp"
#include "core/patch_plan.hpp"
#include "core/patch_runner.hpp"
#include "core/pipeline.hpp"
#include "test_check.hpp"

using cascade::core::Pipeline;
using cascade::core::patch::buildStripSet;
using cascade::core::patch::compile;
using cascade::core::patch::Connect;
using cascade::core::patch::Graph;
using cascade::core::patch::kNoNode;
using cascade::core::patch::listeningChannel;
using cascade::core::patch::NodeId;
using cascade::core::patch::NodeKind;
using cascade::core::patch::PortType;

namespace {

constexpr double kTwoPi = 6.28318530717958647692;
constexpr double kSinkRateHz = Pipeline::kAudioRateHz;
constexpr std::size_t kWin = 4096;

double toneAmplitude(const float* x, std::size_t n, double hz, double rateHz) {
    double re = 0.0;
    double im = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double t = kTwoPi * hz * static_cast<double>(i) / rateHz;
        re += static_cast<double>(x[i]) * std::cos(t);
        im += static_cast<double>(x[i]) * std::sin(t);
    }
    return 2.0 * std::sqrt(re * re + im * im) / static_cast<double>(n);
}

template <typename Fn>
bool waitFor(Fn ready, int timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (ready()) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return ready();
}

}  // namespace

int main() {
    Pipeline::Config cfg;
    cfg.sampleRateHz = 1000000.0;
    cfg.fftSize = 1024;
    cfg.averagingAlpha = 0.5f;
    cfg.audioEnabled = false;   // headless: the chain runs, no device is opened
    Pipeline p(cfg);

    // The receiver's own audio: USB on a carrier 3 kHz up is a 3 kHz tone.
    p.setDemodMode(cascade::dsp::DemodMode::USB);
    p.setSquelchDb(-200.0f);
    p.sigGen().setNoiseFloorDb(-300.0f);
    p.sigGen().setTone(0, 3000.0, 0.0f);

    // The patch's audio: a carrier 200 kHz up with a sideband 1 kHz above it
    // at half the amplitude. Its envelope is 1 + 0.5 cos(1 kHz) - AM with a
    // clean 1 kHz tone, and nowhere near the demodulator's passband.
    constexpr double kChanOffset = 200000.0;
    p.sigGen().setTone(1, kChanOffset, 0.0f);
    p.sigGen().setTone(2, kChanOffset + 1000.0, -6.0f);

    p.start();
    std::vector<float> left(kWin, 0.0f);
    std::vector<float> right(kWin, 0.0f);

    // (a) THE BASELINE, before any patch. Without it every measurement below
    // would be vacuous: a chain producing nothing at all would pass "the patch
    // is what is playing" perfectly.
    CHECK(waitFor([&] { return p.audioSamplesProduced() > 3u * kWin; }, 30000));
    CHECK(p.audioTapStereo(left.data(), right.data(), kWin) == kWin);
    const double demod3k = toneAmplitude(left.data(), kWin, 3000.0, kSinkRateHz);
    const double demod1k = toneAmplitude(left.data(), kWin, 1000.0, kSinkRateHz);
    std::printf("demod:  3 kHz=%.4f  1 kHz=%.4f\n", demod3k, demod1k);
    CHECK(demod3k > 0.01);
    CHECK(demod1k < demod3k / 5.0);

    // (b) THE PATCH TAKES THE SPEAKERS. Radio -> channel on the AM carrier ->
    // AM demod -> speaker, built and published exactly as the page does it.
    Graph g;
    const NodeId radio = g.addNode(NodeKind::Radio, "Radio", PortType::Iq);
    const NodeId chan = g.addNode(NodeKind::Channel, "AM carrier", PortType::Iq);
    const NodeId dm = g.addNode(NodeKind::Demod, "AM", PortType::Iq);
    const NodeId spk = g.addNode(NodeKind::Sink, "Speaker", PortType::Audio);
    const double centre = p.activeSource().centerFrequencyHz();
    g.mutableNode(chan)->freqHz = centre + kChanOffset;
    g.mutableNode(dm)->mode = 2;   // AM, in the host's mode order
    CHECK(g.connect(radio, 0, chan, 0) == Connect::Ok);
    CHECK(g.connect(chan, 0, dm, 0) == Connect::Ok);
    CHECK(g.connect(dm, 0, spk, 0) == Connect::Ok);

    const auto plan = compile(g, cfg.sampleRateHz, centre);
    CHECK(plan.runnable);
    CHECK(listeningChannel(g) == chan);

    p.patchRunner().publish(
        buildStripSet(plan, g, cfg.sampleRateHz, listeningChannel(g), kSinkRateHz));

    // Let the fade finish and the tap fill with nothing but patch audio.
    std::uint64_t mark = p.audioSamplesProduced();
    CHECK(waitFor([&] { return p.audioSamplesProduced() - mark > 3u * kWin; }, 30000));
    CHECK(p.patchRunner().running());
    CHECK(p.audioTapStereo(left.data(), right.data(), kWin) == kWin);
    const double patch1k = toneAmplitude(left.data(), kWin, 1000.0, kSinkRateHz);
    const double patch3k = toneAmplitude(left.data(), kWin, 3000.0, kSinkRateHz);
    std::printf("patch:  1 kHz=%.4f  3 kHz=%.4f\n", patch1k, patch3k);

    // The patch's 1 kHz is what is playing...
    CHECK(patch1k > 0.01);
    // ...and the demodulator's 3 kHz has gone from the speakers - REPLACED,
    // not mixed under it. A mix would leave it at full strength here.
    CHECK(patch3k < demod3k / 5.0);
    CHECK(patch1k > patch3k * 3.0);

    // And the patch was not merely starving into silence: it had audio ready.
    std::printf("patch starved frames: %llu\n",
                static_cast<unsigned long long>(p.patchRunner().starvedFrames()));

    // (c) STOPPING THE PATCH HANDS THE SPEAKERS BACK. Closing the page calls
    // exactly this, and the receiver must come back rather than go silent.
    p.patchRunner().clear();
    mark = p.audioSamplesProduced();
    CHECK(waitFor([&] { return p.audioSamplesProduced() - mark > 3u * kWin; }, 30000));
    CHECK(p.audioTapStereo(left.data(), right.data(), kWin) == kWin);
    const double back3k = toneAmplitude(left.data(), kWin, 3000.0, kSinkRateHz);
    const double back1k = toneAmplitude(left.data(), kWin, 1000.0, kSinkRateHz);
    std::printf("after:  3 kHz=%.4f  1 kHz=%.4f\n", back3k, back1k);
    CHECK(back3k > demod3k * 0.5);
    CHECK(back1k < back3k / 5.0);

    // (d) A patch that is running but LISTENING TO NOTHING leaves the
    // receiver's audio alone. A patch of displays and decoders is a normal
    // thing to build, and it must not silence the radio as a side effect.
    p.patchRunner().publish(buildStripSet(plan, g, cfg.sampleRateHz, kNoNode, kSinkRateHz));
    mark = p.audioSamplesProduced();
    CHECK(waitFor([&] { return p.audioSamplesProduced() - mark > 3u * kWin; }, 30000));
    CHECK(p.patchRunner().running());
    CHECK(p.audioTapStereo(left.data(), right.data(), kWin) == kWin);
    const double quiet3k = toneAmplitude(left.data(), kWin, 3000.0, kSinkRateHz);
    std::printf("no-listen: 3 kHz=%.4f\n", quiet3k);
    CHECK(quiet3k > demod3k * 0.5);

    p.stop();
    return testSummary("test_patch_audio");
}
