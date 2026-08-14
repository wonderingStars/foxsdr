// Tests for Pipeline::setSource — hot-swapping IqSource implementations
// while the pipeline runs — plus the self-paced source path.
//
// The fakes are in-test IqSource implementations wired to a test-owned
// FakeState, so the test observes from outside exactly what the pipeline did
// to them: samples delivered, zero-return retries, and — decisively —
// whether a fake was ever destroyed while its read() was still in flight
// (tornDestruction) or had read() entered after destruction
// (readAfterDestroy). Those two flags are how the test catches a setSource
// that skips the quiesce handshake and swaps the pointer live: the source
// thread parks inside the slow fake's read() ~100% of the time, so a live
// swap destroys the fake mid-read and the destructor records the overlap
// into FakeState BEFORE the memory is freed (the flags outlive the fake
// because the TEST owns them). The seq_cst flag protocol makes the detection
// airtight: read entry increments inRead then checks destroyed; the
// destructor sets destroyed then checks inRead — in the seq_cst total order
// one of the two checks must observe the other thread's write whenever a
// read overlaps or follows destruction, so at least one flag always trips.
//
// Every blocking fake respects an abort flag set by its own stop(). This is
// not a test convenience — it is the IqSource contract's shutdown path:
// real hardware sources need bounded, abortable read timeouts for exactly
// this reason, because Pipeline::stop() and setSource() unblock a parked
// read by calling IqSource::stop() before joining the source thread. A
// source whose read() could block unboundedly with no abort would stall
// every swap and every stop for the full block.
//
// Every wait loop is deadline-bounded (<= 30 s, steady_clock) so a liveness
// bug fails the test instead of hanging it; ctest's 120 s timeout backstops
// a hung join. No randomness is used — all fake payloads are constants.
//
// SPDX-License-Identifier: MIT
#include <algorithm>
#include <atomic>
#include <chrono>
#include <complex>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "core/pipeline.hpp"
#include "source/iq_source.hpp"
#include "test_check.hpp"

using cascade::core::Pipeline;
using cascade::core::SpectrumFrame;
using cascade::source::IqSource;
using std::chrono::milliseconds;
using std::chrono::steady_clock;

namespace {

constexpr auto kDeadline = std::chrono::seconds(30);

// --- Shared observation state -----------------------------------------------

// Owned by the TEST and declared to outlive every fake that points at it, so
// the torn-state flags remain readable after the fake is destroyed — even by
// a leaked source thread still (incorrectly) calling into freed memory.
struct FakeState {
    std::atomic<int> inRead{0};                   // reads currently in flight
    std::atomic<std::uint64_t> readCalls{0};      // total read() entries
    std::atomic<bool> destroyed{false};           // destructor has run
    std::atomic<bool> tornDestruction{false};     // dtor overlapped a read()
    std::atomic<bool> readAfterDestroy{false};    // read() entered post-dtor
    std::atomic<std::uint64_t> delivered{0};      // samples handed to caller
    std::atomic<std::uint64_t> zeroReturns{0};    // reads that returned 0
    std::atomic<bool> finished{false};            // metered source exhausted
};

// Read-entry bracket. Order matters (see the file header): increment inRead
// FIRST, then check destroyed — paired with the destructor's inverse order —
// so in the seq_cst total order any read that overlaps or follows the
// destructor trips at least one flag.
struct ReadScope {
    explicit ReadScope(FakeState& s) : s_(s) {
        s_.inRead.fetch_add(1);
        s_.readCalls.fetch_add(1);
        if (s_.destroyed.load()) { s_.readAfterDestroy.store(true); }
    }
    ~ReadScope() { s_.inRead.fetch_sub(1); }
    ReadScope(const ReadScope&) = delete;
    ReadScope& operator=(const ReadScope&) = delete;
    FakeState& s_;
};

// Destructor-side half of the torn-state protocol.
void markDestroyed(FakeState& s) {
    s.destroyed.store(true);
    if (s.inRead.load() != 0) { s.tornDestruction.store(true); }
}

// --- Fakes -------------------------------------------------------------------

// Free-running fake: read() fills the whole request with pure DC (1 + 0j)
// immediately, so its spectrum peaks exactly at the fftshifted DC bin —
// spectrally distinguishable from the generator's 125 kHz tone, which is what
// proves post-swap frames really derive from the NEW source.
class DcSource final : public IqSource {
public:
    DcSource(FakeState& st, double rateHz) : st_(st), rateHz_(rateHz) {}
    ~DcSource() override { markDestroyed(st_); }

