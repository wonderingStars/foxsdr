// test_pipeline_cwpeak.cpp - THE CW PEAK FILTER IN A RUNNING CHAIN (engine serial lane A, row F8).
//
// A steady carrier on the tuned frequency (plus noise) beats at the CW pitch. With the peak filter on, the pitch comes out louder by the boost while the noise away from it is not touched;
// the filter follows the pitch when the pitch is changed; it does nothing in another mode and nothing when off; the width and boost outside their ranges are refused.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <memory>
#include <random>
#include <thread>
#include <vector>

#include "core/pipeline.hpp"
#include "dsp/demod.hpp"
#include "source/iq_source.hpp"
#include "test_check.hpp"

using cascade::core::Pipeline;
using cascade::dsp::DemodMode;

namespace {

constexpr double kRate = 240000.0;
constexpr double kPi = 3.14159265358979323846;

class CarrierSource final : public cascade::source::IqSource {
public:
    bool start() override {
        running_ = true;
        return true;
    }
    void stop() override { running_ = false; }
    bool running() const override { return running_; }
    bool selfPaced() const override { return false; }
    double sampleRateHz() const override { return kRate; }
    bool setSampleRateHz(double hz) override { return hz == kRate; }
    double centerFrequencyHz() const override { return 0.0; }
    bool setCenterFrequencyHz(double) override { return true; }
    std::size_t read(std::complex<float>* dst, std::size_t n) override {
        std::normal_distribution<double> noise(0.0, 0.15);   // a carrier of 0.4 in noise of 0.15 a side: the noise away from the pitch is real, not the floor of the arithmetic
        for (std::size_t i = 0; i < n; ++i) { dst[i] = std::complex<float>(static_cast<float>(0.4 + noise(rng_)), static_cast<float>(noise(rng_))); }
        return n;
    }
    const char* name() const override { return "carrier stand-in"; }
    const char* lastError() const override { return ""; }

private:
    bool running_ = false;
    std::mt19937_64 rng_{0xc0de};
};

template <typename Pred>
bool waitFor(Pred pred, int timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return pred();
}

void letAudioRun(Pipeline& p, double seconds) {
    const std::uint64_t need = static_cast<std::uint64_t>(seconds * Pipeline::kAudioRateHz);
    const std::uint64_t mark = p.audioSamplesProduced();
    waitFor([&] { return p.audioSamplesProduced() - mark >= need; }, 60000);
}

// Power (linear, Hann-windowed) of the finished audio at `hz`, averaged over several 4096-sample taps.
double powerAt(const Pipeline& p, double hz) {
    double sum = 0.0;
    int taps = 0;
    for (int t = 0; t < 6; ++t) {
        std::vector<float> x(4096);
        const std::size_t n = p.audioTap(x.data(), x.size());
        double c = 0.0, s = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            const double ph = 2.0 * kPi * hz * static_cast<double>(i) / Pipeline::kAudioRateHz;
            const double w = 0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) / static_cast<double>(n));   // Hann: the tone's own sidelobes must not pass for noise
            c += x[i] * w * std::cos(ph);
            s += x[i] * w * std::sin(ph);
        }
        if (n != 0) {
            sum += (c * c + s * s) / static_cast<double>(n) / static_cast<double>(n);
            ++taps;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
    }
    return taps != 0 ? sum / taps : 0.0;
}
// Mean square of the finished audio over several taps (a broadband figure: its spread is a fraction of a dB).
double totalPower(const Pipeline& p) {
    double sum = 0.0;
    std::size_t count = 0;
    for (int t = 0; t < 8; ++t) {
        std::vector<float> x(4096);
        const std::size_t n = p.audioTap(x.data(), x.size());
        for (std::size_t i = 0; i < n; ++i) { sum += static_cast<double>(x[i]) * x[i]; }
        count += n;
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
    }
    return count != 0 ? sum / static_cast<double>(count) : 0.0;
}
// The audio away from the pitch: the mean (in linear power) of nineteen frequencies from 1,200 to 3,000 Hz, so one bin's own scatter (a few dB for a single bin) does not pass for the filter.
double offBand(const Pipeline& p) {
    double sum = 0.0;
    int n = 0;
    for (double hz = 1200.0; hz <= 3000.0; hz += 100.0, ++n) { sum += powerAt(p, hz); }
    return sum / n;
}
double db(double p) { return 10.0 * std::log10(std::max(p, 1e-30)); }

}  // namespace

