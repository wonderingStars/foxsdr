// SHEDDING DISPLAY WORK BEFORE DROPPING RADIO SAMPLES (2026-11-23, the ringdrop measurement; docs/DIAGNOSTICS.md "Shedding display work").
//
// A DSP thread that runs below real time used to do ALL its work for every block and let the source's ring overflow. Now, while the ring holds
// more than half its capacity at the start of a block, the work whose only consumer is the screen is skipped (the spectrum, all but one block in
// eight, and the zoom), and full work resumes under a quarter. This test makes a slow machine with the test seam that burns a chosen number of
// microseconds in a chosen phase of every block, on a source that delivers in real time (2 MS/s: a block of 1024 samples lasts 512 microseconds):
//
//   0. the decision rule, by itself: thresholds, boundary values, hysteresis (no flapping between a quarter and a half);
//   a. the SPECTRUM costs more than the block lasts: without shedding the minute drops samples, with it none, and the audio path gets every block;
//   b. the DEMOD costs more than the block lasts: shedding engages and cannot save it - the drops are still counted (the engine files them);
//   c. HYSTERESIS through the pipeline: the thread enters shedding a handful of times, not once per block;
//   d. WITH HEADROOM nothing is shed, and the spectrum frames are bit-identical to a run with shedding switched off;
//   e. the per-phase time the minute reports adds up and names the phase that was loaded.
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
#include "core/dsp_shed.hpp"
#include "core/pipeline.hpp"
#include "source/iq_source.hpp"
#include "test_check.hpp"

using cascade::core::Pipeline;
using cascade::core::SpectrumFrame;

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

// Hands over exactly what the test grants, one block's worth of a deterministic signal at a time (the bit-for-bit comparison).
class GrantSource final : public cascade::source::IqSource {
public:
    bool start() override { return true; }
    void stop() override {}
    bool running() const override { return true; }
    bool selfPaced() const override { return true; }
    double sampleRateHz() const override { return 2000000.0; }
    bool setSampleRateHz(double) override { return false; }
    double centerFrequencyHz() const override { return 100.0e6; }
    bool setCenterFrequencyHz(double) override { return true; }
    const char* name() const override { return "Granted test source"; }
    const char* lastError() const override { return ""; }
    std::size_t read(std::complex<float>* dst, std::size_t n) override {
        const std::size_t give = std::min<std::size_t>(granted_.load(std::memory_order_acquire), n);
        for (std::size_t i = 0; i < give; ++i) {
            // xorshift noise, the same for every run
            state_ ^= state_ << 13; state_ ^= state_ >> 7; state_ ^= state_ << 17;
            const float re = static_cast<float>(static_cast<std::int32_t>(state_ & 0xFFFFu) - 32768) / 65536.0f;
            const float im = static_cast<float>(static_cast<std::int32_t>((state_ >> 16) & 0xFFFFu) - 32768) / 65536.0f;
            dst[i] = std::complex<float>(re, im);
        }
        granted_.fetch_sub(give, std::memory_order_acq_rel);
        return give;
    }
    void grant(std::size_t n) { granted_.fetch_add(n, std::memory_order_acq_rel); }

private:
    std::atomic<std::size_t> granted_{0};
    std::uint64_t state_ = 0x9E3779B97F4A7C15ull;
};

Pipeline::Config appConfig() {
    Pipeline::Config cfg;   // what the engine builds: 2 MS/s, 1024-point FFT, no sound device
    cfg.sampleRateHz = 2000000.0;
    cfg.fftSize = 1024;
    cfg.averagingAlpha = 1.0f;
    cfg.audioEnabled = false;
    return cfg;
}

template <typename Pred>
bool waitFor(Pred pred, int timeoutMs = 10000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return pred();
}

struct Run {
    std::uint64_t read = 0, dropped = 0, blocks = 0, shedBlocks = 0, shedEntries = 0, audioFrames = 0, ringLeft = 0;
    std::uint64_t phaseNs[Pipeline::kDspPhases] = {};
    double seconds = 0.0;
};

