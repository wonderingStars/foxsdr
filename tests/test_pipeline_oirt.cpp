// test_pipeline_oirt.cpp - the OIRT polar-modulation stereo decoder in the WFM audio path (Pipeline::setStereoSystem; engine wiring W2; dsp/oirt_stereo.hpp).
//
// A REAL FM-modulated OIRT transmission is generated (Rec. ITU-R BS.450-3 section 2.1.2, the same clean-room construction as tests/test_oirt_stereo.cpp: M and S at 80 %,
// pre-emphasised, S amplitude-modulating a 31.25 kHz carrier whose spectrum K(f) = (1 + j6.4f)/(5 + j6.4f) shapes, the carrier ending at 20 %), frequency-modulated at
// 50 kHz peak deviation, and run through a real Pipeline with the demodulator, the channel filter, the resamplers and the AGC. Nothing here reads a figure from the
// implementation.
//
//   O1  with the OIRT system chosen the left-only programme comes out of the LEFT channel: separation well above what mono duplication gives (0 dB), the stereo lamp
//       (pilotLocked / stereoActive) lights and oirtActive() is true
//   O2  the same signal through the PILOT-TONE decoder (the default) is mono: no lamp, left and right alike - proving O1's separation is the OIRT decoder's doing
//   O3  forcing mono (setStereoEnabled(false)) with the OIRT decoder in the path makes both channels carry the same audio, and the toggle is not a one-way door
//   O4  a setStereoSystem that is neither 1 nor 2 changes nothing
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

#include "core/pipeline.hpp"
#include "dsp/demod.hpp"
#include "source/iq_source.hpp"
#include "test_check.hpp"

namespace {

constexpr double kTwoPi = 6.283185307179586476925286766559;
constexpr double kRateHz = 250000.0;          // decimation 1: the channel is the input, as the RDS test's
constexpr double kPeakDeviationHz = 50000.0;  // the OIRT system's maximum deviation (the pipeline's kOirtMaxDeviationHz)
constexpr double kToneHz = 1000.0;
constexpr double kCarrierHz = 31250.0;

std::complex<double> kOf(double hz) {
    const std::complex<double> jw(0.0, 6.4 * hz / 1000.0);
    return (1.0 + jw) / (5.0 + jw);
}
std::complex<double> preOf(double hz) { return {1.0, kTwoPi * hz * 50e-6}; }

// The OIRT multiplex of a tone in the LEFT channel only (A = tone, B = 0), normalised so +-1 is the maximum deviation.
double multiplex(double t, double leftAmp) {
    const std::complex<double> p = preOf(kToneHz);
    const double ph = std::arg(p);
    const double g = std::abs(p) * leftAmp * 0.8 * 0.5;   // (A + B) / 2 and (A - B) / 2, 80 % each, pre-emphasised
    const double m = g * std::cos(kTwoPi * kToneHz * t + ph);
    const std::complex<double> k = kOf(kToneHz);
    const double env = 0.2 + g * std::abs(k) * std::cos(kTwoPi * kToneHz * t + ph + std::arg(k));   // S = +g: A - B = A
    return m + env * std::cos(kTwoPi * kCarrierHz * t + 0.7);
}

class OirtSource : public cascade::source::IqSource {
public:
    explicit OirtSource(double leftAmp) : leftAmp_(leftAmp) {}
    bool start() override {
        running_ = true;
        return true;
    }
    void stop() override { running_ = false; }
    bool running() const override { return running_; }
    bool selfPaced() const override { return false; }
    double sampleRateHz() const override { return kRateHz; }
    bool setSampleRateHz(double) override { return false; }
    double centerFrequencyHz() const override { return centerHz_; }
    bool setCenterFrequencyHz(double hz) override {
        centerHz_ = hz;
        return true;
    }
    std::size_t read(std::complex<float>* dst, std::size_t n) override {
        for (std::size_t i = 0; i < n; ++i) {
            const double t = static_cast<double>(sample_) / kRateHz;
            ++sample_;
            phase_ += kTwoPi * kPeakDeviationHz * multiplex(t, leftAmp_) / kRateHz;
            if (phase_ > kTwoPi) { phase_ -= kTwoPi; }
            if (phase_ < -kTwoPi) { phase_ += kTwoPi; }
            dst[i] = std::complex<float>(static_cast<float>(std::cos(phase_)), static_cast<float>(std::sin(phase_)));
        }
        return n;
    }
    const char* name() const override { return "OIRT stereo test transmitter"; }
    const char* lastError() const override { return ""; }

private:
    double leftAmp_;
    double centerHz_ = 70.0e6;
    double phase_ = 0.0;
    std::uint64_t sample_ = 0;
    bool running_ = false;
};

bool waitFor(const std::function<bool()>& done, int ms) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        if (done()) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return done();
}

double rms(const std::vector<float>& v, std::size_t n) {
    double acc = 0.0;
    for (std::size_t i = 0; i < n; ++i) { acc += static_cast<double>(v[i]) * static_cast<double>(v[i]); }
    return std::sqrt(acc / static_cast<double>(n > 0 ? n : 1));
}
double rmsDiff(const std::vector<float>& a, const std::vector<float>& b, std::size_t n) {
    double acc = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double d = static_cast<double>(a[i]) - static_cast<double>(b[i]);
        acc += d * d;
    }
    return std::sqrt(acc / static_cast<double>(n > 0 ? n : 1));
}

