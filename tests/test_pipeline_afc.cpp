// test_pipeline_afc.cpp - AUTOMATIC FREQUENCY CONTROL IN A RUNNING CHAIN (engine serial lane A, row F13).
//
// An unmodulated carrier (with noise, and a clock that is 25 ppm off) sits some way from the tuned frequency; the AFC nudges the VFO onto it.
//   A  off (the default): nothing is added;  B  on, in AM: a carrier 1,500 Hz away is caught (the correction reaches 1,500 Hz within 40) and the loop locks;  C  a carrier that DRIFTS (100 Hz
//   a second) is followed;  D  a carrier outside the pull-in range is not caught;  E  the user's offset is untouched (vfoOffsetHz() is still the user's) and a retune takes the nudge off;
//   F  a mode with no carrier (USB) leaves the VFO alone;  G  off again takes the nudge off;  H  refusals change nothing; CW (peak mode) finds a keyed carrier too.
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

class CarrierSource final : public cascade::source::IqSource {
public:
    std::atomic<double> freqHz{1500.0};
    std::atomic<double> driftHzPerS{0.0};
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
        std::normal_distribution<double> noise(0.0, 0.003);
        const double drift = driftHzPerS.load();
        for (std::size_t i = 0; i < n; ++i) {
            f_ = freqHz.load() + drift * elapsed_;
            elapsed_ += 1.0 / kRate;
            phase_ += 2.0 * kPi * f_ * 1.000025 / kRate;
            if (phase_ > 2.0 * kPi) { phase_ -= 2.0 * kPi; }
            dst[i] = std::complex<float>(static_cast<float>(0.3 * std::cos(phase_) + noise(rng_)), static_cast<float>(0.3 * std::sin(phase_) + noise(rng_)));
        }
        return n;
    }
    const char* name() const override { return "carrier stand-in"; }
    const char* lastError() const override { return ""; }
    void restartDrift() { elapsed_ = 0.0; }

private:
    bool running_ = false;
    double phase_ = 0.0, f_ = 0.0, elapsed_ = 0.0;
    std::mt19937_64 rng_{0xaf};
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

}  // namespace

int main() {
    std::printf("test_pipeline_afc\n");
    Pipeline::Config cfg;
    cfg.sampleRateHz = kRate;
    cfg.fftSize = 1024;
    cfg.audioEnabled = false;
    Pipeline p(cfg);
    auto srcOwn = std::make_unique<CarrierSource>();
    CarrierSource* src = srcOwn.get();
    p.setSource(std::move(srcOwn));
    p.setDemodMode(DemodMode::AM);
    p.setVfoBandwidthHz(10000.0);
    p.setSquelchDb(-100.0f);
    p.start();
    CHECK(waitFor([&] { return p.audioSamplesProduced() > 4800; }, 30000));

    // A: off by default
    CHECK(!p.afcOn() && p.afcPullInHz() == 2500.0);
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    CHECK(p.afcCorrectionHz() == 0.0 && !p.afcLocked());

    // B: on, a carrier 1,500 Hz away
    CHECK(p.setAfc(true, 2500.0));
    CHECK(p.afcOn());
    const bool caught = waitFor([&] { return std::fabs(p.afcCorrectionHz() - 1500.0) < 40.0 && p.afcLocked(); }, 30000);
    std::printf("  B caught: %d, correction %.1f Hz, locked %d\n", caught ? 1 : 0, p.afcCorrectionHz(), p.afcLocked() ? 1 : 0);
    CHECK(caught);
    CHECK(p.vfoOffsetHz() == 0.0);   // the user's own offset is not touched

    // C: a drifting carrier is followed
    src->restartDrift();
    src->driftHzPerS.store(100.0);
    std::this_thread::sleep_for(std::chrono::milliseconds(4000));
    {
        const double want = 1500.0 + 100.0 * 4.0;
        double got = p.afcCorrectionHz();
        std::printf("  C drifting: correction %.1f Hz after 4 s (carrier near %.0f)\n", got, want);
        CHECK(got > 1700.0 && got < 2300.0);   // it follows, within the pull-in rate and the time that passed
    }
    src->driftHzPerS.store(0.0);
    src->freqHz.store(1500.0);
    src->restartDrift();
    CHECK(waitFor([&] { return std::fabs(p.afcCorrectionHz() - 1500.0) < 60.0; }, 30000));

    // F: USB has no carrier: the nudge is taken off and stays off
    p.setDemodMode(DemodMode::USB);
    CHECK(waitFor([&] { return p.afcCorrectionHz() == 0.0; }, 10000));
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    CHECK(p.afcCorrectionHz() == 0.0);
    p.setDemodMode(DemodMode::AM);
    CHECK(waitFor([&] { return std::fabs(p.afcCorrectionHz() - 1500.0) < 60.0; }, 30000));

    // E: a retune takes the nudge off (the user moves the VFO to where the carrier is: nothing is left to nudge)
    p.setVfoOffsetHz(1500.0);
    CHECK(p.vfoOffsetHz() == 1500.0);
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));
    std::printf("  E after a retune onto the carrier: correction %.1f Hz\n", p.afcCorrectionHz());
    CHECK(std::fabs(p.afcCorrectionHz()) < 60.0);
    p.setVfoOffsetHz(0.0);

    // D: outside the pull-in range
    CHECK(waitFor([&] { return std::fabs(p.afcCorrectionHz() - 1500.0) < 60.0; }, 30000));
    src->freqHz.store(9000.0);   // the carrier jumps far away (6,000 Hz beyond the hold range of 5,000)
    std::this_thread::sleep_for(std::chrono::milliseconds(3000));
    {
        const double got = p.afcCorrectionHz();
        std::printf("  D carrier at 9 kHz, pull-in 2.5 kHz: correction %.1f Hz\n", got);
        CHECK(got < 5100.0);   // never past the hold range
    }
    src->freqHz.store(1500.0);

    // G: off again
    CHECK(p.setAfc(false, 2500.0));
    CHECK(waitFor([&] { return p.afcCorrectionHz() == 0.0 && !p.afcLocked(); }, 10000));
    CHECK(p.vfoOffsetHz() == 0.0);

    // H: refusals change nothing
    for (const double bad : {0.0, 49.0, 20001.0, -5.0, std::nan(""), HUGE_VAL}) { CHECK(!p.setAfc(true, bad)); }
    CHECK(!p.afcOn() && p.afcPullInHz() == 2500.0);

    // CW: peak mode finds the carrier
    p.setDemodMode(DemodMode::CW);
    CHECK(p.setAfc(true, 2500.0));
    const bool cw = waitFor([&] { return std::fabs(p.afcCorrectionHz() - 1500.0) < 60.0; }, 30000);
    std::printf("  CW: caught %d, correction %.1f Hz\n", cw ? 1 : 0, p.afcCorrectionHz());
    CHECK(cw);
    p.stop();
    return testSummary("test_pipeline_afc");
}
