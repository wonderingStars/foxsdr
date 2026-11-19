// The pipeline's SPSC ring must hold what ONE source read hands it, at the
// SOURCE's rate - not at the rate the pipeline happened to be constructed at.
//
// THE DEFECT. The ring was sized once, in the constructor, for
// cfg.sampleRateHz (the app always builds it at 2 MS/s: 131072 samples then,
// 524288 since the ring became a quarter of a second deep for GitHub issue 6), while
// the source thread sizes each read at 10 ms of the SOURCE's own rate. A
// 20 MS/s source (a HackRF menu rate, or simply a 20 MS/s recording) hands the
// ring 200000 samples per read; SpscRing::write accepts only what fits, so
// 68928 samples - a third of every chunk - were dropped on every read, with
// the DSP thread idle and nothing counting the loss.
//
// THE FIX, AND ITS FLOOR. The ring is re-sized for the incoming source at each
// source-thread spawn (start(), and a live setSource()), and a new drop counter
// makes any overflow visible. It is never made SMALLER than the construction
// size: [R4] pins that, because the first version of the fix shrank the ring
// for slow sources and cost test_source_swap samples on a loaded machine.
//
// HOW THIS IS MADE DETERMINISTIC. The wide source below is free-running (the
// pipeline paces it, like the file source) and delivers exactly ONE chunk:
// its second read() parks until stop() releases it. So the only write the
// ring ever sees from it is one 200000-sample chunk into a ring that nothing
// else is filling, and "how many samples did that write drop" has one right
// answer - 0 - whatever the scheduler does. The second read() starting is the
// signal that the first chunk's write has already happened.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <complex>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <thread>

#include "core/pipeline.hpp"
#include "source/iq_source.hpp"
#include "test_check.hpp"

namespace {

constexpr double kWideRateHz = 20.0e6;   // 200000 samples per 10 ms read
constexpr std::size_t kWideChunk = 200000;

// Free-running, one chunk, then parked until stop(). stop() is the IqSource
// contract's abort, and the pipeline calls it before joining the thread.
class OneChunkWideSource final : public cascade::source::IqSource {
public:
    explicit OneChunkWideSource(double rateHz = kWideRateHz) : rateHz_(rateHz) {}
    bool start() override {
        std::lock_guard<std::mutex> lk(m_);
        released_ = false;
        return true;
    }
    void stop() override {
        {
            std::lock_guard<std::mutex> lk(m_);
            released_ = true;
        }
        cv_.notify_all();
    }
    bool running() const override { return true; }
    bool selfPaced() const override { return false; }
    double sampleRateHz() const override { return rateHz_; }
    bool setSampleRateHz(double) override { return false; }
    double centerFrequencyHz() const override { return 100.0e6; }
    bool setCenterFrequencyHz(double) override { return true; }
    const char* name() const override { return "One-chunk 20 MS/s test source"; }
    const char* lastError() const override { return ""; }

    std::size_t read(std::complex<float>* dst, std::size_t n) override {
        const int call = reads_.fetch_add(1, std::memory_order_acq_rel);
        if (call == 0) {
            lastN_.store(n, std::memory_order_relaxed);
            for (std::size_t i = 0; i < n; ++i) { dst[i] = std::complex<float>(0.25f, -0.25f); }
            return n;
        }
        std::unique_lock<std::mutex> lk(m_);
        cv_.wait(lk, [&] { return released_; });
        return 0;
    }

    int reads() const { return reads_.load(std::memory_order_acquire); }
    std::size_t firstReadSize() const { return lastN_.load(std::memory_order_relaxed); }

private:
    const double rateHz_;
    std::mutex m_;
    std::condition_variable cv_;
    bool released_ = false;
    std::atomic<int> reads_{0};
    std::atomic<std::size_t> lastN_{0};
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

cascade::core::Pipeline::Config appConfig() {
    // What AppWindow builds: 2 MS/s, 1024-point FFT.
    cascade::core::Pipeline::Config cfg;
    cfg.sampleRateHz = 2000000.0;
    cfg.fftSize = 1024;
    cfg.averagingAlpha = 1.0f;
    cfg.audioEnabled = false;
    return cfg;
}

// A free-running source whose rate can be changed live, as a vendor radio's can
// (SDRplay, Soapy): setSampleRateHz() moves what sampleRateHz() reports. It
// parks in read() until stop(), like OneChunkWideSource, so no samples flow.
class RetunableRateSource final : public cascade::source::IqSource {
public:
    explicit RetunableRateSource(double rateHz) : rateHz_(rateHz) {}
    bool start() override {
        std::lock_guard<std::mutex> lk(m_);
        released_ = false;
        return true;
    }
    void stop() override {
        {
            std::lock_guard<std::mutex> lk(m_);
            released_ = true;
        }
        cv_.notify_all();
    }
    bool running() const override { return true; }
    bool selfPaced() const override { return false; }
    double sampleRateHz() const override { return rateHz_.load(std::memory_order_relaxed); }
    bool setSampleRateHz(double hz) override { rateHz_.store(hz, std::memory_order_relaxed); return true; }
    double centerFrequencyHz() const override { return 100.0e6; }
    bool setCenterFrequencyHz(double) override { return true; }
    const char* name() const override { return "Retunable-rate test source"; }
    const char* lastError() const override { return ""; }
    std::size_t read(std::complex<float>*, std::size_t) override {
        std::unique_lock<std::mutex> lk(m_);
        cv_.wait(lk, [&] { return released_; });
        return 0;
    }

private:
    std::atomic<double> rateHz_;
    std::mutex m_;
    std::condition_variable cv_;
    bool released_ = false;
};

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    // The construction-time ring, for reference: a quarter of a second at
    // 2 MS/s = 500000, rounded up to a power of two (ringCapacityFor).
    {
        cascade::core::Pipeline p(appConfig());
        std::printf("construction ring capacity: %zu\n", p.ringCapacity());
        CHECK(p.ringCapacity() == 524288u);
        CHECK(p.ringDroppedSamples() == 0u);
    }

