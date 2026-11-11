// test_pipeline_cwpitch.cpp - THE CW SIDETONE PITCH IN A RUNNING CHAIN (engine wiring lane 1, group C).
//
// A steady carrier ON the tuned frequency, demodulated in CW, beats at the pitch: 700 Hz by default (as it always did), then wherever the pitch is set (300 to 1200 Hz), kept
// across a change of mode and of input rate; a pitch outside 300..1200 or not a number is refused and changes nothing. Measured as the power of the finished audio at the
// pitch against the power at the default (the Weaver chain's pass band is the voice SSB filter's, unchanged).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <memory>
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
        for (std::size_t i = 0; i < n; ++i) { dst[i] = std::complex<float>(0.5f, 0.0f); }
        return n;
    }
    const char* name() const override { return "carrier stand-in"; }
    const char* lastError() const override { return ""; }

private:
    bool running_ = false;
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

double powerAt(const Pipeline& p, double hz) {
    std::vector<float> x(4096);
    const std::size_t n = p.audioTap(x.data(), x.size());
    double c = 0.0, s = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double ph = 2.0 * kPi * hz * static_cast<double>(i) / Pipeline::kAudioRateHz;
        c += x[i] * std::cos(ph);
        s += x[i] * std::sin(ph);
    }
    return n != 0 ? (c * c + s * s) / static_cast<double>(n) / static_cast<double>(n) : 0.0;
}

// The pitch the audio really has: the strongest of a sweep in 10 Hz steps.
double peakPitch(const Pipeline& p) {
    double best = 0.0, bestHz = 0.0;
    for (double hz = 250.0; hz <= 1300.0; hz += 10.0) {
        const double v = powerAt(p, hz);
        if (v > best) { best = v; bestHz = hz; }
    }
    return bestHz;
}

}  // namespace

int main() {
    std::printf("test_pipeline_cwpitch\n");
    Pipeline::Config cfg;
    cfg.sampleRateHz = kRate;
    cfg.fftSize = 1024;
    cfg.audioEnabled = false;
    Pipeline p(cfg);
    CHECK(p.cwPitchHz() == 700.0);
    for (const double bad : {299.0, 1201.0, 0.0, -700.0, std::nan(""), HUGE_VAL}) { CHECK(!p.setCwPitchHz(bad)); }
    CHECK(p.cwPitchHz() == 700.0);
    p.setSource(std::make_unique<CarrierSource>());
    p.setDemodMode(DemodMode::CW);
    p.setSquelchDb(-100.0f);
    p.start();
    CHECK(waitFor([&] { return p.audioSamplesProduced() > 4800; }, 20000));
    letAudioRun(p, 1.0);
    const double def = peakPitch(p);
    std::printf("  default: beats at %.0f Hz\n", def);
    CHECK(std::fabs(def - 700.0) <= 20.0);
    for (const double want : {450.0, 1000.0, 300.0, 1200.0}) {
        CHECK(p.setCwPitchHz(want));
        CHECK(p.cwPitchHz() == want);
        letAudioRun(p, 1.0);
        const double got = peakPitch(p);
        std::printf("  pitch %.0f -> beats at %.0f Hz\n", want, got);
        CHECK(std::fabs(got - want) <= 20.0);
    }
    // kept across a change of mode (away and back) and of the input rate
    CHECK(p.setCwPitchHz(900.0));
    p.setDemodMode(DemodMode::USB);
    letAudioRun(p, 0.5);
    p.setDemodMode(DemodMode::CW);
    letAudioRun(p, 1.0);
    CHECK(std::fabs(peakPitch(p) - 900.0) <= 20.0);
    CHECK(p.setInputRateHz(kRate));   // (the same rate: a no-op that must not disturb it)
    CHECK(std::fabs(p.cwPitchHz() - 900.0) < 1e-9);
    p.stop();
    return testSummary("test_pipeline_cwpitch");
}
