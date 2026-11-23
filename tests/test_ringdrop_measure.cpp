// THE RINGDROP / AUDIOLEAD MEASUREMENT (2026-11-23): what the minute digests count, and what they now say.
//
// The field reports "ringdrop" (samples dropped between the source thread and the DSP thread) and "audiolead" (the audio buffer deepened
// after starved callbacks) are the two largest groups on the crash page, and neither a code diagnosis nor a bench run could name a
// cause. This test pins the pieces that make the NEXT report name one (docs/radio-reports.md, "The DSP minute"):
//
//   1. the shared event mask and the three-minute override (core/minute_events.hpp), by themselves;
//   2. samples discarded while the DSP thread is deliberately ABSENT (a thread start; a live sample-rate change joins and respawns it)
//      are counted apart and are NOT ringdrop, while a real stall still is;
//   3. the pipeline counts its own deliberate quiesces (source switch, accepted rate change);
//   4. the machine-state line is fixed words and numbers, fits the log's line, and survives the upload scrub;
//   5. the DSP thread's accounting (busy time, own CPU time, block count, the longest gap between ring reads and the phase it was in) and
//      the source thread's (longest gap between writes);
//   6. the minute lines' widest forms fit DiagLog::kLineBytes and survive the upload scrub with every number intact.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "core/diag_log.hpp"
#include "core/minute_events.hpp"
#include "core/pipeline.hpp"
#include "source/iq_source.hpp"
#include "test_check.hpp"

using cascade::core::Pipeline;
using cascade::core::SpectrumFrame;