    // [R1] A 20 MS/s source installed BEFORE start(): one read of 200000
    // samples must fit the ring whole.
    {
        cascade::core::Pipeline p(appConfig());
        auto src = std::make_unique<OneChunkWideSource>();
        OneChunkWideSource* raw = src.get();
        p.setSource(std::move(src));
        p.start();
        CHECK(waitFor([&] { return raw->reads() >= 2; }));
        CHECK(raw->firstReadSize() == kWideChunk);
        const std::uint64_t dropped = p.ringDroppedSamples();
        std::printf("[R1] start() at 20 MS/s: ring %zu, dropped %llu of %zu\n", p.ringCapacity(),
                    static_cast<unsigned long long>(dropped), raw->firstReadSize());
        CHECK(dropped == 0u);
        CHECK(p.ringCapacity() >= 4u * kWideChunk);
        // A quarter of a second of 20 MS/s is 5 million samples, so this is the
        // ring's ceiling (kRingMaxSamples, 64 MB) - and the ceiling still holds
        // 400 ms of it.
        CHECK(p.ringCapacity() == 8388608u);
        p.stop();
        CHECK(!p.faulted());
    }

    // [R2] THE APP'S OWN PATH: the pipeline is already running on the 2 MS/s
    // generator and the source is swapped for a 20 MS/s one while live. The
    // DSP thread keeps running across a setSource, so this is the path where
    // growing the ring has to take the DSP thread out of the way first - and
    // bring it back, which the frames check below proves.
    {
        cascade::core::Pipeline p(appConfig());
        p.start();
        cascade::core::SpectrumFrame f;
        CHECK(waitFor([&] { return p.getLatestFrame(f); }));
        const std::uint64_t droppedBefore = p.ringDroppedSamples();

        auto src = std::make_unique<OneChunkWideSource>();
        OneChunkWideSource* raw = src.get();
        p.setSource(std::move(src));
        CHECK(p.running());
        CHECK(waitFor([&] { return raw->reads() >= 2; }));
        const std::uint64_t dropped = p.ringDroppedSamples() - droppedBefore;
        std::printf("[R2] live setSource to 20 MS/s: ring %zu, dropped %llu of %zu\n",
                    p.ringCapacity(), static_cast<unsigned long long>(dropped),
                    raw->firstReadSize());
        CHECK(dropped == 0u);
        CHECK(p.ringCapacity() >= 4u * kWideChunk);

        // The one chunk is 195 FFT blocks: the DSP thread must be running
        // again to turn it into frames.
        const std::uint64_t seqAtSwap = f.seq;
        CHECK(waitFor([&] {
            p.getLatestFrame(f);
            return f.seq > seqAtSwap + 100;
        }));

        // [R3] And back to the 2 MS/s generator: the ring returns to its
        // 2 MS/s size. A ring left at the 20 MS/s size would hold over four
        // seconds of 2 MS/s samples, so a DSP thread that falls behind would
        // lag by seconds instead of a quarter of one before anything is dropped.
        p.setSource(nullptr);
        CHECK(p.running());
        std::printf("[R3] back to the 2 MS/s generator: ring %zu\n", p.ringCapacity());
        CHECK(p.ringCapacity() == 524288u);
        const std::uint64_t seqBack = f.seq;
        CHECK(waitFor([&] {
            p.getLatestFrame(f);
            return f.seq > seqBack + 5;
        }));
        p.stop();
        CHECK(!p.faulted());
    }

