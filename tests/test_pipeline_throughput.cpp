// HOW FAST THE DSP THREAD CAN GO, measured through the real Pipeline (GitHub issue 6).
//
// THE FIELD REPORT. A tester's RSPduo at 4 MS/s crackles constantly in 0.99.90. His
// log has the SDRplay service clean through every steady minute (no service gaps,
// no link overflows) while the pipeline's OWN ring - between its source thread and
// its DSP thread - drops 0.66 to 0.88 % of every minute's samples. The ring drops
// only when the DSP thread does not take samples out as fast as the source puts
// them in, so the question is how much of a core the DSP chain costs per input
// sample, and how that cost grows with the rate.
//
// WHAT THIS DOES. A self-paced source that never blocks: every read() hands the
// pipeline the next 1008 samples of a precomputed broadcast-FM composite at once
// (1008 is the SDRplay service's block). The source thread therefore keeps the ring
// full, the DSP thread runs flat out, and what it took out of the ring per second
// IS its ceiling at that rate (samples read minus samples dropped, over the wall
// clock). The ratio of that ceiling to the rate is the headroom: below 1.0 the
// chain cannot keep up on this machine and a real radio at that rate would drop
// exactly the deficit.
//
// The numbers are printed for every rate and mode so a slower machine's figure can
// be read against this one's. The one CHECK: the WFM chain at 4 MS/s keeps up at
// all on the build machine - a ceiling under the rate is the tester's defect
// reproduced here, which is what this file exists to show.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

#include "core/pipeline.hpp"
#include "dsp/demod.hpp"
#include "source/iq_source.hpp"
#include "test_check.hpp"

namespace {

constexpr double kTwoPi = 6.283185307179586476925286766559;
constexpr std::size_t kServiceBlock = 1008;   // one SDRplay callback's worth

// A broadcast-FM composite, like the one the tester was listening to: a 1 kHz tone
// on L+R, a 19 kHz pilot at 9 %, a tone at 57 kHz where RDS sits, 50 kHz peak
// deviation. Precomputed once per rate; the source copies it out cyclically, so
// read() costs a memcpy and nothing else - the source thread must never be the
// bottleneck this file is measuring.
std::vector<std::complex<float>> fmComposite(double rateHz, std::size_t n) {
    std::vector<std::complex<float>> out(n);
    double phase = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / rateHz;
        const double mpx = 0.45 * std::sin(kTwoPi * 1000.0 * t) +
                           0.09 * std::sin(kTwoPi * 19000.0 * t) +
                           0.05 * std::sin(kTwoPi * 57000.0 * t);
        phase += kTwoPi * 50000.0 * mpx / rateHz;
        out[i] = std::complex<float>(static_cast<float>(0.5 * std::cos(phase)),
                                     static_cast<float>(0.5 * std::sin(phase)));
    }
    return out;
}

class FloodSource final : public cascade::source::IqSource {
public:
    explicit FloodSource(double rateHz)
        : rateHz_(rateHz), signal_(fmComposite(rateHz, 1u << 20)) {}
    bool start() override { return true; }
    void stop() override {}
    bool running() const override { return true; }
    bool selfPaced() const override { return true; }
    double sampleRateHz() const override { return rateHz_; }
    bool setSampleRateHz(double) override { return false; }
    double centerFrequencyHz() const override { return 100.0e6; }
    bool setCenterFrequencyHz(double) override { return true; }
    const char* name() const override { return "Flooding FM test source"; }
    const char* lastError() const override { return ""; }

    std::size_t read(std::complex<float>* dst, std::size_t n) override {
        const std::size_t want = n < kServiceBlock ? n : kServiceBlock;
        for (std::size_t i = 0; i < want; ++i) {
            dst[i] = signal_[pos_];
            if (++pos_ == signal_.size()) { pos_ = 0; }
        }
        return want;
    }

private:
    const double rateHz_;
    std::vector<std::complex<float>> signal_;
    std::size_t pos_ = 0;
};

cascade::core::Pipeline::Config appConfig() {
    cascade::core::Pipeline::Config cfg;   // what AppWindow builds
    cfg.sampleRateHz = 2000000.0;
    cfg.fftSize = 1024;
    cfg.averagingAlpha = 0.5f;
    cfg.audioEnabled = false;
    return cfg;
}

struct Ceiling {
    double consumedPerSec = 0.0;   // samples the DSP thread took out of the ring
    double readPerSec = 0.0;       // samples the source offered
    double ratio = 0.0;            // consumed / rate: < 1 means it cannot keep up
};

