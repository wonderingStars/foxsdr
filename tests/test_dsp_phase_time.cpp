// WHERE THE DSP THREAD'S TIME GOES (2026-11-23, the ringdrop measurement; docs/DIAGNOSTICS.md "Where the time goes").
//
// The pipeline's half of the per-phase accounting and the baseline, with a real-time source and a seam that burns a chosen number of microseconds
// in a chosen phase of every block (2 MS/s: a block of 1024 samples lasts 512 microseconds):
//
//   0. the baseline line's text and arithmetic;
//   e. the per-phase time the minute reports adds up and names the phase that was loaded;
//   g. the idle generator's cost per block at three rates (printed, with only a sanity bound).
//
// (This file was test_dsp_shed.cpp; its sections about shedding display work were removed with the shedding itself, review 2026-11-23.)
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <atomic>
#include <chrono>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "core/dsp_baseline.hpp"
#include "core/pipeline.hpp"
#include "source/iq_source.hpp"
#include "test_check.hpp"

using cascade::core::Pipeline;

namespace {

// A device that delivers in REAL TIME: read() returns n samples when n samples are due by the clock (and at once when it is behind), so a
// consumer that is too slow overflows the ring exactly as it does against a radio. Self-paced, so the pipeline does not pace it.
class PacedSource final : public cascade::source::IqSource {
public:
    explicit PacedSource(double rate) : rate_(rate) {}
    bool start() override { return true; }
    void stop() override {}
    bool running() const override { return true; }
    bool selfPaced() const override { return true; }
    double sampleRateHz() const override { return rate_; }
    bool setSampleRateHz(double) override { return false; }
    double centerFrequencyHz() const override { return 100.0e6; }
    bool setCenterFrequencyHz(double) override { return true; }
    const char* name() const override { return "Paced test source"; }
    const char* lastError() const override { return ""; }
    std::size_t read(std::complex<float>* dst, std::size_t n) override {
        if (!started_) {
            started_ = true;
            begin_ = std::chrono::steady_clock::now();
        }
        delivered_ += n;
        const auto due = begin_ + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(static_cast<double>(delivered_) / rate_));
        std::this_thread::sleep_until(due);
        for (std::size_t i = 0; i < n; ++i) { dst[i] = std::complex<float>(0.25f, -0.25f); }
        return n;
    }

private:
    double rate_;
    bool started_ = false;
    std::chrono::steady_clock::time_point begin_{};
    std::uint64_t delivered_ = 0;
};

Pipeline::Config appConfig() {
    Pipeline::Config cfg;   // what the engine builds: 2 MS/s, 1024-point FFT, no sound device
    cfg.sampleRateHz = 2000000.0;
    cfg.fftSize = 1024;
    cfg.averagingAlpha = 1.0f;
    cfg.audioEnabled = false;
    return cfg;
}

struct Run {
    std::uint64_t read = 0, dropped = 0, blocks = 0, audioFrames = 0;
    std::uint64_t phaseNs[Pipeline::kDspPhases] = {};
    double seconds = 0.0;
};