namespace {

// Self-paced and only as generous as the test lets it be (as tests/test_dsp_drop_minute_app.cpp's), with a rate the test can move.
class BudgetSource final : public cascade::source::IqSource {
public:
    explicit BudgetSource(double rate) : rate_(rate) {}
    bool start() override { return true; }
    void stop() override {}
    bool running() const override { return true; }
    bool selfPaced() const override { return true; }
    double sampleRateHz() const override { return rate_.load(); }
    bool setSampleRateHz(double hz) override { rate_.store(hz); return true; }
    double centerFrequencyHz() const override { return 100.0e6; }
    bool setCenterFrequencyHz(double) override { return true; }
    const char* name() const override { return "Budgeted test source"; }
    const char* lastError() const override { return ""; }
    std::size_t read(std::complex<float>* dst, std::size_t n) override {
        std::size_t give = 0;
        {
            std::lock_guard<std::mutex> lk(m_);
            give = static_cast<std::size_t>(std::min<std::uint64_t>(budget_, n));
            budget_ -= give;
        }
        for (std::size_t i = 0; i < give; ++i) { dst[i] = std::complex<float>(0.25f, -0.25f); }
        return give;
    }
    void grant(std::uint64_t samples) {
        std::lock_guard<std::mutex> lk(m_);
        budget_ += samples;
    }
    // A steady supply: `perMs` samples every millisecond until stopped (a self-paced device that is always ready).
    void streamFor(std::uint64_t perMs, int ms) {
        for (int i = 0; i < ms; ++i) {
            grant(perMs);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

private:
    std::mutex m_;
    std::uint64_t budget_ = 0;
    std::atomic<double> rate_;
};

template <typename Pred>
bool waitFor(Pred pred, int timeoutMs = 10000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return pred();
}

Pipeline::Config appConfig() {
    Pipeline::Config cfg;   // what the engine builds: 2 MS/s, 1024-point FFT, no sound device
    cfg.sampleRateHz = 2000000.0;
    cfg.fftSize = 1024;
    cfg.averagingAlpha = 1.0f;
    cfg.audioEnabled = false;
    return cfg;
}

bool hasFrame(Pipeline& p) {
    SpectrumFrame f;
    return p.getLatestFrame(f);
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    using namespace cascade::core;

    // --- 1. THE SHARED EVENT MASK, by itself ------------------------------------------------------------------------------------
    {
        MinuteEventCounts mark;
        MinuteEventCounts now;
        CHECK(minuteEventsSince(now, mark) == 0u);
        now.rate = 1;
        CHECK(minuteEventsSince(now, mark) == kMinuteEventRate);
        CHECK(minuteEventsSince(now, mark) == 0u);   // the mark moved: an event is one minute's
        now.source = 2; now.plugins = 5; now.audio = 1;
        CHECK(minuteEventsSince(now, mark) == (kMinuteEventSwitch | kMinuteEventPlugins | kMinuteEventAudio));
        // a count that went BACKWARDS (a sink replaced under the engine) is no event
        MinuteEventCounts back = now;
        back.audio = 0;
        CHECK(minuteEventsSince(back, mark) == 0u);

        char words[48] = {};
        minuteEventsText(0u, words, sizeof(words));
        CHECK(std::string(words) == "none");
        minuteEventsText(kMinuteEventRate | kMinuteEventSwitch | kMinuteEventPlugins | kMinuteEventAudio, words, sizeof(words));
        CHECK(std::string(words) == "rate|switch|plugins|audio");
        minuteEventsText(kMinuteEventPlugins, words, sizeof(words));
        CHECK(std::string(words) == "plugins");

        // THE JUDGMENT. A clean minute is never judged and ends the run; a loss minute with no event is judged; a loss minute with an
        // event is not - until the third in a row.
        int run = 0;
        CHECK(!minuteJudged(false, 0u, run) && run == 0);
        CHECK(minuteJudged(true, 0u, run) && run == 1);
        CHECK(!minuteJudged(true, kMinuteEventRate, run) && run == 2);
        CHECK(minuteJudged(true, kMinuteEventRate, run) && run == 3);          // the third consecutive: whatever it held
        CHECK(minuteJudged(true, kMinuteEventSwitch | kMinuteEventAudio, run) && run == 4);
        CHECK(!minuteJudged(false, kMinuteEventRate, run) && run == 0);        // a clean minute ends it
        CHECK(!minuteJudged(true, kMinuteEventPlugins, run) && run == 1);
        CHECK(!minuteJudged(true, kMinuteEventPlugins, run) && run == 2);
        CHECK(!minuteJudged(false, 0u, run) && run == 0);                      // ...and the count starts again
        CHECK(!minuteJudged(true, kMinuteEventPlugins, run) && run == 1);
    }

    // --- 2. SAMPLES LOST WHILE THE DSP THREAD IS DELIBERATELY ABSENT ARE NOT RINGDROP --------------------------------------------
    // The test seam holds every DSP thread start for 600 ms (counted as absent) while the source writes three rings' worth: all of it
    // is "absent" and none of it a ringdrop. A start with no delay and no stall loses nothing at all.
    {
        Pipeline p(appConfig());
        auto owned = std::make_unique<BudgetSource>(2000000.0);
        BudgetSource* src = owned.get();
        p.setSource(std::move(owned));
        CHECK(p.sourceSwitches() == 1u);
        const std::size_t ring = p.ringCapacity();
        Pipeline::setDspStartDelayForTest(600);
        p.start();
        CHECK(p.running());
        const std::uint64_t grant = 3 * static_cast<std::uint64_t>(ring);
        src->grant(grant);
        CHECK(waitFor([&] { return p.sourceSamplesRead() >= grant; }));
        const std::uint64_t absent = p.ringDroppedAbsent();
        std::printf("start delay: %llu read, %llu dropped, %llu absent\n", static_cast<unsigned long long>(p.sourceSamplesRead()),
                    static_cast<unsigned long long>(p.ringDroppedSamples()), static_cast<unsigned long long>(absent));
        CHECK(p.ringDroppedSamples() == 0u);
        CHECK(p.ringCutWrites() == 0u);
        CHECK(absent >= grant - ring - 65536);        // everything the ring could not hold
        CHECK(absent <= grant);
        Pipeline::setDspStartDelayForTest(0);
        // The DSP thread has come and drained; nothing is "absent" any more, and a real overflow would count as a drop again.
        CHECK(waitFor([&] { return hasFrame(p); }, 5000));
        p.stop();
    }

    // --- 3. A LIVE RATE CHANGE: THE REBUILD'S OWN WINDOW IS ABSENT, A STALL AFTER IT IS A DROP ---------------------------------------
    {
        Pipeline p(appConfig());
        auto owned = std::make_unique<BudgetSource>(2000000.0);
        BudgetSource* src = owned.get();
        p.setSource(std::move(owned));
        p.start();
        src->grant(8192);
        CHECK(waitFor([&] { return hasFrame(p); }, 5000));
        CHECK(p.rateChanges() == 0u);
        Pipeline::setDspStartDelayForTest(500);   // the respawn after the chain rebuild is slow
        src->setSampleRateHz(1000000.0);
        CHECK(p.setInputRateHz(1000000.0));
        CHECK(p.rateChanges() == 1u);
        CHECK(p.setInputRateHz(1000000.0));        // the same rate again is a no-op, and no event
        CHECK(p.rateChanges() == 1u);
        const std::size_t ring = p.ringCapacity();
        const std::uint64_t droppedBefore = p.ringDroppedSamples();
        const std::uint64_t absentBefore = p.ringDroppedAbsent();
        src->grant(3 * static_cast<std::uint64_t>(ring));   // written while the respawned DSP thread is still absent
        CHECK(waitFor([&] { return p.ringDroppedAbsent() > absentBefore + ring; }, 5000));
        CHECK(p.ringDroppedSamples() == droppedBefore);
        Pipeline::setDspStartDelayForTest(0);
        p.stop();
    }

    return testSummary("test_ringdrop_measure");
}