    bool start() override { running_ = true; return true; }
    void stop() override { running_ = false; }
    bool running() const override { return running_; }
    bool selfPaced() const override { return false; }
    double sampleRateHz() const override { return rateHz_; }
    bool setSampleRateHz(double) override { return false; }
    double centerFrequencyHz() const override { return 0.0; }
    bool setCenterFrequencyHz(double) override { return true; }

    std::size_t read(std::complex<float>* dst, std::size_t n) override {
        ReadScope scope(st_);
        for (std::size_t i = 0; i < n; ++i) {
            dst[i] = std::complex<float>(1.0f, 0.0f);
        }
        st_.delivered.fetch_add(n);
        return n;  // free-running: always fills
    }

    const char* name() const override { return "DC fake"; }
    const char* lastError() const override { return ""; }

private:
    FakeState& st_;
    double rateHz_;
    bool running_ = false;
};

// Self-paced fake that meters out EXACTLY total samples on an internal
// schedule: batch k (kBatch samples) becomes available kIntervalMs*k after
// start(). read() bounded-blocks (kReadTimeout) until something is due or
// abort; a timeout with nothing due returns 0 — deliberately frequent
// (timeout < interval) so the test can prove the pipeline treats 0 as
// "retry", not EOF, and that the no-pacing-clock path conserves every
// sample through to published FFT frames.
class MeteredSource final : public IqSource {
public:
    MeteredSource(FakeState& st, std::uint64_t total) : st_(st), total_(total) {}
    ~MeteredSource() override { markDestroyed(st_); }

    static constexpr std::uint64_t kBatch = 2000;
    static constexpr std::int64_t kIntervalMs = 5;
    static constexpr milliseconds kReadTimeout{2};

    bool start() override {
        std::lock_guard<std::mutex> lk(m_);
        t0_ = steady_clock::now();
        started_ = true;
        return true;
    }
    void stop() override {
        { std::lock_guard<std::mutex> lk(m_); abort_ = true; }
        cv_.notify_all();
    }
    bool running() const override {
        std::lock_guard<std::mutex> lk(m_);
        return started_ && !abort_;
    }
    bool selfPaced() const override { return true; }
    double sampleRateHz() const override {
        return 1000.0 * static_cast<double>(kBatch) /
               static_cast<double>(kIntervalMs);
    }
    bool setSampleRateHz(double) override { return false; }
    double centerFrequencyHz() const override { return 0.0; }
    bool setCenterFrequencyHz(double) override { return true; }

    std::size_t read(std::complex<float>* dst, std::size_t n) override {
        ReadScope scope(st_);
        if (dst == nullptr || n == 0) { return 0; }
        std::unique_lock<std::mutex> lk(m_);
        // Bounded block: due samples, abort, or timeout — whichever first.
        cv_.wait_for(lk, kReadTimeout, [&] { return abort_ || dueNow() > given_; });
        if (abort_) { return 0; }
        const std::uint64_t due = dueNow();
        if (due <= given_) {
            st_.zeroReturns.fetch_add(1);
            return 0;  // timed out empty — caller must retry
        }
        const std::uint64_t avail = due - given_;
        const std::size_t m = static_cast<std::size_t>(
            std::min<std::uint64_t>(avail, static_cast<std::uint64_t>(n)));
        for (std::size_t i = 0; i < m; ++i) {
            dst[i] = std::complex<float>(0.25f, 0.0f);
        }
        given_ += m;
        st_.delivered.fetch_add(m);
        if (given_ >= total_) { st_.finished.store(true); }
        return m;
    }

    const char* name() const override { return "metered fake"; }
    const char* lastError() const override { return ""; }

private:
    // Samples whose scheduled release time has passed (caller holds m_).
    std::uint64_t dueNow() const {
        if (!started_) { return 0; }
        const auto elapsedMs = std::chrono::duration_cast<milliseconds>(
                                   steady_clock::now() - t0_)
                                   .count();
        const std::uint64_t batches =
            1 + static_cast<std::uint64_t>(elapsedMs) /
                    static_cast<std::uint64_t>(kIntervalMs);
        return std::min(total_, batches * kBatch);
    }

