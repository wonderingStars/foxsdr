// A source that throws out of read() must not kill the process.
//
// This is the unplug-the-SDR crash in deterministic form: vendor drivers
// throw from inside a stream read when the USB device disappears, and an
// exception escaping a std::thread is an immediate std::terminate (Windows
// reports 0xC0000409 fail-fast in ucrtbase). Before the catch-all wrappers in
// Pipeline::sourceThreadMain/dspThreadMain, running this file aborted the
// whole test executable rather than failing an assertion.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <complex>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <thread>

#include "core/pipeline.hpp"
#include "source/iq_source.hpp"
#include "test_check.hpp"

namespace {

// Free-running source that delivers a few blocks and then throws, mimicking a
// device that vanishes mid-capture.
class ExplodingSource : public cascade::source::IqSource {
public:
    explicit ExplodingSource(int blocksBeforeThrow) : budget_(blocksBeforeThrow) {}

    bool start() override { running_ = true; return true; }
    void stop() override { running_ = false; }
    bool running() const override { return running_; }
    bool selfPaced() const override { return false; }
    double sampleRateHz() const override { return 2000000.0; }
    bool setSampleRateHz(double) override { return false; }
    double centerFrequencyHz() const override { return centerHz_; }
    bool setCenterFrequencyHz(double hz) override { centerHz_ = hz; return true; }
    const char* name() const override { return "Exploding test source"; }
    const char* lastError() const override { return ""; }

    std::size_t read(std::complex<float>* dst, std::size_t n) override {
        if (budget_.fetch_sub(1, std::memory_order_relaxed) <= 0) {
            threw_.store(true, std::memory_order_relaxed);
            throw std::runtime_error("device disconnected");
        }
        for (std::size_t i = 0; i < n; ++i) { dst[i] = std::complex<float>(0.0f, 0.0f); }
        return n;
    }

    bool threw() const { return threw_.load(std::memory_order_relaxed); }

private:
    std::atomic<int> budget_;
    std::atomic<bool> threw_{false};
    bool running_ = false;
    double centerHz_ = 100000000.0;
};

// Waits until pred() or the deadline; returns pred()'s final value so a
// hang becomes a failed CHECK instead of a stuck suite.
template <typename Pred>
bool waitFor(Pred pred, int timeoutMs = 15000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return pred();
}

cascade::core::Pipeline::Config testConfig() {
    cascade::core::Pipeline::Config cfg;
    cfg.sampleRateHz = 2000000.0;
    cfg.fftSize = 1024;
    cfg.averagingAlpha = 1.0f;
    cfg.audioEnabled = false;  // no device needed in CI
    return cfg;
}

}  // namespace

int main() {
    // --- The crash itself: a throwing read must be contained -----------------
    {
        cascade::core::Pipeline pipeline(testConfig());
        auto src = std::make_unique<ExplodingSource>(3);
        ExplodingSource* raw = src.get();
        pipeline.setSource(std::move(src));
        pipeline.start();

        // Reaching this line at all is the headline result: pre-fix, the
        // escaping exception terminated the process here.
        CHECK(waitFor([&] { return pipeline.faulted(); }));
        CHECK(raw->threw());
        CHECK(pipeline.faulted());

        // The fault takes the pipeline down rather than leaving it half alive.
        CHECK(waitFor([&] { return !pipeline.running(); }));
        CHECK(!pipeline.running());

        const std::string msg = pipeline.faultMessage();
        CHECK(!msg.empty());
        CHECK(msg.find("device disconnected") != std::string::npos);
        CHECK(msg.find("source thread") != std::string::npos);

        pipeline.stop();  // must be clean after a fault, not a hang or a throw
        CHECK(!pipeline.running());
    }

    // --- Recovery: a healthy source after a fault clears it ------------------
    {
        cascade::core::Pipeline pipeline(testConfig());
        pipeline.setSource(std::make_unique<ExplodingSource>(2));
        pipeline.start();
        CHECK(waitFor([&] { return pipeline.faulted(); }));

        // Swapping back to the built-in generator and restarting must recover
        // fully: fault cleared, frames flowing again.
        pipeline.setSource(nullptr);
        pipeline.start();
        CHECK(!pipeline.faulted());
        CHECK(pipeline.faultMessage().empty());
        CHECK(pipeline.running());

        cascade::core::SpectrumFrame frame;
        CHECK(waitFor([&] { return pipeline.getLatestFrame(frame) || frame.seq > 0; }));
        CHECK(frame.seq > 0);
        pipeline.stop();
    }

    // --- Destruction while faulted must not hang or terminate ----------------
    {
        cascade::core::Pipeline pipeline(testConfig());
        pipeline.setSource(std::make_unique<ExplodingSource>(1));
        pipeline.start();
        CHECK(waitFor([&] { return pipeline.faulted(); }));
        // ~Pipeline runs here with a faulted, already-exited source thread.
    }
    CHECK(true);  // reaching this line means the destructor path survived

    return testSummary("test_thread_fault");
}