// Runs the real-time source against a pipeline for `ms` with `us` microseconds of load in `phase` and shedding on or off, and returns the counters
// after the source is stopped and the ring has drained (so every sample is either dropped or in a block).
Run runLoaded(const char* phase, int us, bool shedding, int ms, double rate = 2000000.0) {
    Pipeline::setDspShedEnabledForTest(shedding);
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
        r.shedBlocks = p.dspShedBlocks();
        r.shedEntries = p.dspShedEntries();
        r.audioFrames = p.audioSamplesProduced();
        p.dspPhaseTotalsNs(r.phaseNs);
    }
    if (phase != nullptr) { Pipeline::setDspPhaseLoadForTest(phase, 0); }
    Pipeline::setDspShedEnabledForTest(true);
    return r;
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    using cascade::core::dspShedDecision;

    // --- 0. THE DECISION RULE, by itself ------------------------------------------------------------------------------------------------
    {
        constexpr std::size_t cap = 1000;
        // Not shedding: only MORE than half the ring is behind.
        CHECK(!dspShedDecision(false, 0, cap));
        CHECK(!dspShedDecision(false, 500, cap));    // exactly half is not behind
        CHECK(dspShedDecision(false, 501, cap));
        CHECK(dspShedDecision(false, 1000, cap));
        // Shedding: stays on until UNDER a quarter.
        CHECK(dspShedDecision(true, 900, cap));
        CHECK(dspShedDecision(true, 500, cap));
        CHECK(dspShedDecision(true, 251, cap));
        CHECK(dspShedDecision(true, 250, cap));      // exactly a quarter is still shedding
        CHECK(!dspShedDecision(true, 249, cap));
        CHECK(!dspShedDecision(true, 0, cap));
        // A ring of no capacity is never behind.
        CHECK(!dspShedDecision(false, 5, 0));
        CHECK(!dspShedDecision(true, 5, 0));
        // HYSTERESIS: a fill that wanders between 40% and 60% flips the state once (up at the first 501) and never back; a fill that wanders
        // between 20% and 30% flips it once more (down, at the first 249) and never up again. No flapping.
        bool shed = false;
        int flips = 0;
        const std::size_t wander[] = {400, 600, 400, 600, 450, 520, 480, 510, 300, 350, 600, 260, 255, 260, 600, 255};
        for (const std::size_t f : wander) {
            const bool next = dspShedDecision(shed, f, cap);
            if (next != shed) { ++flips; }
            shed = next;
        }
        CHECK(flips == 1 && shed);   // up at 600, then never under 250
        const std::size_t low[] = {240, 300, 245, 260, 249, 300, 400, 499, 240};
        flips = 0;
        for (const std::size_t f : low) {
            const bool next = dspShedDecision(shed, f, cap);
            if (next != shed) { ++flips; }
            shed = next;
        }
        CHECK(flips == 1 && !shed);   // down at 240; 300, 400 and 499 are not over half: it stays down
        CHECK(!dspShedDecision(false, 499, cap));
    }

    // --- baseline: the block's budget at 2 MS/s is 512 microseconds ----------------------------------------------------------------
    {
        const std::string line = cascade::core::dspBaselineText(2000000.0, 1024, 1000, 40000000ull, "demod:61,spectrum:22");
        std::printf("%s\n", line.c_str());
        CHECK(line == "dsp baseline: rate=2000000 block=1024 budget_us=512 cost_us=40.0 headroom=12.8 top=demod:61,spectrum:22");
    }

    // --- a. THE SPECTRUM COSTS MORE THAN THE BLOCK LASTS ---------------------------------------------------------------------------------
    // 1200 microseconds of extra spectrum work per block against a 512-microsecond block. Without shedding the thread keeps up with 40% of the
    // stream and the ring overflows; with it the spectrum runs one block in eight (150 microseconds a block on average) and nothing drops.
    {
        const Run without = runLoaded("spectrum", 1200, /*shedding=*/false, 3000);
        std::printf("a. spectrum +1200 us, NO shedding: read %llu, dropped %llu (%.1f%%), blocks %llu, shed blocks %llu\n",
                    static_cast<unsigned long long>(without.read), static_cast<unsigned long long>(without.dropped),
                    100.0 * static_cast<double>(without.dropped) / static_cast<double>(without.read ? without.read : 1),
                    static_cast<unsigned long long>(without.blocks), static_cast<unsigned long long>(without.shedBlocks));
        CHECK(without.read > 4000000u);
        CHECK(without.dropped * 10u > without.read);   // more than 10% of the stream was lost: the load really does overload the thread
        CHECK(without.shedBlocks == 0u);

        const Run with = runLoaded("spectrum", 1200, /*shedding=*/true, 3000);
        std::printf("a. spectrum +1200 us, shedding:    read %llu, dropped %llu, blocks %llu, shed blocks %llu in %llu entries, audio frames %llu\n",
                    static_cast<unsigned long long>(with.read), static_cast<unsigned long long>(with.dropped),
                    static_cast<unsigned long long>(with.blocks), static_cast<unsigned long long>(with.shedBlocks),
                    static_cast<unsigned long long>(with.shedEntries), static_cast<unsigned long long>(with.audioFrames));
        CHECK(with.read > 4000000u);
        CHECK(with.dropped == 0u);                     // the radio's samples all arrived
        CHECK(with.shedBlocks > 0u);                   // ...because display work was skipped
        // THE AUDIO PATH GOT EVERY BLOCK: every sample read is in a block (less the partial block and what was still in the ring when the
        // thread was stopped), and the audio frames made from them are 48000/2000000 of that, to the rounding of each block.
        CHECK(with.blocks * 1024u <= with.read);
        CHECK(with.blocks * 1024u + 400000u >= with.read);   // under three quarters of a ring (524288) was still waiting at the stop
        const double expectFrames = static_cast<double>(with.blocks) * 1024.0 * 48000.0 / 2000000.0;
        CHECK(static_cast<double>(with.audioFrames) > expectFrames * 0.97);
        CHECK(static_cast<double>(with.audioFrames) < expectFrames * 1.03);
        // C. HYSTERESIS through the pipeline: the thread went into shedding a handful of times for the whole run, not once per few blocks.
        CHECK(with.shedEntries >= 1u);
        CHECK(with.shedEntries * 20u < with.shedBlocks);
    }

    // --- b. THE DEMOD COSTS MORE THAN THE BLOCK LASTS: shedding engages and cannot save it -------------------------------------------------
    {
        const Run r = runLoaded("demod", 1200, /*shedding=*/true, 3000);
        std::printf("b. demod +1200 us, shedding: read %llu, dropped %llu (%.1f%%), shed blocks %llu\n", static_cast<unsigned long long>(r.read),
                    static_cast<unsigned long long>(r.dropped), 100.0 * static_cast<double>(r.dropped) / static_cast<double>(r.read ? r.read : 1),
                    static_cast<unsigned long long>(r.shedBlocks));
        CHECK(r.read > 4000000u);
        CHECK(r.shedBlocks > 0u);                      // it tried
        CHECK(r.dropped * 10u > r.read);               // and the drops are still counted (the engine files them, exactly as before)
    }

    // --- e. THE PER-PHASE TIME names the loaded phase and adds up ---------------------------------------------------------------------------
    {
        const Run r = runLoaded("demod", 300, /*shedding=*/true, 2000);
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

    // --- d. WITH HEADROOM NOTHING IS SHED AND THE SPECTRUM IS BIT-IDENTICAL ---------------------------------------------------------------
    // The same deterministic signal, one block at a time, through a pipeline with shedding compiled in and idle and through one with it switched
    // off: every spectrum frame must match bit for bit, and the idle one must not have shed a block.
    {
        constexpr int kBlocks = 40;
        std::vector<std::vector<float>> frames[2];
        std::uint64_t shed[2] = {0, 0};
        for (int mode = 0; mode < 2; ++mode) {
            Pipeline::setDspShedEnabledForTest(mode == 0);
            Pipeline p(appConfig());
            auto owned = std::make_unique<GrantSource>();
            GrantSource* src = owned.get();
            p.setSource(std::move(owned));
            p.start();
            SpectrumFrame f;
            for (int b = 0; b < kBlocks; ++b) {
                src->grant(1024);
                CHECK(waitFor([&] { return p.dspBlocks() >= static_cast<std::uint64_t>(b + 1); }, 5000));
                CHECK(p.getLatestFrame(f));
                frames[mode].push_back(f.dbBins);
            }
            shed[mode] = p.dspShedBlocks();
            p.stop();
        }
        Pipeline::setDspShedEnabledForTest(true);
        CHECK(shed[0] == 0u && shed[1] == 0u);
        std::size_t same = 0;
        for (int b = 0; b < kBlocks; ++b) {
            if (frames[0][b].size() == frames[1][b].size() && !frames[0][b].empty() &&
                std::memcmp(frames[0][b].data(), frames[1][b].data(), frames[0][b].size() * sizeof(float)) == 0) { ++same; }
        }
        std::printf("d. with headroom: %zu of %d spectrum frames bit-identical, shed blocks %llu / %llu\n", same, kBlocks,
                    static_cast<unsigned long long>(shed[0]), static_cast<unsigned long long>(shed[1]));
        CHECK(same == static_cast<std::size_t>(kBlocks));
        // and the frames are not all one (the check above is not vacuous)
        CHECK(frames[0][0] != frames[0][1]);
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

    return testSummary("test_dsp_shed");
}