    FakeState& st_;
    const std::uint64_t total_;
    mutable std::mutex m_;
    std::condition_variable cv_;
    steady_clock::time_point t0_{};
    std::uint64_t given_ = 0;
    bool started_ = false;
    bool abort_ = false;
};

// Self-paced fake that never has data: read() parks for up to 10 s waiting —
// deliberately far past the pipeline's stop() latency budget — unless its
// stop() aborts the wait. This is what proves Pipeline::stop() unblocks a
// parked read through IqSource::stop() rather than waiting out the bound.
class BlockingSource final : public IqSource {
public:
    explicit BlockingSource(FakeState& st) : st_(st) {}
    ~BlockingSource() override { markDestroyed(st_); }

    bool start() override { return true; }
    void stop() override {
        { std::lock_guard<std::mutex> lk(m_); abort_ = true; }
        cv_.notify_all();
    }
    bool running() const override {
        std::lock_guard<std::mutex> lk(m_);
        return !abort_;
    }
    bool selfPaced() const override { return true; }
    double sampleRateHz() const override { return 1000000.0; }
    bool setSampleRateHz(double) override { return false; }
    double centerFrequencyHz() const override { return 0.0; }
    bool setCenterFrequencyHz(double) override { return true; }

    std::size_t read(std::complex<float>*, std::size_t) override {
        ReadScope scope(st_);
        std::unique_lock<std::mutex> lk(m_);
        cv_.wait_for(lk, std::chrono::seconds(10), [&] { return abort_; });
        return 0;
    }

    const char* name() const override { return "blocking fake"; }
    const char* lastError() const override { return ""; }

private:
    FakeState& st_;
    mutable std::mutex m_;
    std::condition_variable cv_;
    bool abort_ = false;
};

// Self-paced fake whose every read() holds for ~20 ms (abortable) before
// delivering a small batch. The hold keeps the source thread INSIDE read()
// essentially all the time, which is what makes the torn-destruction check
// deterministic: an unquiesced swap destroys this object mid-read with
// near-certainty, and even in the sliver where it lands between reads, the
// loop's very next read() entry trips readAfterDestroy instead.
class SlowSource final : public IqSource {
public:
    explicit SlowSource(FakeState& st) : st_(st) {}
    ~SlowSource() override { markDestroyed(st_); }

    bool start() override { return true; }
    void stop() override {
        { std::lock_guard<std::mutex> lk(m_); abort_ = true; }
        cv_.notify_all();
    }
    bool running() const override {
        std::lock_guard<std::mutex> lk(m_);
        return !abort_;
    }
    bool selfPaced() const override { return true; }
    double sampleRateHz() const override { return 12800.0; }
    bool setSampleRateHz(double) override { return false; }
    double centerFrequencyHz() const override { return 0.0; }
    bool setCenterFrequencyHz(double) override { return true; }

    std::size_t read(std::complex<float>* dst, std::size_t n) override {
        ReadScope scope(st_);
        std::unique_lock<std::mutex> lk(m_);
        cv_.wait_for(lk, milliseconds(20), [&] { return abort_; });
        if (abort_ || dst == nullptr || n == 0) { return 0; }
        const std::size_t m = std::min<std::size_t>(n, 256);
        for (std::size_t i = 0; i < m; ++i) {
            dst[i] = std::complex<float>(0.25f, 0.0f);
        }
        st_.delivered.fetch_add(m);
        return m;
    }

