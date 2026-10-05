// A source that reports MORE samples than it was asked for must not be believed.
//
// IqSource::read(dst, n) returns how many of the n samples it wrote, and every
// consumer in this program - the pipeline's source thread (both its paced and
// its free-running loop) and each patch-page radio's reader - used the returned
// count to size a copy out of its own n-sample buffer. A driver that returns
// more than n (a vendor module that returns its internal block size, an off-by-
// one in a wrapper, a stream that reports bytes where samples were meant) turned
// that into a read past the end of the buffer: harmless-looking garbage on one
// run, a crash on the next, and exactly the kind of fault that no test noticed
// because every source the suite uses tells the truth.
//
// THE SOURCE HERE LIES, and tells the truth about nothing else: it writes
// exactly the n samples it was asked for and returns n plus a generous excess,
// so the only way a consumer can touch more than n samples is by trusting the
// return value. Under AddressSanitizer an unclamped consumer is a
// heap-buffer-overflow; in a plain build the over-read lands on neighbouring heap
// memory and is silent, which is why this test is a sanitizer test first. What it
// can assert everywhere is that the consumers keep running through such a source
// and report no fault.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <complex>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>

#include "core/patch_radio.hpp"
#include "core/pipeline.hpp"
#include "source/iq_source.hpp"
#include "test_check.hpp"

namespace {

constexpr double kRate = 48000.0;
constexpr std::size_t kExcess = 20000;  // samples beyond what was asked; far past any red zone

class OverreportingSource final : public cascade::source::IqSource {
public:
    explicit OverreportingSource(bool paced) : paced_(paced) {}

    bool start() override {
        running_.store(true);
        return true;
    }
    void stop() override { running_.store(false); }
    bool running() const override { return running_.load(); }
    bool selfPaced() const override { return paced_; }
    double sampleRateHz() const override { return kRate; }
    bool setSampleRateHz(double) override { return false; }
    double centerFrequencyHz() const override { return 0.0; }
    bool setCenterFrequencyHz(double) override { return true; }

    std::size_t read(std::complex<float>* dst, std::size_t n) override {
        for (std::size_t i = 0; i < n; ++i) {
            dst[i] = {static_cast<float>(i % 7) * 0.01f, 0.0f};
        }
        if (n > maxAsked_.load()) { maxAsked_.store(n); }
        reads_.fetch_add(1);
        if (paced_) {
            // A device's read blocks for its samples; the free-running kind is
            // paced by the consumer.
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return n + kExcess;  // the lie
    }

    const char* name() const override { return "overreporting"; }
    const char* lastError() const override { return ""; }

    int reads() const { return reads_.load(); }
    std::size_t maxAsked() const { return maxAsked_.load(); }

private:
    const bool paced_;
    std::atomic<bool> running_{false};
    std::atomic<int> reads_{0};
    std::atomic<std::size_t> maxAsked_{0};
};

template <class Pred>
bool waitFor(Pred pred, int ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return pred();
}

void pipelineDoesNotTrustTheReturnValue(bool paced) {
    cascade::core::Pipeline::Config cfg;
    cfg.sampleRateHz = kRate;
    cfg.fftSize = 1024;
    cfg.audioEnabled = false;
    cascade::core::Pipeline pipeline(cfg);

    auto owned = std::make_unique<OverreportingSource>(paced);
    OverreportingSource* src = owned.get();
    pipeline.setSource(std::move(owned));
    pipeline.start();
    CHECK(waitFor([&] { return src->reads() >= 8; }, 10000));
    pipeline.stop();

    std::printf("  pipeline, %s source: %d reads of at most %zu samples, faulted=%d\n",
                paced ? "paced" : "free-running", src->reads(), src->maxAsked(),
                pipeline.faulted() ? 1 : 0);
    CHECK(src->reads() >= 8);
    CHECK(!pipeline.faulted());
}

void patchRadioDoesNotTrustTheReturnValue(bool paced) {
    auto owned = std::make_unique<OverreportingSource>(paced);
    OverreportingSource* src = owned.get();
    cascade::core::patch::PatchRadio radio(1, std::move(owned), "overreporting");
    std::string why;
    CHECK(radio.start(why));
    CHECK(waitFor([&] { return src->reads() >= 8; }, 10000));
    radio.stop();

    std::printf("  patch radio, %s source: %d reads of at most %zu samples, fault=\"%s\"\n",
                paced ? "paced" : "free-running", src->reads(), src->maxAsked(),
                radio.fault().c_str());
    CHECK(src->reads() >= 8);
    CHECK(radio.fault().empty());
}

}  // namespace

int main() {
    pipelineDoesNotTrustTheReturnValue(true);
    pipelineDoesNotTrustTheReturnValue(false);
    patchRadioDoesNotTrustTheReturnValue(true);
    patchRadioDoesNotTrustTheReturnValue(false);
    return testSummary("test_source_overreport");
}