struct Window {
    double lRms = 0.0, rRms = 0.0, diffOverL = 0.0;
};
Window take(cascade::core::Pipeline& p) {
    constexpr std::size_t kWin = 4096;
    std::vector<float> l(kWin, 0.0f), r(kWin, 0.0f);
    const std::uint64_t base = p.audioSamplesProduced();
    waitFor([&] { return p.audioSamplesProduced() - base >= kWin; }, 20000);
    const std::size_t got = p.audioTapStereo(l.data(), r.data(), kWin);
    Window w;
    if (got != kWin) { return w; }
    w.lRms = rms(l, got);
    w.rRms = rms(r, got);
    w.diffOverL = rmsDiff(l, r, got) / (w.lRms + 1e-12);
    return w;
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    cascade::core::Pipeline::Config cfg;
    cfg.sampleRateHz = kRateHz;
    cfg.fftSize = 1024;
    cfg.averagingAlpha = 0.5f;
    cfg.audioEnabled = false;   // headless: the tap is what gets measured
    cascade::core::Pipeline pipeline(cfg);
    pipeline.setDemodMode(cascade::dsp::DemodMode::WFM);
    pipeline.setVfoBandwidthHz(220000.0);
    pipeline.setSquelchDb(-120.0f);
    pipeline.setStereoEnabled(true);
    pipeline.setSource(std::make_unique<OirtSource>(0.4));
    pipeline.start();

    // O2 first: the default is the pilot-tone decoder, which finds no pilot in an OIRT signal: mono.
    CHECK(pipeline.stereoSystem() == 1);
    CHECK(waitFor([&] { return pipeline.audioSamplesProduced() > 48000; }, 20000));
    {
        const Window w = take(pipeline);
        std::printf("pilot-tone decoder on an OIRT signal: L rms %.5f R rms %.5f |L-R|/|L| %.5f lamp %d oirt %d\n", w.lRms, w.rRms, w.diffOverL,
                    pipeline.pilotLocked() ? 1 : 0, pipeline.oirtActive() ? 1 : 0);
        CHECK(w.lRms > 0.005);                      // there is audio (the mono part)
        CHECK(!pipeline.pilotLocked());             // no pilot, no lamp
        CHECK(!pipeline.stereoActive());
        CHECK(!pipeline.oirtActive());
        CHECK(w.diffOverL < 0.05);                  // L and R alike
    }

    // O1: the OIRT decoder.
    pipeline.setStereoSystem(2);
    CHECK(waitFor([&] { return pipeline.stereoSystem() == 2 && pipeline.oirtActive() && pipeline.stereoActive(); }, 20000));
    std::this_thread::sleep_for(std::chrono::milliseconds(400));   // the decoder settles and the AGC with it
    {
        const Window w = take(pipeline);
        const double sepDb = 20.0 * std::log10((w.lRms + 1e-12) / (w.rRms + 1e-12));
        std::printf("OIRT decoder on an OIRT signal (left only): L rms %.5f R rms %.5f separation %.1f dB lamp %d active %d level %.2f\n", w.lRms, w.rRms, sepDb,
                    pipeline.pilotLocked() ? 1 : 0, pipeline.stereoActive() ? 1 : 0, static_cast<double>(pipeline.pilotLevel()));
        CHECK(w.lRms > 0.01);
        CHECK(sepDb > 12.0);                        // mono duplication would read 0 dB
        CHECK(pipeline.pilotLocked());              // the stereo lamp is the OIRT carrier lock
        CHECK(pipeline.stereoActive());
        CHECK(pipeline.oirtActive());
        CHECK(pipeline.pilotLevel() > 0.5f);        // the carrier is about the nominal 20 %
    }

    // O3: forced mono with the OIRT decoder in the path: both channels alike; and back.
    pipeline.setStereoEnabled(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    {
        const Window w = take(pipeline);
        std::printf("OIRT forced mono: |L-R|/|L| %.5f\n", w.diffOverL);
        CHECK(w.diffOverL < 0.02);
        CHECK(!pipeline.stereoActive());
    }
    pipeline.setStereoEnabled(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    {
        const Window w = take(pipeline);
        std::printf("OIRT stereo again: |L-R|/|L| %.5f\n", w.diffOverL);
        CHECK(w.diffOverL > 0.5);
    }

    // O4: a value that is neither 1 nor 2 is ignored.
    pipeline.setStereoSystem(0);
    pipeline.setStereoSystem(3);
    pipeline.setStereoSystem(-1);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(pipeline.stereoSystem() == 2);

    // Back to the pilot-tone decoder: mono again.
    pipeline.setStereoSystem(1);
    CHECK(waitFor([&] { return pipeline.stereoSystem() == 1 && !pipeline.oirtActive(); }, 20000));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    {
        const Window w = take(pipeline);
        CHECK(w.diffOverL < 0.05);
        CHECK(!pipeline.stereoActive());
    }
    pipeline.stop();
    return testSummary("test_pipeline_oirt");
}