    const char* name() const override { return "slow fake"; }
    const char* lastError() const override { return ""; }

private:
    FakeState& st_;
    mutable std::mutex m_;
    std::condition_variable cv_;
    bool abort_ = false;
};

// --- Helpers -----------------------------------------------------------------

std::size_t argmax(const std::vector<float>& v) {
    std::size_t best = 0;
    for (std::size_t i = 1; i < v.size(); ++i) {
        if (v[i] > v[best]) { best = i; }
    }
    return best;
}

// |a - b| without size_t underflow.
std::size_t absDiff(std::size_t a, std::size_t b) {
    return (a > b) ? (a - b) : (b - a);
}

// Polls for NEW frames (seq-gated, so frames provably keep flowing) until
// the spectral peak sits within +/-1 bin of `bin` or the deadline expires.
// Convergence is expected: the estimator's EMA (alpha 0.5) forgets the old
// source within a handful of frames once the new source's samples dominate.
bool waitForPeakAt(Pipeline& p, SpectrumFrame& f, std::size_t bin) {
    const auto t0 = steady_clock::now();
    while (steady_clock::now() - t0 < kDeadline) {
        if (p.getLatestFrame(f) && f.dbBins.size() != 0 &&
            absDiff(argmax(f.dbBins), bin) <= 1) {
            return true;
        }
        std::this_thread::sleep_for(milliseconds(1));
    }
    return false;
}

// Waits (deadline-bounded) until pred() holds; returns whether it did.
template <typename Pred>
bool waitUntil(Pred pred) {
    const auto t0 = steady_clock::now();
    while (steady_clock::now() - t0 < kDeadline) {
        if (pred()) { return true; }
        std::this_thread::sleep_for(milliseconds(1));
    }
    return false;
}

}  // namespace

