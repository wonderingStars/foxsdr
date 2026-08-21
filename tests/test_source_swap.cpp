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
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <atomic>
#include <chrono>
#include <complex>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "core/pipeline.hpp"
#include "core/plugin_host.hpp"
#include "core/plugin_runner.hpp"
// For asyncOpenStillWanted (block 6). Free function in a header that pulls in
// no GLFW/ImGui — see the note at the top of app_window.hpp.
#include "gui/app_window.hpp"
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

    // --- Destroy-under-read latch (block 5) ---------------------------------
    // The gate a parked read blocks on lives HERE, in test-owned state, not in
    // the fake. That is deliberate: block 5 deliberately provokes a swap that
    // may destroy the fake while a read is parked, and a read parked on the
    // fake's OWN mutex/condition_variable would then be waiting on freed
    // memory — the failure would arrive as an access violation instead of as
    // a named CHECK. Parked on test-owned memory it stays well defined long
    // enough to report.
    std::mutex gateM;
    std::condition_variable gateCv;
    std::atomic<bool> park{false};       // test asks the fake to park in read()
    std::atomic<bool> parked{false};     // fake is inside the parked read()
    std::atomic<bool> gateOpen{false};   // release for the parked read
    std::atomic<bool> loopDone{false};   // source loop's last touch after destroy
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
    // Reads forced to return empty before any data flows (see read()).
    static constexpr int kForcedEmptyReads = 3;

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
        // Deterministic starve window. The retry-after-empty property used to
        // be left to timing luck ("a 2 ms timeout must expire inside a 5 ms
        // batch interval"), which is only true on an idle machine: under load
        // the reads land after data is already due and zeroReturns stays 0.
        // Measured 13 failures in 40 isolated runs. Forcing the first few
        // reads to return empty makes the property tested by construction
        // instead of by hope — and tests it harder, since the pipeline now
        // meets an empty source immediately at startup.
        if (forcedEmpty_.load(std::memory_order_relaxed) > 0) {
            forcedEmpty_.fetch_sub(1, std::memory_order_relaxed);
            st_.zeroReturns.fetch_add(1);
            return 0;
        }
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
    std::atomic<int> forcedEmpty_{kForcedEmptyReads};
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

// Self-paced fake for the DSP-FAULT swap case (block 5). It delivers full
// chunks until the test raises st.park, then enters one read() that blocks on
// the TEST-OWNED gate until something aborts it. Its stop() is that abort, so
// a pipeline that honours the quiesce handshake gets it out of read()
// promptly; a pipeline that skips the handshake destroys it while that read
// is still in flight, which is exactly what the block is written to catch.
class LatchSource final : public IqSource {
public:
    explicit LatchSource(FakeState& st) : st_(st) {}

    ~LatchSource() override {
        // Latch the overlap FIRST — that is the property under test, and it
        // has to be recorded before anything else can go wrong.
        markDestroyed(st_);
        if (st_.inRead.load() == 0) { return; }  // correct impl: nothing parked

        // BUGGY PATH ONLY. A read is in flight on an object being destroyed.
        // The flags are latched already, so the destructor's remaining job is
        // to make the failure REPORTABLE rather than a 0xC0000005: release the
        // parked read and stay inside this destructor body until the source
        // loop has finished touching us. Inside a destructor body the dynamic
        // type is still LatchSource and every member is still alive, so the
        // loop's post-read virtual call (faulted(), below) remains well
        // defined for as long as we are here. Bounded, so a wedged loop fails
        // the suite instead of hanging it.
        { std::lock_guard<std::mutex> lk(st_.gateM); st_.gateOpen.store(true); }
        st_.gateCv.notify_all();
        const auto deadline = steady_clock::now() + std::chrono::seconds(10);
        while (!st_.loopDone.load() && steady_clock::now() < deadline) {
            std::this_thread::sleep_for(milliseconds(1));
        }
    }

