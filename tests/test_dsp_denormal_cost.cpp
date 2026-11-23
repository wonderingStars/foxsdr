// DOES A DENORMAL-HEAVY INPUT SLOW A DSP BLOCK ON THIS BUILD? (2026-11-23, the ringdrop diagnosis, item 6)
//
// The diagnosis found nothing in src that sets flush-to-zero or denormals-are-zero, and every recursive filter on the DSP thread (and inside
// every plugin, which runs on that thread) is exposed whenever its input becomes exact zero or decays through the denormal range. It
// recorded this as "UNVERIFIED that any field report is this". The lane's rule was: do not add FTZ/DAZ unless a test in the lane shows a
// denormal-heavy input measurably slows a DSP block on this build - more than 2x - and report the numbers.
//
// WHAT IS MEASURED. A real Pipeline (AM, notch on, two equaliser bands moved, squelch closed, 2 MS/s, 1024-point blocks, no sound device) is
// fed three kinds of input in turn, and the DSP thread's own wall time per block (Pipeline::dspBusyNs / dspBlocks) is taken for each:
//   noise   ordinary signal;
//   zeros   exact digital silence after the noise (the closed-squelch and muted-demodulator case: every recursive filter's state then
//           decays through the denormal range);
//   decay   a tone whose amplitude falls by e^-99 over the phase, so the last part of it is made of denormal floats themselves.
// Each is run with the DSP thread leaving the floating-point control register alone (the seam Pipeline::setDspFlushDenormalsForTest(false)),
// and as the product runs it: flush-to-zero and denormals-are-zero set on the DSP thread itself, which the product does because of this
// measurement (this machine: noise 1.0x, exact silence 1.1-1.2x, denormal-valued samples 5.4-5.9x slower without it). The numbers are printed.
// THE CHECKS pin the outcome: with the register left alone a denormal block costs more than twice a noise block (so the measurement measures
// something - a machine that ever fails this line proves nothing there), and as the product runs it neither silence nor denormals cost twice
// a noise block.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "core/pipeline.hpp"
#include "source/iq_source.hpp"
#include "test_check.hpp"

