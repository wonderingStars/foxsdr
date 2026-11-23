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
#include "core/machine_state.hpp"
#include "core/minute_events.hpp"
#include "core/pipeline.hpp"
#include "core/thread_priority.hpp"
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

// A FREE-RUNNING source (the pipeline paces it, as the built-in generator and a file) whose third read stalls for 400 ms, once: the
// pacing loop falls more than 250 ms behind and resynchronises instead of bursting.
class StallOnceSource final : public cascade::source::IqSource {
public:
    bool start() override { return true; }
    void stop() override {}
    bool running() const override { return true; }
    bool selfPaced() const override { return false; }
    double sampleRateHz() const override { return 2000000.0; }
    bool setSampleRateHz(double) override { return false; }
    double centerFrequencyHz() const override { return 100.0e6; }
    bool setCenterFrequencyHz(double) override { return true; }
    const char* name() const override { return "Stall-once test source"; }
    const char* lastError() const override { return ""; }
    std::size_t read(std::complex<float>* dst, std::size_t n) override {
        if (reads_.fetch_add(1) == 2) {
            stalls_.fetch_add(1);
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
        }
        for (std::size_t i = 0; i < n; ++i) { dst[i] = std::complex<float>(0.25f, -0.25f); }
        return n;
    }
    int stalls() const { return stalls_.load(); }

private:
    std::atomic<int> reads_{0};
    std::atomic<int> stalls_{0};
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

    // --- 4. THE MACHINE LINE: fixed words and numbers, inside the log's line, intact through the upload scrub ---------------------
    {
        const std::string line = machineStateText("mmcss", "none: MMCSS refused (error 1552)");
        std::printf("%s\n", line.c_str());
        CHECK(line.rfind("machine: power=", 0) == 0);
        for (const char* key : {" saver=", " throttle=", " timer_100ns=", " cpus=", " affinity=", " window=", " display=", " prio=dsp:mmcss,src:none"}) {
            CHECK(line.find(key) != std::string::npos);
        }
        CHECK(line.find("1552") == std::string::npos);   // only the first word of an outcome: no error text, no device
        CHECK(line.size() < static_cast<std::size_t>(DiagLog::kLineBytes - 1));
        // The scrub leaves it as it was (a line that "sounds like tuning" has its numbers masked: this one must not).
        CHECK(scrubUploadLine(line) == line);
        // Changed-only logging: the second identical call says nothing; a forced one does.
        resetMachineStateLogForTest();
        CHECK(logMachineStateIfChanged("mmcss", "mmcss", false));
        CHECK(!logMachineStateIfChanged("mmcss", "mmcss", false));
        CHECK(logMachineStateIfChanged("mmcss", "mmcss", true));
        noteDisplayState(1);
        CHECK(logMachineStateIfChanged("mmcss", "mmcss", false));   // the display went off: a change
        CHECK(machineStateText("mmcss", "mmcss").find("display=off") != std::string::npos);
        noteDisplayState(-1);
    }

    // --- 4b. A FAILED MMCSS REGISTRATION IS SAID (2026-11-23) ------------------------------------------------------------------------
    // The outcome of a thread that fell back to above-normal does not carry the reason, so a refused registration was silent. The scope now
    // keeps the reason (a fixed phrase and the Windows error number) for the caller's one warning line. The test seam makes the refusal.
    {
        ThreadPriorityScope::setMmcssDisabledForTest(true);
        {
            const ThreadPriorityScope refused;
#if defined(_WIN32)
            CHECK(!refused.mmcssFailure().empty());
            CHECK(refused.mmcssFailure().find("MMCSS") != std::string::npos);
            CHECK(refused.outcome().rfind("mmcss", 0) != 0);   // (it did not get the class)
#endif
        }
        ThreadPriorityScope::setMmcssDisabledForTest(false);
        {
            const ThreadPriorityScope fine;
#if defined(_WIN32)
            if (fine.outcome() == "mmcss") { CHECK(fine.mmcssFailure().empty()); }   // success says nothing
#endif
        }
    }

