// test_pipeline_noisesq.cpp - THE FM NOISE SQUELCH IN A RUNNING CHAIN (engine wiring lane 1, group C).
//
// A narrow-FM carrier with a 1 kHz tone: CLEAN it quiets the discriminator (the noise above 5 kHz is far below the threshold) and the gate is open, the audio plays; with strong
// complex noise added (the signal a squelch exists to hide) the discriminator's high band is loud and the gate is closed, the audio is exactly silent; the level squelch (set wide
// open) is not what did it. Off, the audio plays in both. Outside NFM nothing is applied. Bad thresholds and tails are refused.
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

class NoisyFm final : public cascade::source::IqSource {
public:
    explicit NoisyFm(std::atomic<float>* noise) : noise_(noise), gen_(7), dist_(0.0f, 1.0f) {}
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
        const float sigma = noise_->load();
        for (std::size_t i = 0; i < n; ++i) {
            const double a = 0.6 * std::sin(2.0 * kPi * 1000.0 * static_cast<double>(t_++) / kRate);
            phase_ += 2.0 * kPi * 3000.0 * a / kRate;
            if (phase_ > kPi) { phase_ -= 2.0 * kPi; }
            if (phase_ < -kPi) { phase_ += 2.0 * kPi; }
            dst[i] = std::complex<float>(0.5f * static_cast<float>(std::cos(phase_)) + sigma * dist_(gen_), 0.5f * static_cast<float>(std::sin(phase_)) + sigma * dist_(gen_));
        }
        return n;
    }
    const char* name() const override { return "noisy FM stand-in"; }
    const char* lastError() const override { return ""; }

private:
    std::atomic<float>* noise_;
    std::mt19937 gen_;
    std::normal_distribution<float> dist_;
    std::uint64_t t_ = 0;
    double phase_ = 0.0;
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

double audioRms(const Pipeline& p) {
    std::vector<float> x(4096);
    const std::size_t n = p.audioTap(x.data(), x.size());
    double s = 0.0;
    for (std::size_t i = 0; i < n; ++i) { s += static_cast<double>(x[i]) * x[i]; }
    return n != 0 ? std::sqrt(s / static_cast<double>(n)) : 0.0;
}

}  // namespace

int main() {
    std::printf("test_pipeline_noisesq\n");
    std::atomic<float> noise{0.0f};
    Pipeline::Config cfg;
    cfg.sampleRateHz = kRate;
    cfg.fftSize = 1024;
    cfg.audioEnabled = false;
    Pipeline p(cfg);
    CHECK(!p.noiseSquelchOn());
    CHECK(!p.setNoiseSquelch(true, 5.0, 100.0));
    CHECK(!p.setNoiseSquelch(true, -91.0, 100.0));
    CHECK(!p.setNoiseSquelch(true, -30.0, -1.0));
    CHECK(!p.setNoiseSquelch(true, -30.0, 2001.0));
    CHECK(!p.setNoiseSquelch(true, std::nan(""), 100.0));
    p.setSource(std::make_unique<NoisyFm>(&noise));
    p.setDemodMode(DemodMode::NFM);
    p.setVfoBandwidthHz(12500.0);
    p.setSquelchDb(-100.0f);   // the level squelch is wide open: it is not what closes the audio
    p.start();
    CHECK(waitFor([&] { return p.audioSamplesProduced() > 4800; }, 20000));
    // OFF: clean and noisy both play
    letAudioRun(p, 1.0);
    CHECK(audioRms(p) > 0.01);
    // ON with a threshold between the two levels: clean = open
    CHECK(p.setNoiseSquelch(true, -70.0, 100.0));
    CHECK(p.noiseSquelchOn());
    CHECK(waitFor([&] { return p.noiseSquelchOpen(); }, 15000));
    letAudioRun(p, 0.5);
    std::printf("  clean: noise level %.1f dB, open %d, audio %.3f\n", p.noiseSquelchLevelDb(), p.noiseSquelchOpen() ? 1 : 0, audioRms(p));
    CHECK(p.noiseSquelchOpen());
    CHECK(audioRms(p) > 0.01);
    // noise in: the high band is loud, the gate closes, the audio is exactly silent
    noise.store(0.8f);
    CHECK(waitFor([&] { return !p.noiseSquelchOpen(); }, 15000));
    letAudioRun(p, 0.5);
    std::printf("  noisy: noise level %.1f dB, open %d, audio %.3f\n", p.noiseSquelchLevelDb(), p.noiseSquelchOpen() ? 1 : 0, audioRms(p));
    CHECK(p.noiseSquelchLevelDb() > -60.0);
    CHECK(audioRms(p) == 0.0);
    // off again: the noisy audio plays
    CHECK(p.setNoiseSquelch(false, -70.0, 100.0));
    letAudioRun(p, 0.5);
    CHECK(audioRms(p) > 0.01);
    // on, noise out: opens again
    CHECK(p.setNoiseSquelch(true, -70.0, 100.0));
    noise.store(0.0f);
    CHECK(waitFor([&] { return p.noiseSquelchOpen(); }, 15000));
    // outside NFM nothing is applied and nothing is measured
    noise.store(0.8f);
    p.setDemodMode(DemodMode::AM);
    letAudioRun(p, 1.0);
    CHECK(!p.noiseSquelchOpen());
    CHECK(p.noiseSquelchLevelDb() == -200.0);
    CHECK(audioRms(p) > 0.0);
    p.stop();
    return testSummary("test_pipeline_noisesq");
}