int main() {
    std::printf("test_pipeline_cwpeak\n");
    Pipeline::Config cfg;
    cfg.sampleRateHz = kRate;
    cfg.fftSize = 1024;
    cfg.audioEnabled = false;
    Pipeline p(cfg);
    CHECK(!p.cwPeakOn() && p.cwPeakWidthHz() == 100.0 && p.cwPeakBoostDb() == 12.0);
    p.setSource(std::make_unique<CarrierSource>());
    p.setDemodMode(DemodMode::CW);
    p.setSquelchDb(-100.0f);
    // A fixed gain, not the AGC, and a low one: the tone leaves the chain at about -30 dB so that a 24 dB boost still fits under the output's clip (a clipped tone grows harmonics and would pass for a fault of the filter).
    p.setAgcMode(cascade::dsp::Agc::Mode::Off);
    p.setAgcFixedGain(-20.0f);
    p.start();
    CHECK(waitFor([&] { return p.audioSamplesProduced() > 4800; }, 30000));
    letAudioRun(p, 4.0);   // (the AGC and the noise reduction settle)

    // off, on, off: the figure is the on against the mean of the two offs, so a slow drift of the chain upstream does not pass for the filter
    const double off1Tone = db(powerAt(p, 700.0)), off1Noise = db(offBand(p));
    CHECK(p.setCwPeak(true, 80.0, 12.0));
    CHECK(p.cwPeakOn() && p.cwPeakWidthHz() == 80.0 && p.cwPeakBoostDb() == 12.0);
    letAudioRun(p, 1.0);
    const double onTone = db(powerAt(p, 700.0)), onNoise = db(offBand(p));
    CHECK(p.setCwPeak(false, 80.0, 12.0));
    letAudioRun(p, 1.0);
    const double off2Tone = db(powerAt(p, 700.0)), off2Noise = db(offBand(p));
    const double offTone = 0.5 * (off1Tone + off2Tone), offNoise = 0.5 * (off1Noise + off2Noise);
    std::printf("  at the pitch: %.1f / %.1f dB off, %.1f dB on (%+.1f); noise 1200-3000 Hz: %.1f / %.1f dB off, %.1f dB on (%+.1f)\n", off1Tone, off2Tone, onTone, onTone - offTone, off1Noise, off2Noise, onNoise, onNoise - offNoise);
    CHECK(std::fabs(off1Tone - off2Tone) < 2.0);   // the chain is steady
    CHECK(std::fabs((onTone - offTone) - 12.0) < 2.5);
    CHECK(std::fabs(onNoise - offNoise) < 1.5);

    // the filter follows the pitch: the carrier now beats at 1000 Hz
    CHECK(p.setCwPitchHz(1000.0));
    CHECK(p.setCwPeak(false, 80.0, 12.0));
    letAudioRun(p, 3.0);
    const double movedOff = db(powerAt(p, 1000.0));
    CHECK(p.setCwPeak(true, 80.0, 12.0));
    letAudioRun(p, 1.0);
    const double movedTone = db(powerAt(p, 1000.0));
    std::printf("  pitch 1000 Hz: %.1f dB with the filter, %.1f dB without (%+.1f)\n", movedTone, movedOff, movedTone - movedOff);
    CHECK(std::fabs((movedTone - movedOff) - 12.0) < 2.5);

    // not in another mode
    CHECK(p.setCwPeak(true, 80.0, 24.0));
    p.setDemodMode(DemodMode::USB);
    letAudioRun(p, 1.0);
    const double usbOn = db(totalPower(p));
    CHECK(p.setCwPeak(false, 80.0, 24.0));
    letAudioRun(p, 1.0);
    const double usbOff = db(totalPower(p));
    std::printf("  USB: %.1f dB on, %.1f dB off\n", usbOn, usbOff);
    CHECK(std::fabs(usbOn - usbOff) < 1.0);   // (a 24 dB boost 80 Hz wide would add about 3 dB to this)

    // refusals
    for (const double w : {29.9, 500.1, 0.0, -80.0, std::nan(""), HUGE_VAL}) { CHECK(!p.setCwPeak(true, w, 12.0)); }
    for (const double g : {-0.1, 24.1, std::nan(""), HUGE_VAL}) { CHECK(!p.setCwPeak(true, 100.0, g)); }
    CHECK(!p.cwPeakOn() && p.cwPeakWidthHz() == 80.0 && p.cwPeakBoostDb() == 24.0);
    p.stop();
    return testSummary("test_pipeline_cwpeak");
}
