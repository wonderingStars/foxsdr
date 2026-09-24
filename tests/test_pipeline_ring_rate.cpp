// The pipeline's SPSC ring must hold what ONE source read hands it, at the
// SOURCE's rate - not at the rate the pipeline happened to be constructed at.
//
// THE DEFECT. The ring was sized once, in the constructor, for
// cfg.sampleRateHz (the app always builds it at 2 MS/s: 131072 samples), while
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

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    // The construction-time ring, for reference: 4 x 10 ms at 2 MS/s = 80000,
    // rounded up to a power of two.
    {
        cascade::core::Pipeline p(appConfig());
        std::printf("construction ring capacity: %zu\n", p.ringCapacity());
        CHECK(p.ringCapacity() == 131072u);
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
        // 2 MS/s size. A ring left at the 20 MS/s size would hold 0.5 s of
        // 2 MS/s samples, so a DSP thread that falls behind would lag by
        // half a second instead of 65 ms before anything is dropped.
        p.setSource(nullptr);
        CHECK(p.running());
        std::printf("[R3] back to the 2 MS/s generator: ring %zu\n", p.ringCapacity());
        CHECK(p.ringCapacity() == 131072u);
        const std::uint64_t seqBack = f.seq;
        CHECK(waitFor([&] {
            p.getLatestFrame(f);
            return f.seq > seqBack + 5;
        }));
        p.stop();
        CHECK(!p.faulted());
    }

    // [R4] A SLOWER source never SHRINKS the ring below its construction
    // size. Sized purely from a 250 kHz source's rate the ring would be 16384
    // samples - 65 ms at that rate, but far less slack than the 131072 every
    // source slower than the construction rate has always had, and
    // test_source_swap's 400 kHz conservation check lost samples to exactly
    // that shrink on a loaded machine. The floor keeps every such source
    // where it was; only a source whose read would not fit gets more.
    {
        cascade::core::Pipeline p(appConfig());
        p.start();
        p.setSource(std::make_unique<OneChunkWideSource>(250000.0));
        std::printf("[R4] live setSource to 250 kHz: ring %zu\n", p.ringCapacity());
        CHECK(p.ringCapacity() == 131072u);
        p.stop();
        auto slow = std::make_unique<OneChunkWideSource>(250000.0);
        p.setSource(std::move(slow));
        p.start();
        std::printf("[R4] start() at 250 kHz: ring %zu\n", p.ringCapacity());
        CHECK(p.ringCapacity() == 131072u);
        p.stop();
        CHECK(!p.faulted());
    }

    return testSummary("test_pipeline_ring_rate");
}