// Runs the real-time source against a pipeline for `ms` with `us` microseconds of load in `phase`, and returns the counters
// after the source is stopped and the ring has drained (so every sample is either dropped or in a block).
Run runLoaded(const char* phase, int us, int ms, double rate = 2000000.0) {
    if (phase != nullptr) { Pipeline::setDspPhaseLoadForTest(phase, us); }
    Run r;
    {
        Pipeline::Config cfg = appConfig();
        cfg.sampleRateHz = rate;
        Pipeline p(cfg);
        p.setSource(std::make_unique<PacedSource>(rate));
        const auto t0 = std::chrono::steady_clock::now();
        p.start();
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        // the SOURCE stops first (stop() ends both threads; the figures below are read after it, from a ring that holds whatever was left)
        p.stop();
        r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        r.read = p.sourceSamplesRead();
        r.dropped = p.ringDroppedSamples();
        r.blocks = p.dspBlocks();
        r.audioFrames = p.audioSamplesProduced();
        p.dspPhaseTotalsNs(r.phaseNs);
    }
    if (phase != nullptr) { Pipeline::setDspPhaseLoadForTest(phase, 0); }
    return r;
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    // --- baseline: the block's budget at 2 MS/s is 512 microseconds ----------------------------------------------------------------
    {
        const std::string line = cascade::core::dspBaselineText(2000000.0, 1024, 1000, 40000000ull, "demod:61,spectrum:22");
        std::printf("%s\n", line.c_str());
        CHECK(line == "dsp baseline: rate=2000000 block=1024 budget_us=512 cost_us=40.0 headroom=12.8 top=demod:61,spectrum:22");
    }

    // --- e. THE PER-PHASE TIME names the loaded phase and adds up ---------------------------------------------------------------------------
    {
        const Run r = runLoaded("demod", 300, 2000);
        std::uint64_t work = 0;
        unsigned top = 1;
        for (unsigned i = 1; i < Pipeline::kDspPhases; ++i) {
            work += r.phaseNs[i];
            if (r.phaseNs[i] > r.phaseNs[top]) { top = i; }
        }
        std::printf("e. demod +300 us: work %.0f ms over %llu blocks (%.1f us a block), top phase %s %.0f ms\n", static_cast<double>(work) / 1e6,
                    static_cast<unsigned long long>(r.blocks), static_cast<double>(work) / 1000.0 / static_cast<double>(r.blocks ? r.blocks : 1),
                    Pipeline::dspPhaseName(top), static_cast<double>(r.phaseNs[top]) / 1e6);
        CHECK(std::string(Pipeline::dspPhaseName(top)) == "demod");
        CHECK(r.phaseNs[top] >= 300ull * 1000ull * (r.blocks - 1u));     // the whole burn is charged to it
        CHECK(work >= r.phaseNs[top]);
        // the minute's text names it first, and only phases over 1% of the work
        const char* names[Pipeline::kDspPhases];
        for (unsigned i = 0; i < Pipeline::kDspPhases; ++i) { names[i] = Pipeline::dspPhaseName(i); }
        const std::string text = cascade::core::dspPhaseMsText(r.phaseNs, names, Pipeline::kDspPhases, 140);
        std::printf("e. phase_ms=%s\n", text.c_str());
        CHECK(text.rfind("demod:", 0) == 0);
        CHECK(std::string(Pipeline::dspPhaseName(Pipeline::kDspPhases)).empty());
    }

    // --- g. THE IDLE GENERATOR'S COST PER BLOCK AT THREE RATES (item 5; printed, with only a sanity bound) -----------------------------
    // The built-in generator is what a machine with no radio open runs: a full chain on a demonstration signal. Same pipeline, three input
    // rates, the real generator (no seam): the mean time a block of 1024 samples keeps the DSP thread busy, against the time the block lasts.
    for (const double rate : {2000000.0, 1000000.0, 500000.0}) {
        Pipeline::Config cfg = appConfig();
        cfg.sampleRateHz = rate;
        Pipeline p(cfg);
        p.start();
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));   // warm-up
        const std::uint64_t busy0 = p.dspBusyNs(), blocks0 = p.dspBlocks();
        std::uint64_t ph0[Pipeline::kDspPhases];
        p.dspPhaseTotalsNs(ph0);
        std::this_thread::sleep_for(std::chrono::milliseconds(5000));
        const std::uint64_t busy = p.dspBusyNs() - busy0, blocks = p.dspBlocks() - blocks0;
        std::uint64_t ph1[Pipeline::kDspPhases];
        p.dspPhaseTotalsNs(ph1);
        p.stop();
        std::uint64_t ph[Pipeline::kDspPhases];
        const char* names[Pipeline::kDspPhases];
        for (unsigned i = 0; i < Pipeline::kDspPhases; ++i) { ph[i] = ph1[i] - ph0[i]; names[i] = Pipeline::dspPhaseName(i); }
        const std::string line = cascade::core::dspBaselineText(rate, 1024, blocks, busy, cascade::core::dspTopPhasesText(ph, names, Pipeline::kDspPhases));
        std::printf("g. idle generator: %s\n", line.c_str());
        CHECK(blocks > 100u);
        CHECK(busy > 0u);
    }

    return testSummary("test_dsp_phase_time");
}