namespace {

using cascade::core::Pipeline;

// Self-paced, with a mode: 0 noise, 1 exact zeros, 2 a decaying tone.
class ModeSource final : public cascade::source::IqSource {
public:
    bool start() override { return true; }
    void stop() override {}
    bool running() const override { return true; }
    bool selfPaced() const override { return true; }
    double sampleRateHz() const override { return 2000000.0; }
    bool setSampleRateHz(double) override { return false; }
    double centerFrequencyHz() const override { return 100.0e6; }
    bool setCenterFrequencyHz(double) override { return true; }
    const char* name() const override { return "Denormal test source"; }
    const char* lastError() const override { return ""; }
    std::size_t read(std::complex<float>* dst, std::size_t n) override {
        std::size_t give = 0;
        {
            std::lock_guard<std::mutex> lk(m_);
            give = static_cast<std::size_t>(std::min<std::uint64_t>(budget_, n));
            budget_ -= give;
        }
        const int mode = mode_.load(std::memory_order_relaxed);
        for (std::size_t i = 0; i < give; ++i) {
            if (mode == 0) {
                lcg_ = lcg_ * 1664525u + 1013904223u;
                const float a = (static_cast<float>(lcg_ >> 8) * (2.0f / 16777216.0f) - 1.0f) * 0.2f;
                lcg_ = lcg_ * 1664525u + 1013904223u;
                const float b = (static_cast<float>(lcg_ >> 8) * (2.0f / 16777216.0f) - 1.0f) * 0.2f;
                dst[i] = std::complex<float>(a, b);
            } else if (mode == 1) {
                dst[i] = std::complex<float>(0.0f, 0.0f);
            } else {
                phase_ += 0.05f;
                if (phase_ > 6.2831853f) { phase_ -= 6.2831853f; }
                amp_ *= decay_;
                dst[i] = std::complex<float>(amp_ * std::cos(phase_), amp_ * std::sin(phase_));
            }
        }
        return give;
    }
    void grant(std::uint64_t samples) {
        std::lock_guard<std::mutex> lk(m_);
        budget_ += samples;
    }
    void setMode(int m) { mode_.store(m, std::memory_order_relaxed); }
    // The tone starts at 0.2 and falls to about e^-99 (1e-43: below the smallest normal float, 1.2e-38) over `samples` samples.
    void startDecay(std::uint64_t samples) {
        amp_ = 0.2f;
        decay_ = static_cast<float>(std::exp(-99.0 / static_cast<double>(samples)));
    }
    float amplitude() const { return amp_; }

private:
    std::mutex m_;
    std::uint64_t budget_ = 0;
    std::atomic<int> mode_{0};
    std::uint32_t lcg_ = 0x13572468u;
    float phase_ = 0.0f;
    float amp_ = 0.2f;
    float decay_ = 1.0f;
};

template <typename Pred>
bool waitFor(Pred pred, int timeoutMs = 20000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return pred();
}

struct Result {
    double noiseNs = 0;     // wall time per block
    double zerosNs = 0;     // ...over the last 2000 blocks of silence
    double decayNs = 0;     // ...over the blocks whose samples are denormal
    double decayAllNs = 0;  // ...over the whole decay phase
};

// Feeds `blocks` blocks in steps the ring can hold, waiting for the DSP thread to take each; returns the DSP thread's wall time per block over
// the blocks counted from `skip` on (the first `skip` blocks of the phase are fed but not timed: a filter's state is still settling).
double feed(Pipeline& p, ModeSource& src, int blocks, int skip, bool denormalOnly = false) {
    constexpr int kStep = 100;
    std::uint64_t timedNs = 0;
    std::uint64_t timedBlocks = 0;
    for (int done = 0; done < blocks; done += kStep) {
        const std::uint64_t b0 = p.dspBlocks();
        const std::uint64_t n0 = p.dspBusyNs();
        const bool denormalNow = src.amplitude() < 1.1754944e-38f;
        src.grant(static_cast<std::uint64_t>(kStep) * 1024u);
        CHECK(waitFor([&] { return p.dspBlocks() >= b0 + kStep; }));
        if (done >= skip && (!denormalOnly || denormalNow)) {
            timedNs += p.dspBusyNs() - n0;
            timedBlocks += p.dspBlocks() - b0;
        }
    }
    return timedBlocks == 0 ? 0.0 : static_cast<double>(timedNs) / static_cast<double>(timedBlocks);
}

// flush: 0 = the seam says leave the register alone, -1 = the seam is not touched (the product as it ships).
Result run(int flush) {
    if (flush >= 0) { Pipeline::setDspFlushDenormalsForTest(flush != 0); }
    Pipeline::Config cfg;
    cfg.sampleRateHz = 2000000.0;
    cfg.fftSize = 1024;
    cfg.averagingAlpha = 1.0f;
    cfg.audioEnabled = false;
    Pipeline p(cfg);
    auto owned = std::make_unique<ModeSource>();
    ModeSource* src = owned.get();
    p.setSource(std::move(owned));
    p.setDemodMode(cascade::dsp::DemodMode::AM);
    p.setNotchEnabled(true);
    p.setNotchFrequencyHz(1000.0);
    p.setEqBand(2, 6.0);
    p.setEqBand(5, -6.0);
    p.setSquelchDb(30.0f);   // far above anything here: closed, so the demodulated audio is exact digital silence
    p.start();
    Result r;
    src->setMode(0);
    r.noiseNs = feed(p, *src, 2000, 500);
    src->setMode(1);
    r.zerosNs = feed(p, *src, 6000, 4000);
    src->setMode(2);
    src->startDecay(3000ull * 1024u);
    r.decayAllNs = feed(p, *src, 3000, 0);
    // (the amplitude is now at its end: below the smallest normal float) one more stretch made only of denormal samples
    src->startDecay(1000ull * 1024u);
    src->setMode(2);
    r.decayNs = feed(p, *src, 1000, 0, true);
    p.stop();
    Pipeline::setDspFlushDenormalsForTest(true);   // (the default again)
    return r;
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const Result on = run(-1);   // as shipped, FIRST: the seam has not been touched in this process
    const Result off = run(0);
    std::printf("per DSP block, wall time (us)         noise   zeros   decay(all)  decay(denormal part)\n");
    std::printf("  register left alone (no FTZ/DAZ)  %7.1f %7.1f %9.1f %12.1f\n", off.noiseNs / 1000.0, off.zerosNs / 1000.0,
                off.decayAllNs / 1000.0, off.decayNs / 1000.0);
    std::printf("  as the product runs (FTZ + DAZ)   %7.1f %7.1f %9.1f %12.1f\n", on.noiseNs / 1000.0, on.zerosNs / 1000.0,
                on.decayAllNs / 1000.0, on.decayNs / 1000.0);
    const double zerosRatio = on.zerosNs > 0 ? off.zerosNs / on.zerosNs : 0;
    const double decayRatio = on.decayNs > 0 ? off.decayNs / on.decayNs : 0;
    const double noiseRatio = on.noiseNs > 0 ? off.noiseNs / on.noiseNs : 0;
    std::printf("slowdown without FTZ/DAZ: noise %.2fx, zeros %.2fx, denormal decay %.2fx\n", noiseRatio, zerosRatio, decayRatio);
    CHECK(off.noiseNs > 0 && on.noiseNs > 0);
    CHECK(off.zerosNs > 0 && on.zerosNs > 0);
    CHECK(off.decayNs > 0 && on.decayNs > 0);
    // The measurement measures something: left alone, a block of denormal-valued samples is more than twice a noise block...
    CHECK(off.decayNs > 2.0 * off.noiseNs);
    // ...and as the product runs it, neither silence nor denormals cost twice a noise block.
    CHECK(on.zerosNs < 2.0 * on.noiseNs);
    CHECK(on.decayNs < 2.0 * on.noiseNs);
    return testSummary("test_dsp_denormal_cost");
}
