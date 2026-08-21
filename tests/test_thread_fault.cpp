// A source that throws out of read() must not kill the process.
//
// This is the unplug-the-SDR crash in deterministic form: vendor drivers
// throw from inside a stream read when the USB device disappears, and an
// exception escaping a std::thread is an immediate std::terminate (Windows
// reports 0xC0000409 fail-fast in ucrtbase). Before the catch-all wrappers in
// Pipeline::sourceThreadMain/dspThreadMain, running this file aborted the
// whole test executable rather than failing an assertion.
//
// The last two blocks cover the OTHER half of the same failure, the one the
// catch-all cannot see. A source that catches the driver exception itself
// (SoapySource does, so its own teardown stays sane) reaches the pipeline as
// a plain 0-return, which the self-paced loop is required to retry — so
// device loss became a spectrum that silently froze while the source thread
// span on a dead handle. Those two properties, the fault reaching
// Pipeline::faulted() and the retry loop not spinning, are asserted with
// self-paced fakes here because a real unplug cannot be staged in a unit
// test.
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

// Self-paced source that goes bad WITHOUT throwing: after a few reads it
// stops delivering and raises its fault flag, exactly the shape SoapySource
// has once it has caught a driver exception out of readStream. The pipeline
// can only learn about this by asking, which is what IqSource::faulted() is
// for.
class FaultingSource : public cascade::source::IqSource {
public:
    explicit FaultingSource(int readsBeforeFault) : budget_(readsBeforeFault) {}

    bool start() override { running_ = true; return true; }
    void stop() override { running_ = false; }
    bool running() const override { return running_; }
    bool selfPaced() const override { return true; }
    double sampleRateHz() const override { return 1000000.0; }
    bool setSampleRateHz(double) override { return false; }
    double centerFrequencyHz() const override { return 0.0; }
    bool setCenterFrequencyHz(double) override { return true; }
    const char* name() const override { return "Faulting test source"; }
    // A literal, so this is safe to call from the source thread (the fault
    // path does) as well as from the test thread.
    const char* lastError() const override {
        return "readStream failed: device removed";
    }
    bool faulted() const override { return faulted_.load(std::memory_order_relaxed); }

    std::size_t read(std::complex<float>* dst, std::size_t n) override {
        reads_.fetch_add(1, std::memory_order_relaxed);
        if (budget_.fetch_sub(1, std::memory_order_relaxed) <= 0) {
            faulted_.store(true, std::memory_order_relaxed);
            return 0;  // the "retry" signal a dead device is indistinguishable from
        }
        for (std::size_t i = 0; i < n; ++i) { dst[i] = std::complex<float>(0.0f, 0.0f); }
        return n;
    }

    int reads() const { return reads_.load(std::memory_order_relaxed); }

private:
    std::atomic<int> budget_;
    std::atomic<int> reads_{0};
    std::atomic<bool> faulted_{false};
    bool running_ = false;
};

// Self-paced source that is merely IDLE: every read returns the contract's
// "nothing yet, retry" 0 immediately, with no bounded block of its own and no
// fault. Real hardware blocks for its timeout first, but a dead handle (and
// several drivers' fail-fast paths) return instantly — and the loop's
// documented "retry immediately" then consumed an entire core.
class IdleSource : public cascade::source::IqSource {
public:
    bool start() override { return true; }
    void stop() override {}
    bool running() const override { return true; }
    bool selfPaced() const override { return true; }
    double sampleRateHz() const override { return 1000000.0; }
    bool setSampleRateHz(double) override { return false; }
    double centerFrequencyHz() const override { return 0.0; }
    bool setCenterFrequencyHz(double) override { return true; }
    const char* name() const override { return "Idle test source"; }
    const char* lastError() const override { return ""; }

    std::size_t read(std::complex<float>*, std::size_t) override {
        reads_.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }

    long long reads() const { return reads_.load(std::memory_order_relaxed); }

private:
    std::atomic<long long> reads_{0};
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

    // --- A non-throwing device loss must still reach faulted() --------------
    // The exception never escapes read() here, so the catch-all wrappers see
    // nothing; the ONLY way the pipeline can learn the radio is gone is by
    // asking the source. Pre-fix this block hung at the first wait: the loop
    // retried the 0-return forever and faulted() stayed false.
    {
        cascade::core::Pipeline pipeline(testConfig());
        auto src = std::make_unique<FaultingSource>(3);
        FaultingSource* raw = src.get();
        pipeline.setSource(std::move(src));
        pipeline.start();

        CHECK(waitFor([&] { return pipeline.faulted(); }));
        CHECK(pipeline.faulted());

        // The message must identify both where it happened and what the
        // source said — "Device stopped: <reason>" in the GUI is built from
        // exactly this string.
        const std::string msg = pipeline.faultMessage();
        CHECK(msg.find("source thread") != std::string::npos);
        CHECK(msg.find("device removed") != std::string::npos);

        // And the pipeline comes down rather than idling on a dead device.
        CHECK(waitFor([&] { return !pipeline.running(); }));

        // The source loop must have LEFT, not merely reported: a fault that
        // stops the pipeline but keeps reading a dead handle is the same
        // wasted core in a different disguise.
        const int atFault = raw->reads();
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        CHECK(raw->reads() == atFault);

        pipeline.stop();
        CHECK(!pipeline.running());
    }

    // --- An idle self-paced source must be backed off, not spun on ----------
    // The source below returns 0 instantly and forever. A loop that retries
    // with no pause reads it as fast as the CPU allows (millions of calls in
    // the window); the backed-off loop manages a few hundred at most. The
    // threshold sits far above the sleeping rate and far below the spinning
    // one, so neither a loaded machine nor a coarse Windows timer can flip it.
    {
        cascade::core::Pipeline pipeline(testConfig());
        auto src = std::make_unique<IdleSource>();
        IdleSource* raw = src.get();
        pipeline.setSource(std::move(src));
        pipeline.start();

        CHECK(waitFor([&] { return raw->reads() > 0; }));  // loop is running
        const long long before = raw->reads();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        const long long during = raw->reads() - before;
        std::printf("idle self-paced reads in 200 ms: %lld\n", during);
        CHECK(during > 0);      // the loop kept retrying (0 is not end-of-stream)
        CHECK(during < 5000);   // ...but paced, not spinning

        pipeline.stop();
        CHECK(!pipeline.running());
        CHECK(!pipeline.faulted());  // idle is not a fault
    }

    return testSummary("test_thread_fault");
}