int main() {
    Pipeline::Config cfg;
    cfg.sampleRateHz = 1000000.0;
    cfg.fftSize = 1024;
    cfg.averagingAlpha = 0.5f;
    cfg.audioEnabled = false;  // no device needed; the chain still runs

    // 125 kHz at 1 MHz / 1024 bins = exactly +128 bins from DC; fftshifted
    // layout puts DC at 512, so tone at 640 — computed from the config, not
    // from implementation output.
    const double toneHz = 125000.0;
    const std::size_t dcBin = cfg.fftSize / 2;
    const std::size_t toneBin =
        dcBin + static_cast<std::size_t>(toneHz / cfg.sampleRateHz *
                                             static_cast<double>(cfg.fftSize) +
                                         0.5);

    // --- 1. Hot swap mid-run: no deadlock, frames flow from the NEW source,
    //        and a null swap restores the generator's spectrum. ------------
    {
        FakeState dcSt;  // outlives the pipeline (and thus the fake)
        {
            Pipeline p(cfg);
            p.sigGen().setTone(0, toneHz, 0.0f);
            p.sigGen().setNoiseFloorDb(-300.0f);
            p.start();

            SpectrumFrame f;
            CHECK(waitForPeakAt(p, f, toneBin));  // generator baseline

            // Swap to the DC fake while running. waitForPeakAt only accepts
            // NEW frames (seq-gated), so success simultaneously proves the
            // swap did not deadlock, frames kept flowing, and their content
            // now comes from the new source (peak moved to the DC bin).
            p.setSource(std::make_unique<DcSource>(dcSt, cfg.sampleRateHz));
            CHECK(std::strcmp(p.activeSourceName(), "DC fake") == 0);
            CHECK(waitForPeakAt(p, f, dcBin));
            CHECK(dcSt.delivered.load() > 0);

            // Null restores the built-in generator: the tone returns.
            p.setSource(nullptr);
            CHECK(std::strcmp(p.activeSourceName(), "Signal generator") == 0);
            CHECK(p.activeSource().selfPaced() == false);
            CHECK(waitForPeakAt(p, f, toneBin));

            p.stop();
            CHECK(p.running() == false);
        }
        // The null swap destroyed the fake; the quiesce handshake means its
        // destruction never overlapped a read.
        CHECK(dcSt.destroyed.load() == true);
        CHECK(dcSt.tornDestruction.load() == false);
        CHECK(dcSt.readAfterDestroy.load() == false);
    }

    // --- 2. Self-paced conservation: with the pacing clock out of the loop,
    //        every metered sample reaches the DSP thread. Full end-to-end
    //        sample-count conservation is unobservable (a final partial FFT
    //        block legitimately stays in the ring), so per the contract we
    //        assert the frame floor instead: N samples must yield at least
    //        floor(N / fftSize) - 1 published frames, and the zero-returns
    //        the source deliberately produced must have been retried (the
    //        deliveries that followed them are the proof). --------------------
    {
        FakeState mSt;
        const std::uint64_t kTotal = 100000;  // 50 batches of 2000, 5 ms apart
        const std::uint64_t expectMin =
            kTotal / static_cast<std::uint64_t>(cfg.fftSize) - 1;  // 96
        {
            Pipeline p(cfg);
            p.setSource(std::make_unique<MeteredSource>(mSt, kTotal));
            CHECK(std::strcmp(p.activeSourceName(), "metered fake") == 0);
            CHECK(p.activeSource().selfPaced() == true);
            p.start();

            // Wait until the source has metered out everything AND the frame
            // floor is met (deadline-bounded — a pacing clock wrongly left in
            // this path would discard samples and hold seq below the floor).
            SpectrumFrame f;
            CHECK(waitUntil([&] {
                (void)p.getLatestFrame(f);
                return mSt.finished.load() && f.seq >= expectMin;
            }));
            CHECK(mSt.delivered.load() == kTotal);  // source-side conservation
            CHECK(f.seq >= expectMin);              // >= floor(N/fft) - 1 frames
            // The schedule guarantees empty bounded-block timeouts (2 ms
            // timeout vs 5 ms batch interval); deliveries continued after
            // them, so the pipeline retried rather than treating 0 as EOF.
            CHECK(mSt.zeroReturns.load() > 0);

            p.stop();
        }
        CHECK(mSt.tornDestruction.load() == false);
        CHECK(mSt.readAfterDestroy.load() == false);
    }

    // --- 3. stop() during a blocked self-paced read returns promptly: the
    //        pipeline must abort the read via IqSource::stop() (the fake's
    //        10 s internal bound would otherwise dominate). -------------------
    {
        FakeState bSt;
        {
            Pipeline p(cfg);
            p.setSource(std::make_unique<BlockingSource>(bSt));
            p.start();

            // Ensure the source thread has actually parked inside read().
            CHECK(waitUntil([&] { return bSt.inRead.load() > 0; }));

            const auto t0 = steady_clock::now();
            p.stop();
            const double stopSec =
                std::chrono::duration<double>(steady_clock::now() - t0).count();
            CHECK(stopSec < 5.0);  // far under the fake's 10 s block
            CHECK(p.running() == false);
        }
        CHECK(bSt.readCalls.load() > 0);
        CHECK(bSt.tornDestruction.load() == false);
        CHECK(bSt.readAfterDestroy.load() == false);
    }

    // --- 4. Torn-state check (the mutant detector): swapping away from a
    //        source whose reads hold ~20 ms must never destroy it mid-read.
    //        A setSource that skips the quiesce handshake fails HERE: the
    //        live swap runs the fake's destructor while the source thread is
    //        parked inside read(), so markDestroyed sees inRead != 0 and
    //        latches tornDestruction into the test-owned state before the
    //        object is freed (and the leaked thread's next call would latch
    //        readAfterDestroy — either flag turns the CHECKs below red). ------
    {
        FakeState slowSt;
        {
            Pipeline p(cfg);
            p.sigGen().setTone(0, toneHz, 0.0f);
            p.sigGen().setNoiseFloorDb(-300.0f);
            p.setSource(std::make_unique<SlowSource>(slowSt));
            p.start();

            // Let the thread settle into the 20 ms blocking-read cadence so
            // an unquiesced swap would land mid-read with near-certainty.
            CHECK(waitUntil([&] { return slowSt.readCalls.load() >= 3; }));

            p.setSource(nullptr);  // correct impl: stop -> join -> destroy
            // Grace window: give a mutant's leaked source thread time to trip
            // readAfterDestroy before asserting. Harmless when correct.
            std::this_thread::sleep_for(milliseconds(200));

            CHECK(slowSt.destroyed.load() == true);  // swap really destroyed it
            CHECK(slowSt.tornDestruction.load() == false);
            CHECK(slowSt.readAfterDestroy.load() == false);

            // And the restored generator is live again: frames converge back
            // to the tone (also proves the respawned thread reads the NEW
            // active source, not a stale captured one).
            SpectrumFrame f;
            CHECK(waitForPeakAt(p, f, toneBin));

            p.stop();
        }
    }

    return testSummary("test_source_swap");
}