Ceiling measure(double rateHz, cascade::dsp::DemodMode mode, bool stereo, int seconds) {
    cascade::core::Pipeline p(appConfig());
    p.setDemodMode(mode);
    p.setStereoEnabled(stereo);
    CHECK(p.setInputRateHz(rateHz));
    p.setSource(std::make_unique<FloodSource>(rateHz));
    p.start();
    // A second of warm-up: ring fill, filter histories, the RDS and pilot locks.
    std::this_thread::sleep_for(std::chrono::seconds(1));
    const auto t0 = std::chrono::steady_clock::now();
    const std::uint64_t read0 = p.sourceSamplesRead();
    const std::uint64_t drop0 = p.ringDroppedSamples();
    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    const std::uint64_t read1 = p.sourceSamplesRead();
    const std::uint64_t drop1 = p.ringDroppedSamples();
    const auto t1 = std::chrono::steady_clock::now();
    p.stop();
    CHECK(!p.faulted());
    const double dt = std::chrono::duration<double>(t1 - t0).count();
    Ceiling c;
    c.readPerSec = static_cast<double>(read1 - read0) / dt;
    c.consumedPerSec = static_cast<double>((read1 - read0) - (drop1 - drop0)) / dt;
    c.ratio = c.consumedPerSec / rateHz;
    return c;
}

// THE SERVICE'S BURSTS, as the tester's health line describes them: 825 read
// time-outs in a steady 4 MS/s minute, each a 20 ms wait on an empty link ring,
// and a longest callback gap of 43 ms. So the SDRplay service hands its samples
// over in runs with 20 to 43 ms of silence between them, 13 to 14 times a second.
// This source does the same: every `periodMs` it falls silent for `silenceMs`,
// then delivers what accrued in the silence and streams on live until the next
// one. read() behaves exactly like SdrPlaySource::read - whatever is available,
// one service block at a time; nothing available, a wait of at most 20 ms that
// returns 0 when it runs out (and the pipeline's own back-off follows, as it does
// for the real radio). The wait spins on the clock rather than sleeping, because
// the real read is woken by the service's callback the instant samples arrive and
// this bench must not add a timer's latency the real path has not.
class BurstSource final : public cascade::source::IqSource {
public:
    BurstSource(double rateHz, int periodMs, int silenceMs)
        : rateHz_(rateHz), period_(std::chrono::milliseconds(periodMs)),
          silence_(std::chrono::milliseconds(silenceMs)), signal_(fmComposite(rateHz, 1u << 20)) {}
    bool start() override {
        stop_.store(false);
        origin_ = std::chrono::steady_clock::now();
        delivered_ = 0;
        return true;
    }
    void stop() override { stop_.store(true); }
    bool running() const override { return true; }
    bool selfPaced() const override { return true; }
    double sampleRateHz() const override { return rateHz_; }
    bool setSampleRateHz(double) override { return false; }
    double centerFrequencyHz() const override { return 100.0e6; }
    bool setCenterFrequencyHz(double) override { return true; }
    const char* name() const override { return "Bursting FM test source"; }
    const char* lastError() const override { return ""; }

    std::size_t read(std::complex<float>* dst, std::size_t n) override {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(20);
        std::size_t avail = available(std::chrono::steady_clock::now());
        while (avail == 0) {
            if (stop_.load() || std::chrono::steady_clock::now() >= deadline) { return 0; }
            std::this_thread::yield();
            avail = available(std::chrono::steady_clock::now());
        }
        const std::size_t want = std::min({n, kServiceBlock, avail});
        for (std::size_t i = 0; i < want; ++i) {
            dst[i] = signal_[pos_];
            if (++pos_ == signal_.size()) { pos_ = 0; }
        }
        delivered_ += want;
        return want;
    }

private:
    // Samples released by `now` and not yet handed over. Inside a silence the
    // release point stays at the silence's start; outside one it is `now`.
    std::size_t available(std::chrono::steady_clock::time_point now) const {
        const auto since = now - origin_;
        const auto intoPeriod = since % period_;
        const auto releasedUpTo = intoPeriod < silence_ ? since - intoPeriod : since;
        const double secs = std::chrono::duration<double>(releasedUpTo).count();
        const std::size_t released = static_cast<std::size_t>(rateHz_ * secs);
        return released > delivered_ ? released - delivered_ : 0;
    }

    const double rateHz_;
    const std::chrono::steady_clock::duration period_;
    const std::chrono::steady_clock::duration silence_;
    std::vector<std::complex<float>> signal_;
    std::size_t pos_ = 0;
    std::size_t delivered_ = 0;
    std::chrono::steady_clock::time_point origin_;
    std::atomic<bool> stop_{false};
};