    bool start() override { return true; }
    void stop() override {
        { std::lock_guard<std::mutex> lk(st_.gateM); st_.gateOpen.store(true); }
        st_.gateCv.notify_all();
    }
    bool running() const override { return !st_.gateOpen.load(); }
    bool selfPaced() const override { return true; }
    double sampleRateHz() const override { return 1000000.0; }
    bool setSampleRateHz(double) override { return false; }
    double centerFrequencyHz() const override { return 0.0; }
    bool setCenterFrequencyHz(double) override { return true; }

    // The source loop calls this immediately after every read() returns, and
    // on the exit iteration it is the LAST thing it touches on the source.
    // That makes it the "you may be freed now" signal the destructor above
    // waits for on the buggy path; while the fake is alive it is an ordinary
    // "no fault here".
    bool faulted() const override {
        if (st_.destroyed.load()) { st_.loopDone.store(true); }
        return false;
    }

    std::size_t read(std::complex<float>* dst, std::size_t n) override {
        // Bind the test-owned state BEFORE parking: on the buggy path `this`
        // is destroyed while this call is still on the stack, so nothing below
        // the park may reach through the object again.
        FakeState& st = st_;
        ReadScope scope(st);
        if (!st.park.load()) {
            if (dst == nullptr || n == 0) { return 0; }
            for (std::size_t i = 0; i < n; ++i) {
                dst[i] = std::complex<float>(0.25f, 0.0f);
            }
            st.delivered.fetch_add(n);
            return n;
        }
        std::unique_lock<std::mutex> lk(st.gateM);
        st.parked.store(true);
        // Generous but bounded: the abort is supposed to arrive in
        // milliseconds, and 20 s only ever elapses when the thing under test
        // is broken.
        st.gateCv.wait_for(lk, std::chrono::seconds(20),
                           [&] { return st.gateOpen.load(); });
        return 0;
    }

    const char* name() const override { return "latch fake"; }
    const char* lastError() const override { return ""; }

private:
    FakeState& st_;
};

// --- A plugin that throws, to fault the DSP thread on demand -----------------
//
// The DSP-thread fault is not reachable from a fake SOURCE: a source that
// faults does so ON the source thread, which then leaves read() and exits, so
// there is never a read in flight to destroy under. The fault has to come from
// the OTHER thread, and the only third-party code the DSP thread runs is a
// decoder plugin — whose ABI says "never throws" and whose catch-all in
// Pipeline::dspThreadMain exists precisely because that promise can be broken.
// So the fault is injected exactly where the product would really take one.
//
// process() blocks until the source has parked before it throws. A real
// decoder must not block (the ABI says so), but that rendezvous is what makes
// the test deterministic rather than a race between the DSP thread's next
// block and the source thread's next read.
struct ThrowingIqDecoder {
    FakeState* st = nullptr;
    std::atomic<int> calls{0};
};

ThrowingIqDecoder g_iqDecoder;

void* iqCreate(double, double) { return &g_iqDecoder; }

void iqProcess(void*, const float*, std::size_t) {
    g_iqDecoder.calls.fetch_add(1);
    if (g_iqDecoder.st != nullptr) {
        const auto deadline = steady_clock::now() + std::chrono::seconds(20);
        while (!g_iqDecoder.st->parked.load() && steady_clock::now() < deadline) {
            std::this_thread::sleep_for(milliseconds(1));
        }
    }
    throw std::runtime_error("decoder plugin exploded");
}

std::int32_t iqPoll(void*, char*, std::size_t) { return 0; }