    // --- 5. THE DSP THREAD'S ACCOUNTING (the one measurement that settles the rest) ---------------------------------------------------
    // The audio mutex is held for 400 ms while samples arrive: the DSP thread reads one block and then WAITS inside it. Wall time inside
    // blocks must rise by about the hold, its own CPU time must not, and the longest gap between two ring reads must be the hold and name
    // the phase the thread was in - "audio-lock", the step that waits for the mutex. This is what a ringdrop report could never say.
    {
        Pipeline p(appConfig());
        auto owned = std::make_unique<BudgetSource>(2000000.0);
        BudgetSource* src = owned.get();
        p.setSource(std::move(owned));
        p.start();
        src->grant(32768);
        CHECK(waitFor([&] { return p.dspBlocks() >= 16u; }, 5000));
        (void)p.takeDspGapLongest(nullptr);
        const std::uint64_t busy0 = p.dspBusyNs();
        const std::uint64_t blocks0 = p.dspBlocks();
        std::atomic<bool> acquired{false};
        std::thread holder([&] { p.holdLockForTest(Pipeline::LockForTest::Audio, 400, &acquired); });
        CHECK(waitFor([&] { return acquired.load(std::memory_order_acquire); }));
        src->grant(8192);   // eight blocks: the first one waits for the mutex
        holder.join();
        CHECK(waitFor([&] { return p.dspBlocks() >= blocks0 + 8u; }, 5000));
        p.stop();           // the thread's last CPU time is published as it ends
        const std::uint64_t busyMs = (p.dspBusyNs() - busy0) / 1000000u;
        const char* phase = "";
        const std::uint64_t gapMs = p.takeDspGapLongest(&phase) / 1000000u;
        std::printf("held 400 ms: busy +%llu ms, longest gap %llu ms in %s, cpu known %d, cpu %llu ms in total\n",
                    static_cast<unsigned long long>(busyMs), static_cast<unsigned long long>(gapMs), phase, p.dspCpuKnown() ? 1 : 0,
                    static_cast<unsigned long long>(p.dspCpuNs() / 1000000u));
        CHECK(busyMs >= 350u);
        CHECK(busyMs < 5000u);
        CHECK(gapMs >= 350u);
        CHECK(std::string(phase) == "audio-lock");
        CHECK(p.dspBlocks() >= blocks0 + 8u);
        if (p.dspCpuKnown()) {
            // The whole run did a few dozen blocks of work: far less processor time than the 400 ms it spent waiting.
            CHECK(p.dspCpuNs() / 1000000u < 300u);
        }
        // The mark is a window: taking it started a new one.
        CHECK(p.takeDspGapLongest(&phase) / 1000000u < 350u);
    }

    // --- 5b. THE DSP BLOCK DELAY SEAM makes a slow thread: busy time is the blocks' span, the gap names the phase it slept in -------------
    {
        Pipeline p(appConfig());
        auto owned = std::make_unique<BudgetSource>(2000000.0);
        BudgetSource* src = owned.get();
        p.setSource(std::move(owned));
        Pipeline::setDspBlockDelayForTest(30);
        p.start();
        src->grant(1024 * 12);
        CHECK(waitFor([&] { return p.dspBlocks() >= 12u; }, 10000));
        Pipeline::setDspBlockDelayForTest(0);
        p.stop();
        const char* phase = "";
        const std::uint64_t gapMs = p.takeDspGapLongest(&phase) / 1000000u;
        std::printf("30 ms per block: longest gap %llu ms in %s\n", static_cast<unsigned long long>(gapMs), phase);
        CHECK(gapMs >= 25u);
        CHECK(std::string(phase) == "sink");   // the delay is taken after the last beat of a block
    }