// What share of a bursting source's samples the pipeline drops, over `seconds`.
struct BurstLoss {
    std::size_t ringCapacity = 0;
    double droppedShare = 0.0;   // dropped / read
    double readPerSec = 0.0;
};

BurstLoss measureBursts(double rateHz, int periodMs, int silenceMs, int seconds) {
    cascade::core::Pipeline p(appConfig());
    p.setDemodMode(cascade::dsp::DemodMode::WFM);
    p.setStereoEnabled(true);
    CHECK(p.setInputRateHz(rateHz));
    p.setSource(std::make_unique<BurstSource>(rateHz, periodMs, silenceMs));
    p.start();
    std::this_thread::sleep_for(std::chrono::seconds(1));
    const auto t0 = std::chrono::steady_clock::now();
    const std::uint64_t read0 = p.sourceSamplesRead();
    const std::uint64_t drop0 = p.ringDroppedSamples();
    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    const std::uint64_t read1 = p.sourceSamplesRead();
    const std::uint64_t drop1 = p.ringDroppedSamples();
    const auto t1 = std::chrono::steady_clock::now();
    BurstLoss b;
    b.ringCapacity = p.ringCapacity();
    p.stop();
    CHECK(!p.faulted());
    const double dt = std::chrono::duration<double>(t1 - t0).count();
    b.readPerSec = static_cast<double>(read1 - read0) / dt;
    b.droppedShare = (read1 > read0)
        ? static_cast<double>(drop1 - drop0) / static_cast<double>(read1 - read0) : 0.0;
    return b;
}

void report(const char* label, double rateHz, const Ceiling& c) {
    std::printf("%-28s %5.1f MS/s: DSP ceiling %7.2f MS/s (%.2fx the rate), source offered %7.2f MS/s\n",
                label, rateHz / 1e6, c.consumedPerSec / 1e6, c.ratio, c.readPerSec / 1e6);
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    using cascade::dsp::DemodMode;

    // The granularity this machine gives a 1 ms sleep - the DSP thread's idle wait
    // and the source thread's back-off are both sleep_for(1 ms), and on Windows
    // without a timer-resolution request that is 15.6 ms, not 1.
    {
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 50; ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::printf("50 x sleep_for(1 ms) took %.1f ms (%.2f ms each)\n", ms, ms / 50.0);
    }

    // The tester's case first: WFM, stereo on, 4 MS/s.
    const Ceiling tester = measure(4.0e6, DemodMode::WFM, true, 4);
    report("WFM stereo (tester's case)", 4.0e6, tester);
    CHECK(tester.ratio >= 1.0);

    // The shape of the cost against the rate, same chain.
    for (const double rate : {2.0e6, 3.0e6, 6.0e6, 8.0e6}) {
        report("WFM stereo", rate, measure(rate, DemodMode::WFM, true, 3));
    }

    // Where the cost sits: the stereo decoder, the mode, the channel chain at all.
    report("WFM mono", 4.0e6, measure(4.0e6, DemodMode::WFM, false, 3));
    for (const DemodMode m : {DemodMode::NFM, DemodMode::AM, DemodMode::RAW}) {
        report(cascade::dsp::modeName(m), 4.0e6, measure(4.0e6, m, false, 3));
    }

    // THE BURST TOLERANCE: the tester's service pattern (a silence 13 to 14 times a
    // second, so a 72 ms period) with the silence stepped up to and past the 43 ms
    // he saw, at his three rates. A ring that rides out the 43 ms silences the
    // first minute of his log measured, with the pipeline's own timer sleeps on
    // top, drops nothing; the share that is dropped at each step is printed.
    for (const double rate : {2.0e6, 3.0e6, 4.0e6}) {
        for (const int silence : {20, 45, 60}) {
            const BurstLoss b = measureBursts(rate, 72, silence, 3);
            std::printf("silence %2d ms of every 72 at %.0f MS/s: ring %7zu samples (%5.1f ms), "
                        "offered %5.2f MS/s, dropped %6.3f %%\n",
                        silence, rate / 1e6, b.ringCapacity,
                        1000.0 * static_cast<double>(b.ringCapacity) / rate,
                        b.readPerSec / 1e6, 100.0 * b.droppedShare);
            // The regression: the tester's own silences (43 ms, rounded up) at the
            // tester's own rate must cost nothing.
            if (rate == 4.0e6 && silence == 45) { CHECK(b.droppedShare == 0.0); }
        }
    }

    return testSummary("test_pipeline_throughput");
}
