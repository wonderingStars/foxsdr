// test_pipeline_passband.cpp - THE PASSBAND'S EDGES AND SHIFT IN A RUNNING CHAIN (screen lane S1, wave 8, rows G1-35 and G1-34).
//
// A steady tone (with noise) sits some way from the tuned frequency; the channel power the S-meter reads AFTER the passband band-pass (Pipeline::signalPowerDb) says whether the tone is inside the band.
//   A  off (the default): a tone near and a tone far are both heard at the same level, and nothing is in the chain;  B  on in USB with 300 to 2700 Hz: the tone inside stays, the tone beyond the high cut is
//   down at least 15 dB;  C  a shift of +1500 Hz moves the band: the tone that was inside is now below it and the tone at 3000 Hz is inside;  D  LSB mirrors it: the tone at -1500 Hz is inside, the one at
//   +1500 Hz is not;  E  a mode with no passband (WFM) leaves the tone alone while the setting stays on, and the filter reports it is not in the chain;  F  off again gives the first levels back;
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

class ToneSource final : public cascade::source::IqSource {
public:
    std::atomic<double> freqHz{1500.0};
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
        const double f = freqHz.load();
        for (std::size_t i = 0; i < n; ++i) {
            phase_ += 2.0 * kPi * f / kRate;
            if (phase_ > 2.0 * kPi) { phase_ -= 2.0 * kPi; }
            if (phase_ < -2.0 * kPi) { phase_ += 2.0 * kPi; }
            dst[i] = std::complex<float>(static_cast<float>(0.3 * std::cos(phase_) + noise(rng_)), static_cast<float>(0.3 * std::sin(phase_) + noise(rng_)));
        }
        return n;
    }
    const char* name() const override { return "tone stand-in"; }
    const char* lastError() const override { return ""; }

private:
    bool running_ = false;
    double phase_ = 0.0;
    std::mt19937_64 rng_{0x51};
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

// The channel power (dB) with the tone at `hz`, once the meter has settled on it.
double levelAt(Pipeline& p, ToneSource& src, double hz) {
    src.freqHz.store(hz);
    std::this_thread::sleep_for(std::chrono::milliseconds(1800));
    double sum = 0.0;
    for (int i = 0; i < 10; ++i) {
        sum += static_cast<double>(p.signalPowerDb());
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
    }
    return sum / 10.0;
}

}  // namespace

int main() {
    std::printf("test_pipeline_passband\n");
    Pipeline::Config cfg;
    cfg.sampleRateHz = kRate;
    cfg.fftSize = 1024;
    cfg.audioEnabled = false;
    Pipeline p(cfg);
    auto srcOwn = std::make_unique<ToneSource>();
    ToneSource* src = srcOwn.get();
    p.setSource(std::move(srcOwn));
    p.setDemodMode(DemodMode::USB);
    p.setVfoBandwidthHz(6000.0);
    p.setSquelchDb(-100.0f);
    p.start();
    CHECK(waitFor([&] { return p.audioSamplesProduced() > 4800; }, 30000));

    // A: off by default; near and far tones are heard alike
    CHECK(!p.passbandOn() && !p.passbandActive() && p.passbandLowCutHz() == 300.0 && p.passbandHighCutHz() == 2700.0 && p.passbandShiftHz() == 0.0);
    const double near0 = levelAt(p, *src, 1500.0);
    const double far0 = levelAt(p, *src, 5000.0);
    std::printf("  A off: tone at 1500 Hz %.1f dB, tone at 5000 Hz %.1f dB\n", near0, far0);
    CHECK(near0 > -30.0 && far0 > -30.0);
    CHECK(std::fabs(near0 - far0) < 3.0);

    // B: on, 300 to 2700 Hz in USB
    CHECK(p.setPassband(true, 300.0, 2700.0, 0.0));
    CHECK(waitFor([&] { return p.passbandActive(); }, 5000));
    const double nearB = levelAt(p, *src, 1500.0);
    const double farB = levelAt(p, *src, 5000.0);
    std::printf("  B 300..2700: tone at 1500 Hz %.1f dB (was %.1f), tone at 5000 Hz %.1f dB (was %.1f)\n", nearB, near0, farB, far0);
    CHECK(std::fabs(nearB - near0) < 3.0);
    CHECK(farB < far0 - 15.0);

    // C: a shift of +1500 Hz: the band is 1800 to 4200
    CHECK(p.setPassband(true, 300.0, 2700.0, 1500.0));
    const double lowC = levelAt(p, *src, 500.0);
    const double midC = levelAt(p, *src, 3000.0);
    std::printf("  C shift +1500: tone at 500 Hz %.1f dB, tone at 3000 Hz %.1f dB\n", lowC, midC);
    CHECK(std::fabs(midC - near0) < 3.0);
    CHECK(lowC < near0 - 15.0);
    CHECK(p.passbandShiftHz() == 1500.0);

    // D: LSB mirrors it (the band is -2700 to -300 with no shift)
    CHECK(p.setPassband(true, 300.0, 2700.0, 0.0));
    p.setDemodMode(DemodMode::LSB);
    CHECK(waitFor([&] { return p.passbandActive(); }, 5000));
    const double lsbIn = levelAt(p, *src, -1500.0);
    const double lsbOut = levelAt(p, *src, 1500.0);
    std::printf("  D LSB: tone at -1500 Hz %.1f dB, tone at +1500 Hz %.1f dB\n", lsbIn, lsbOut);
    CHECK(lsbIn > lsbOut + 15.0);
    p.setDemodMode(DemodMode::USB);

    // E: WFM has no passband: the setting stays on, the filter is out of the chain and the tone at 5000 Hz is not touched
    p.setDemodMode(DemodMode::WFM);
    CHECK(waitFor([&] { return !p.passbandActive(); }, 5000));
    CHECK(p.passbandOn());
    const double wfmFar = levelAt(p, *src, 5000.0);
    std::printf("  E WFM: tone at 5000 Hz %.1f dB (the passband is on but not in the chain)\n", wfmFar);
    CHECK(wfmFar > -30.0);
    p.setDemodMode(DemodMode::USB);
    CHECK(waitFor([&] { return p.passbandActive(); }, 5000));

    // F: off again: the first levels come back
    CHECK(p.setPassband(false, 300.0, 2700.0, 0.0));
    CHECK(waitFor([&] { return !p.passbandActive(); }, 5000));
    const double farF = levelAt(p, *src, 5000.0);
    std::printf("  F off: tone at 5000 Hz %.1f dB (first %.1f)\n", farF, far0);
    CHECK(std::fabs(farF - far0) < 3.0);

    // G: refusals change nothing
    CHECK(!p.setPassband(true, -1.0, 2700.0, 0.0));
    CHECK(!p.setPassband(true, 300.0, 25000.0, 0.0));
    CHECK(!p.setPassband(true, 300.0, 2700.0, 6500.0));
    CHECK(!p.setPassband(true, std::nan(""), 2700.0, 0.0));
    CHECK(!p.setPassband(true, 300.0, HUGE_VAL, 0.0));
    CHECK(!p.passbandOn() && p.passbandHighCutHz() == 2700.0 && p.passbandShiftHz() == 0.0);
    p.stop();
    return testSummary("test_pipeline_passband");
}