void iqDestroy(void*) {}

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
            // The source forces its first kForcedEmptyReads reads to return
            // empty (and may time out empty later too), so this is guaranteed
            // by construction rather than by timing. Deliveries continued
            // afterwards, which is the actual property: the pipeline retried
            // instead of treating 0 as end-of-stream.
            CHECK(mSt.zeroReturns.load() >= MeteredSource::kForcedEmptyReads);

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

    // --- 5. Swapping AFTER a DSP-thread fault must still quiesce the source
    //        thread. The quiesce handshake used to be gated on run_, and a
    //        thread fault clears run_ WITHOUT joining anything — so from the
    //        moment the DSP thread died, setSource believed there was no
    //        source thread to wait for and destroyed the source while its
    //        read() was still parked inside the driver. That is a
    //        use-after-free on the source thread, reached by the ordinary
    //        "the radio stopped, pick another one" click.
    //
    //        The sequencing is a rendezvous, not a race: the plugin's
    //        process() (running on the DSP thread) waits until the source has
    //        parked inside read() before it throws, so at the instant run_
    //        clears there is guaranteed to be a read in flight. ---------------
    {
        FakeState latchSt;
        {
            // Declared before the pipeline so it outlives it: the DSP thread
            // dereferences this pointer every block.
            cascade::core::PluginRunner runner;

            CascadeIqDecoderApi api{};
            api.structSize = static_cast<std::uint32_t>(sizeof(CascadeIqDecoderApi));
            api.requiredRateHz = 0.0;  // "any rate": always instantiated
            api.preferredRateHz = 0.0;
            api.create = &iqCreate;
            api.process = &iqProcess;
            api.retune = nullptr;
            api.poll_text = &iqPoll;
            api.destroy = &iqDestroy;

            cascade::core::LoadedPlugin lp;
            lp.loaded = true;
            lp.name = "exploding";
            lp.version = "1.0.0";
            lp.iqDecoder = &api;
            std::vector<cascade::core::LoadedPlugin> plugins{lp};

            g_iqDecoder.st = &latchSt;
            g_iqDecoder.calls.store(0);
            runner.rebuild(plugins, 48000.0, cfg.sampleRateHz, 0.0);
            CHECK(runner.activeCount() == 1);

            Pipeline p(cfg);
            p.setPluginRunner(&runner);
            p.setSource(std::make_unique<LatchSource>(latchSt));
            p.start();

            // The source loop is running and the DSP thread has reached the
            // plugin (where it now waits for the park).
            CHECK(waitUntil([&] { return latchSt.readCalls.load() >= 2; }));
            CHECK(waitUntil([&] { return g_iqDecoder.calls.load() > 0; }));

            // Park the source inside read(), which releases the plugin's
            // throw and takes the DSP thread — and with it run_ — down.
            latchSt.park.store(true);
            CHECK(waitUntil([&] { return latchSt.parked.load(); }));
            CHECK(waitUntil([&] { return p.faulted(); }));
            CHECK(waitUntil([&] { return !p.running(); }));
            const std::string msg = p.faultMessage();
            CHECK(msg.find("DSP thread") != std::string::npos);
            CHECK(msg.find("decoder plugin exploded") != std::string::npos);

            // The state the bug needs: run_ is false, and the source thread is
            // STILL inside read(). If this ever stops holding, the block below
            // proves nothing, so it is asserted rather than assumed.
            CHECK(latchSt.inRead.load() > 0);

            // The user picks another source. This must wait for the parked
            // read to leave before destroying what it is parked in.
            p.setSource(nullptr);

            CHECK(latchSt.destroyed.load() == true);           // it really was destroyed
            CHECK(latchSt.tornDestruction.load() == false);    // ...but not under a read
            CHECK(latchSt.readAfterDestroy.load() == false);
            CHECK(latchSt.inRead.load() == 0);                 // the read had exited first

            // And the pipeline is left restartable rather than wedged: detach
            // the exploding plugin, restart, and frames must flow again.
            p.setPluginRunner(nullptr);
            p.sigGen().setTone(0, toneHz, 0.0f);
            p.sigGen().setNoiseFloorDb(-300.0f);
            p.start();
            CHECK(p.faulted() == false);
            SpectrumFrame f;
            CHECK(waitForPeakAt(p, f, toneBin));
            p.stop();
            p.setPluginRunner(nullptr);
        }
        g_iqDecoder.st = nullptr;
    }

    // --- 5b. RESTARTING after a DSP-thread fault must abort the in-flight read
    //         before joining, exactly as stop() and setSource do.
    //
    //         Same state as block 5 — run_ cleared by the fault, the source
    //         thread still parked inside a self-paced read() — but the user
    //         presses Start instead of picking another source. start() joined
    //         the source thread WITHOUT calling active_->stop() first, so the
    //         join could only complete when the parked read gave up on its own:
    //         the whole of the device's read timeout, during which start()
    //         holds controlMutex_ and the application is wedged. Real drivers
    //         use timeouts of a second or more, and the fake below uses the
    //         same 20 s bound the other blocks do, so the difference between
    //         aborting and waiting is not subtle.
    //
    //         The bound is deliberately generous: what is being measured is
    //         "returned promptly" against "waited out a full read timeout",
    //         and a loaded machine cannot turn one into the other. -----------
    {
        FakeState latchSt;
        {
            cascade::core::PluginRunner runner;

            CascadeIqDecoderApi api{};
            api.structSize = static_cast<std::uint32_t>(sizeof(CascadeIqDecoderApi));
            api.requiredRateHz = 0.0;
            api.preferredRateHz = 0.0;
            api.create = &iqCreate;
            api.process = &iqProcess;
            api.retune = nullptr;
            api.poll_text = &iqPoll;
            api.destroy = &iqDestroy;

            cascade::core::LoadedPlugin lp;
            lp.loaded = true;
            lp.name = "exploding";
            lp.version = "1.0.0";
            lp.iqDecoder = &api;
            std::vector<cascade::core::LoadedPlugin> plugins{lp};

            g_iqDecoder.st = &latchSt;
            g_iqDecoder.calls.store(0);
            runner.rebuild(plugins, 48000.0, cfg.sampleRateHz, 0.0);
            CHECK(runner.activeCount() == 1);

            Pipeline p(cfg);
            p.setPluginRunner(&runner);
            p.setSource(std::make_unique<LatchSource>(latchSt));
            p.start();

            CHECK(waitUntil([&] { return latchSt.readCalls.load() >= 2; }));
            CHECK(waitUntil([&] { return g_iqDecoder.calls.load() > 0; }));

            latchSt.park.store(true);
            CHECK(waitUntil([&] { return latchSt.parked.load(); }));
            CHECK(waitUntil([&] { return p.faulted(); }));
            CHECK(waitUntil([&] { return !p.running(); }));

            // The state the bug needs, asserted rather than assumed: run_ is
            // false and a read is still in flight on the source thread.
            CHECK(latchSt.inRead.load() > 0);

            // Detach the exploding plugin so the restart is about the join and
            // nothing else, then press Start and time it.
            p.setPluginRunner(nullptr);
            const auto t0 = steady_clock::now();
            p.start();
            const auto elapsed = std::chrono::duration_cast<milliseconds>(
                                     steady_clock::now() - t0)
                                     .count();
            std::printf("start() after a DSP fault took %lld ms\n",
                        static_cast<long long>(elapsed));
            // The parked read's own bound is 20 s; aborting it costs
            // milliseconds. Anything near the bound means start() sat waiting.
            CHECK(elapsed < 5000);

            // ...and it really did restart, rather than returning early.
            CHECK(p.running());
            CHECK(!p.faulted());
            CHECK(p.faultMessage().empty());

            p.stop();
            CHECK(!p.running());
            CHECK(latchSt.readAfterDestroy.load() == false);
            CHECK(latchSt.tornDestruction.load() == false);
        }
        g_iqDecoder.st = nullptr;
    }

    // --- 6. The other half of "the user switched away": a device open that
    //        finishes on a worker AFTER the user picked something else must
    //        not be applied. The GUI wiring around it is not reachable from a
    //        unit test (it needs a window, a real driver and a multi-second
    //        open), but the decision the wiring now defers to is, so it is
    //        asserted here rather than left to inspection alone. -------------
    {
        // Nothing installed since the request: the answer is still about the
        // source the user is looking at.
        CHECK(cascade::gui::asyncOpenStillWanted(0, 0) == true);
        CHECK(cascade::gui::asyncOpenStillWanted(7, 7) == true);
        // The user installed a source in the meantime (generator, IQ file, or
        // a device that resolved first): the answer is stale.
        CHECK(cascade::gui::asyncOpenStillWanted(7, 8) == false);
        CHECK(cascade::gui::asyncOpenStillWanted(0, 1) == false);
        // Two switches while one open was in flight is still just "stale" —
        // the counter is compared, never differenced.
        CHECK(cascade::gui::asyncOpenStillWanted(7, 9) == false);
    }

    return testSummary("test_source_swap");
}