    // --- 5c. THE SOURCE THREAD'S LONGEST GAP BETWEEN WRITES ------------------------------------------------------------------------------
    {
        Pipeline p(appConfig());
        auto owned = std::make_unique<BudgetSource>(2000000.0);
        BudgetSource* src = owned.get();
        p.setSource(std::move(owned));
        p.start();
        src->grant(20000);
        CHECK(waitFor([&] { return p.sourceSamplesRead() >= 20000u; }, 5000));
        (void)p.takeSrcGapLongestNs();
        std::this_thread::sleep_for(std::chrono::milliseconds(300));   // the device goes quiet for 300 ms
        src->grant(20000);
        CHECK(waitFor([&] { return p.sourceSamplesRead() >= 40000u; }, 5000));
        const std::uint64_t gapMs = p.takeSrcGapLongestNs() / 1000000u;
        std::printf("quiet device: longest gap between writes %llu ms\n", static_cast<unsigned long long>(gapMs));
        CHECK(gapMs >= 250u);
        CHECK(gapMs < 5000u);
        CHECK(p.takeSrcGapLongestNs() / 1000000u < 250u);   // a window
        p.stop();
    }

    // --- 5d. THE FREE-RUNNING GENERATOR'S RESYNC AFTER A LONG STALL IS COUNTED ----------------------------------------------------------
    {
        Pipeline p(appConfig());
        auto owned = std::make_unique<StallOnceSource>();
        StallOnceSource* src = owned.get();
        p.setSource(std::move(owned));
        CHECK(p.genResyncs() == 0u);
        p.start();
        CHECK(waitFor([&] { return p.genResyncs() >= 1u; }, 5000));
        const std::uint64_t skippedMs = p.genSkippedNs() / 1000000u;
        std::printf("generator stall: %llu resync(s), %llu ms skipped (stalled %d times)\n", static_cast<unsigned long long>(p.genResyncs()),
                    static_cast<unsigned long long>(skippedMs), src->stalls());
        CHECK(p.genResyncs() == 1u);
        CHECK(skippedMs >= 250u);
        CHECK(skippedMs < 2000u);
        p.stop();
    }

    // --- 6. THE MINUTE LINES AT THEIR WIDEST fit the log's line and survive the upload scrub with every number ---------------------------
    {
        const auto cap = [](unsigned long long v) { return v > 9999999999ULL ? 9999999999ULL : v; };
        char a[256];
        std::snprintf(a, sizeof(a),
                      "dsp minute: dropped=%llu pct=%s busy_ms=%llu cpu_ms=%s longest_gap_ms=%llu gap_phase=%s blocks=%llu ring_high_pct=%llu [radio 8]",
                      cap(~0ULL), "100.00", cap(~0ULL), "9999999999", cap(~0ULL), "plugin-audio", cap(~0ULL), 100ULL);
        char b[256];
        std::snprintf(b, sizeof(b), "dsp minute: src_longest_gap_ms=%llu gen_resyncs=%llu gen_skipped_ms=%llu absent_discards=%llu events=%s [radio 8]",
                      cap(~0ULL), cap(~0ULL), cap(~0ULL), cap(~0ULL), "rate|switch|plugins|audio");
        std::printf("widest minute line A: %zu characters; B: %zu\n", std::strlen(a), std::strlen(b));
        // (the log adds "HH:MM:SS.mmm info " before the text in the ring; the budget is for the text itself)
        CHECK(std::strlen(a) < static_cast<std::size_t>(DiagLog::kLineBytes - 1));
        CHECK(std::strlen(b) < static_cast<std::size_t>(DiagLog::kLineBytes - 1));
        // The upload scrub leaves both as they are: no number of theirs is masked as a "tuning".
        CHECK(scrubUploadLine(a) == std::string(a));
        CHECK(scrubUploadLine(b) == std::string(b));
        const std::string typical = "dsp minute: dropped=1048576 pct=66.67 busy_ms=3012 cpu_ms=41 longest_gap_ms=2982 gap_phase=audio-lock blocks=1536 ring_high_pct=100";
        CHECK(scrubUploadLine(typical) == typical);
    }

    return testSummary("test_ringdrop_measure");
}