    // [R4] A SLOWER source never SHRINKS the ring below its construction
    // size. Sized purely from a 250 kHz source's rate the ring would be 65536
    // samples - a quarter of a second at that rate, but an eighth of the
    // 524288 every source slower than the construction rate gets, and
    // test_source_swap's 400 kHz conservation check lost samples to exactly
    // such a shrink on a loaded machine. The floor keeps every such source
    // where it was; only a source whose read would not fit gets more.
    {
        cascade::core::Pipeline p(appConfig());
        p.start();
        p.setSource(std::make_unique<OneChunkWideSource>(250000.0));
        std::printf("[R4] live setSource to 250 kHz: ring %zu\n", p.ringCapacity());
        CHECK(p.ringCapacity() == 524288u);
        p.stop();
        auto slow = std::make_unique<OneChunkWideSource>(250000.0);
        p.setSource(std::move(slow));
        p.start();
        std::printf("[R4] start() at 250 kHz: ring %zu\n", p.ringCapacity());
        CHECK(p.ringCapacity() == 524288u);
        p.stop();
        CHECK(!p.faulted());
    }

    // [R5] THE TESTER'S RATE (GitHub issue 6, the 0.99.90 report): an RSPduo at
    // 4 MS/s gets a ring of at least a quarter of a second - 1000000 samples,
    // rounded up to 1048576 - where it used to get 262144 (65 ms), which the
    // SDRplay service's 43 ms silences plus two 15.6 ms timer sleeps overran.
    {
        cascade::core::Pipeline p(appConfig());
        p.setSource(std::make_unique<OneChunkWideSource>(4000000.0));
        p.start();
        std::printf("[R5] start() at 4 MS/s: ring %zu\n", p.ringCapacity());
        CHECK(p.ringCapacity() >= 1000000u);
        CHECK(p.ringCapacity() == 1048576u);
        p.stop();
        CHECK(!p.faulted());
    }

    // [R6] A LIVE RATE CHANGE GIVES THE DEPTH A START AT THAT RATE GETS (field
    // report 5A76C1C8, 0.99.109: an RSPduo opened at 2 MS/s and moved to 10 and
    // then 6 MS/s dropped 189472 samples, the ring still 524288 - 87 ms at 6
    // MS/s - because only start() and setSource() sized it). The source's rate
    // moves first, then the pipeline's, the order the application uses.
    {
        auto depthOfFreshStart = [](double rateHz) {
            cascade::core::Pipeline f(appConfig());
            f.setSource(std::make_unique<RetunableRateSource>(rateHz));
            f.start();
            const std::size_t depth = f.ringCapacity();
            f.stop();
            return depth;
        };
        const std::size_t at6 = depthOfFreshStart(6.0e6);
        const std::size_t at2 = depthOfFreshStart(2.0e6);
        CHECK(at6 == 2097152u);
        CHECK(at2 == 524288u);

        cascade::core::Pipeline p(appConfig());
        auto src = std::make_unique<RetunableRateSource>(2.0e6);
        RetunableRateSource* raw = src.get();
        p.setSource(std::move(src));
        p.start();
        CHECK(p.ringCapacity() == at2);
        cascade::core::SpectrumFrame f;

        CHECK(raw->setSampleRateHz(6.0e6));
        CHECK(p.setInputRateHz(6.0e6));
        std::printf("[R6] live 2 -> 6 MS/s: ring %zu (a fresh start gets %zu)\n", p.ringCapacity(), at6);
        CHECK(p.ringCapacity() == at6);
        CHECK(p.running());

        // And back down: the depth returns to what a start at 2 MS/s gets.
        CHECK(raw->setSampleRateHz(2.0e6));
        CHECK(p.setInputRateHz(2.0e6));
        std::printf("[R6] live 6 -> 2 MS/s: ring %zu (a fresh start gets %zu)\n", p.ringCapacity(), at2);
        CHECK(p.ringCapacity() == at2);

        // 2 -> 10 -> 6, the sequence in the report.
        CHECK(raw->setSampleRateHz(10.0e6));
        CHECK(p.setInputRateHz(10.0e6));
        CHECK(p.ringCapacity() == depthOfFreshStart(10.0e6));
        CHECK(raw->setSampleRateHz(6.0e6));
        CHECK(p.setInputRateHz(6.0e6));
        CHECK(p.ringCapacity() == at6);
        CHECK(p.running());
        p.stop();
        CHECK(!p.faulted());

        // The reverse, from a start at 6 MS/s: down to what a start at 2 gets.
        cascade::core::Pipeline q(appConfig());
        auto srcQ = std::make_unique<RetunableRateSource>(6.0e6);
        RetunableRateSource* rawQ = srcQ.get();
        CHECK(q.setInputRateHz(6.0e6));   // the chain follows the radio, as the application does
        q.setSource(std::move(srcQ));
        q.start();
        CHECK(q.ringCapacity() == at6);
        CHECK(rawQ->setSampleRateHz(2.0e6));
        CHECK(q.setInputRateHz(2.0e6));
        std::printf("[R6] start at 6, live to 2 MS/s: ring %zu (a fresh start gets %zu)\n", q.ringCapacity(), at2);
        CHECK(q.ringCapacity() == at2);
        CHECK(q.running());
        q.stop();
        CHECK(!q.faulted());
    }

    return testSummary("test_pipeline_ring_rate");
}
