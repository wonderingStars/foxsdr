// test_pipeline_sam.cpp - THE SYNCHRONOUS AM DETECTOR IN A RUNNING CHAIN (screen lane S1, wave 8, row G1-19).
//
// A carrier with ONE sideband (the upper one, 1000 Hz above it) and a little noise is received in AM mode. The envelope detector hears the sideband as a tone; the synchronous detector on both sidebands
// does too; on the upper sideband alone it keeps it; on the lower sideband alone it rejects it. The finished audio (Pipeline::audioTap) says which.
//   A  the envelope (the default): the tone is there, and the loop is not locked;  B  synchronous, both sidebands: the loop locks on the carrier and the tone is there at about the same level;  C  upper only: the
//   tone stays;  D  lower only: the tone is down at least 15 dB;  E  back to the envelope: the tone is back and the lock is let go;  F  the choice is kept in another mode and takes effect when AM is chosen;
//   G  refusals change nothing.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
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
constexpr double kToneHz = 1000.0;

// carrier at +60 Hz (the loop has to find it) and one sideband 1000 Hz above it
class OneSidebandSource final : public cascade::source::IqSource {
public:
    bool start() override { running_ = true; return true; }
    void stop() override { running_ = false; }
    bool running() const override { return running_; }
    bool selfPaced() const override { return false; }
    double sampleRateHz() const override { return kRate; }
    bool setSampleRateHz(double hz) override { return hz == kRate; }
    double centerFrequencyHz() const override { return 0.0; }
    bool setCenterFrequencyHz(double) override { return true; }
    std::size_t read(std::complex<float>* dst, std::size_t n) override {
        std::normal_distribution<double> noise(0.0, 0.002);
        for (std::size_t i = 0; i < n; ++i) {
            carrier_ += 2.0 * kPi * 60.0 / kRate;
            side_ += 2.0 * kPi * (60.0 + kToneHz) / kRate;
            if (carrier_ > 2.0 * kPi) { carrier_ -= 2.0 * kPi; }
            if (side_ > 2.0 * kPi) { side_ -= 2.0 * kPi; }
            const std::complex<double> v = 0.25 * std::polar(1.0, carrier_) + 0.10 * std::polar(1.0, side_);
            dst[i] = std::complex<float>(static_cast<float>(v.real() + noise(rng_)), static_cast<float>(v.imag() + noise(rng_)));
        }
        return n;
    }
    const char* name() const override { return "one-sideband stand-in"; }
    const char* lastError() const override { return ""; }

private:
    bool running_ = false;
    double carrier_ = 0.0, side_ = 0.0;
    std::mt19937_64 rng_{0x77};
};

template <typename Pred>
bool waitFor(Pred pred, int timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return pred();
}

// The level (dB) of the 1000 Hz tone in the last 85 ms of finished audio (a Goertzel sum at the tone, against the audio's rate), once things have settled.
double toneDb(Pipeline& p) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));
    std::vector<float> a(8192);
    const std::size_t got = p.audioTap(a.data(), a.size());
    if (got < 2048) { return -200.0; }
    const double fs = 48000.0;
    const double w = 2.0 * kPi * kToneHz / fs;
    double re = 0.0, im = 0.0;
    for (std::size_t i = 0; i < got; ++i) { re += a[i] * std::cos(w * static_cast<double>(i)); im += a[i] * std::sin(w * static_cast<double>(i)); }
    const double mag = 2.0 * std::sqrt(re * re + im * im) / static_cast<double>(got);
    return 20.0 * std::log10(mag + 1e-12);
}

}  // namespace

int main() {
    std::printf("test_pipeline_sam\n");
    Pipeline::Config cfg;
    cfg.sampleRateHz = kRate;
    cfg.fftSize = 1024;
    cfg.audioEnabled = false;
    Pipeline p(cfg);
    p.setSource(std::make_unique<OneSidebandSource>());
    p.setDemodMode(DemodMode::AM);
    p.setVfoBandwidthHz(10000.0);
    p.setSquelchDb(-100.0f);
    p.start();
    CHECK(waitFor([&] { return p.audioSamplesProduced() > 4800; }, 30000));

    // A: the envelope by default
    CHECK(p.amDetector() == 0 && !p.amDetectorLocked());
    const double env = toneDb(p);
    std::printf("  A envelope: tone %.1f dB, locked %d\n", env, p.amDetectorLocked() ? 1 : 0);
    CHECK(env > -60.0);
    CHECK(!p.amDetectorLocked());

    // B: synchronous on both sidebands
    CHECK(p.setAmDetector(1));
    CHECK(waitFor([&] { return p.amDetector() == 1; }, 5000));
    const bool locked = waitFor([&] { return p.amDetectorLocked(); }, 20000);
    const double both = toneDb(p);
    std::printf("  B synchronous, both: locked %d (carrier at %.1f Hz, presence %.2f), tone %.1f dB (envelope %.1f)\n", locked ? 1 : 0, p.amDetectorOffsetHz(), p.amDetectorPresence(), both, env);
    CHECK(locked);
    CHECK(std::fabs(p.amDetectorOffsetHz() - 60.0) < 25.0);
    CHECK(both > env - 12.0);

    // C: upper only keeps the tone
    CHECK(p.setAmDetector(2));
    CHECK(waitFor([&] { return p.amDetector() == 2; }, 5000));
    const double upper = toneDb(p);
    std::printf("  C upper only: tone %.1f dB\n", upper);
    CHECK(upper > env - 12.0);

    // D: lower only rejects it
    CHECK(p.setAmDetector(3));
    CHECK(waitFor([&] { return p.amDetector() == 3; }, 5000));
    const double lower = toneDb(p);
    std::printf("  D lower only: tone %.1f dB (upper %.1f)\n", lower, upper);
    CHECK(lower < upper - 15.0);

    // E: back to the envelope
    CHECK(p.setAmDetector(0));
    CHECK(waitFor([&] { return p.amDetector() == 0 && !p.amDetectorLocked(); }, 5000));
    const double again = toneDb(p);
    std::printf("  E envelope again: tone %.1f dB (first %.1f)\n", again, env);
    CHECK(std::fabs(again - env) < 4.0);

    // F: the choice is kept while another mode runs and is in force when AM is chosen
    CHECK(p.setAmDetector(3));
    p.setDemodMode(DemodMode::USB);
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    CHECK(p.amDetector() == 3 && !p.amDetectorLocked());
    p.setDemodMode(DemodMode::AM);
    const double back = toneDb(p);
    std::printf("  F lower kept through USB: tone %.1f dB (lower before %.1f)\n", back, lower);
    CHECK(back < env - 12.0);

    // G: refusals change nothing
    CHECK(!p.setAmDetector(4));
    CHECK(!p.setAmDetector(-1));
    CHECK(p.amDetector() == 3);
    p.stop();
    return testSummary("test_pipeline_sam");
}
