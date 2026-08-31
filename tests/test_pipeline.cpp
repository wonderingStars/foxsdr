// Tests for core/pipeline.hpp — the threaded render pipeline.
//
// Every wait loop is deadline-bounded on steady_clock (<= 30 s) so a liveness
// bug fails the test instead of hanging it; ctest's 120 s per-test timeout is
// the backstop for a hung join inside stop()/~Pipeline. Expected peak bins are
// computed in-test from the configured tone frequency and the documented
// fftshift layout (DC at fftSize/2), never from implementation output.
//
// The last block pins setInputRateHz's behaviour on a FAULTED pipeline, which
// needs a thread fault injected on demand: a self-paced fake source that parks
// inside read(), plus an I/Q decoder plugin whose process() throws on the DSP
// thread. Those fixtures are deliberately private to this file even though
// test_source_swap.cpp grows similar ones for its own (setSource) subject —
// the two files are owned by different work streams, and a shared fixture
// header would couple them so that a change made for one silently re-aims the
// other's tests. Duplication is the cheaper of the two failure modes here.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/pipeline.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "core/plugin_host.hpp"
#include "core/plugin_runner.hpp"
#include "source/iq_source.hpp"
#include "test_check.hpp"

using cascade::core::Pipeline;
using cascade::core::SpectrumFrame;
using cascade::source::IqSource;
using std::chrono::milliseconds;
using std::chrono::steady_clock;

namespace {

constexpr auto kDeadline = std::chrono::seconds(30);

// Polls getLatestFrame until a frame newer than out.seq arrives or the
// deadline expires. Returns false on timeout — callers CHECK the result, so a
// stalled pipeline fails loudly.
bool waitForNewFrame(Pipeline& p, SpectrumFrame& out) {
    const auto t0 = steady_clock::now();
    while (steady_clock::now() - t0 < kDeadline) {
        if (p.getLatestFrame(out)) { return true; }
        std::this_thread::sleep_for(milliseconds(1));
    }
    return false;
}

std::size_t argmax(const std::vector<float>& v) {
    std::size_t best = 0;
    for (std::size_t i = 1; i < v.size(); ++i) {
        if (v[i] > v[best]) { best = i; }
    }
    return best;
}

float medianOf(std::vector<float> v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

// |a - b| as size_t without underflow.
std::size_t absDiff(std::size_t a, std::size_t b) {
    return (a > b) ? (a - b) : (b - a);
}

// Waits (deadline-bounded) until pred() holds; returns whether it did. Every
// use below is a rendezvous on state the fakes publish, never a fixed sleep,
// so the sequencing is deterministic rather than timing-dependent.
template <typename Pred>
bool waitUntil(Pred pred) {
    const auto t0 = steady_clock::now();
    while (steady_clock::now() - t0 < kDeadline) {
        if (pred()) { return true; }
        std::this_thread::sleep_for(milliseconds(1));
    }
    return false;
}

// Polls for NEW frames until the spectral peak sits within +/-1 bin of `bin`.
// Seq-gated through getLatestFrame, so a pass also proves frames kept flowing.
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

// --- Fault-injection fixtures (last block only) -------------------------------

// Owned by the TEST and declared to outlive every fake that points at it, so
// the torn-state flags stay readable after the fake is destroyed — even by a
// leaked source thread still (incorrectly) calling into freed memory.
struct FakeState {
    std::atomic<int> inRead{0};                 // reads currently in flight
    std::atomic<std::uint64_t> readCalls{0};    // total read() entries
    std::atomic<bool> destroyed{false};         // destructor has run
    std::atomic<bool> tornDestruction{false};   // dtor overlapped a read()
    std::atomic<bool> readAfterDestroy{false};  // read() entered post-dtor
    std::atomic<std::uint64_t> delivered{0};    // samples handed to caller

    // The gate a parked read blocks on lives HERE, in test-owned state, not in
    // the fake: the whole point of the block is a state in which the fake may
    // (wrongly) be destroyed while a read is parked, and a read parked on the
    // fake's OWN mutex would then be waiting on freed memory — an access
    // violation instead of a named CHECK.
    std::mutex gateM;
    std::condition_variable gateCv;
    std::atomic<bool> park{false};      // test asks the fake to park in read()
    std::atomic<bool> parked{false};    // fake is inside the parked read()
    std::atomic<bool> gateOpen{false};  // release for the parked read
    std::atomic<bool> loopDone{false};  // source loop's last touch after destroy
};

// Read-entry bracket. Order matters: increment inRead FIRST, then check
// destroyed — paired with the destructor's inverse order — so in the seq_cst
// total order any read overlapping or following the destructor trips a flag.
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

// Self-paced fake: delivers full chunks until the test raises st.park, then
// enters one read() that blocks on the TEST-OWNED gate until something aborts
// it. Its stop() is that abort, which is the IqSource contract's shutdown path
// — real hardware sources need bounded, abortable reads for exactly this,
// because stop()/setSource() unblock a parked read before joining.
class LatchSource final : public IqSource {
public:
    explicit LatchSource(FakeState& st) : st_(st) {}

    ~LatchSource() override {
        // Latch the overlap FIRST: it is the property under test and has to be
        // recorded before anything else can go wrong.
        markDestroyed(st_);
        if (st_.inRead.load() == 0) { return; }  // correct impl: nothing parked

        // BUGGY PATH ONLY. A read is in flight on an object being destroyed.
        // The flags are latched, so the destructor's remaining job is to make
        // the failure REPORTABLE rather than a 0xC0000005: release the parked
        // read and stay inside this body until the source loop has finished
        // touching us. Inside a destructor body the dynamic type is still
        // LatchSource and every member is alive, so the loop's post-read
        // virtual call (faulted(), below) stays well defined meanwhile.
        // Bounded, so a wedged loop fails the suite instead of hanging it.
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
    // on the exit iteration it is the LAST thing it touches on the source —
    // which makes it the "you may be freed now" signal the destructor waits
    // for on the buggy path. While the fake is alive it is "no fault here".
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
        // milliseconds, and 20 s only ever elapses when something is broken.
        st.gateCv.wait_for(lk, std::chrono::seconds(20),
                           [&] { return st.gateOpen.load(); });
        return 0;
    }

    const char* name() const override { return "latch fake"; }
    const char* lastError() const override { return ""; }

private:
    FakeState& st_;
};

// --- Zombie-safety fixtures (Pipeline::stop()'s bounded source-thread join)
// ---------------------------------------------------------------------------
//
// LatchSource above tests the CORRECT-teardown side of the setSource()
// use-after-free fix: its stop() releases the gate, so every read it parks
// in is abortable and every join in the product completes. This pair tests
// the opposite case Pipeline::stop() now has to survive on its own: a source
// whose read() does not honour stop() at all, standing in for a vendor
// driver that ignores its own timeout and never returns from readStream
// (field report 4214EAE4, a Mirics device). Pipeline::stop() must not wait
// for that read forever — it must give up within its own bound and abandon
// the thread instead.
class HungReadSource final : public IqSource {
public:
    // Published state a test polls instead of sleeping blindly (this file's
    // convention — see waitUntil above): `entered` proves the source thread
    // has actually reached the blocking read() before the test calls stop(),
    // so the abandonment path is genuinely exercised rather than raced past;
    // `returned` proves the block has finished unwinding, which the test
    // needs before it is safe to let the owning Pipeline be destroyed (see
    // the block comment in main()).
    std::atomic<bool> entered{false};
    std::atomic<bool> returned{false};

    bool start() override { running_.store(true); return true; }
    // Deliberately does NOT touch the block in read() below. A vendor
    // driver's readStream() ignoring its own timeout parameter is exactly
    // the failure this fake exists to reproduce, and a stop() that could
    // still abort it would not be testing that failure mode at all.
    void stop() override { running_.store(false); }
    bool running() const override { return running_.load(); }
    bool selfPaced() const override { return true; }
    double sampleRateHz() const override { return 1000000.0; }
    bool setSampleRateHz(double) override { return false; }
    double centerFrequencyHz() const override { return 0.0; }
    bool setCenterFrequencyHz(double) override { return true; }

    std::size_t read(std::complex<float>*, std::size_t) override {
        entered.store(true);
        // Blocks for a FIXED ~8 s, ignoring stop(), unless releaseForTest()
        // is called first. The fixed bound is what makes this a reproduction
        // rather than a hang: it is finite ON PURPOSE, so a regression in
        // Pipeline::stop()'s own bound (currently 3 s) shows up as a FAILED
        // elapsed-time CHECK — read() eventually returns by itself and the
        // suite moves on — rather than a suite that never completes and
        // relies on ctest's 120 s per-test timeout to say so.
        //
        // releaseForTest() is a TEST-ONLY shortcut with no equivalent in the
        // product (it is not wired through stop()): main() only calls it
        // AFTER already asserting Pipeline::stop() returned inside its own
        // 5 s bound, i.e. after the abandonment this class exists to trigger
        // has already happened, purely so the suite is not stuck waiting out
        // the remainder of the 8 s on every run.
        std::unique_lock<std::mutex> lk(m_);
        cv_.wait_for(lk, std::chrono::seconds(8), [this] { return release_.load(); });
        lk.unlock();
        returned.store(true);
        return 0;
    }

    void releaseForTest() {
        { std::lock_guard<std::mutex> lk(m_); release_.store(true); }
        cv_.notify_all();
    }

    const char* name() const override { return "hung-read fake"; }
    const char* lastError() const override { return ""; }

private:
    std::atomic<bool> running_{false};
    std::mutex m_;
    std::condition_variable cv_;
    std::atomic<bool> release_{false};
};

// A self-paced fake that behaves like a healthy device: delivers a filled
// chunk immediately on every read() and honours stop() the way the IqSource
// contract requires — the contrast HungReadSource above exists to draw. Used
// (rather than reverting to the built-in generator via setSource(nullptr))
// specifically to exercise setSource()'s non-null swap path with a genuinely
// different external source object, which is the realistic case: picking a
// different radio after one hangs, not merely giving up on hardware.
class HealthySource final : public IqSource {
public:
    bool start() override { running_.store(true); return true; }
    void stop() override { running_.store(false); }
    bool running() const override { return running_.load(); }
    bool selfPaced() const override { return true; }
    double sampleRateHz() const override { return 1000000.0; }
    bool setSampleRateHz(double) override { return false; }
    double centerFrequencyHz() const override { return 0.0; }
    bool setCenterFrequencyHz(double) override { return true; }

    std::size_t read(std::complex<float>* dst, std::size_t n) override {
        if (!running_.load() || dst == nullptr || n == 0) { return 0; }
        for (std::size_t i = 0; i < n; ++i) {
            dst[i] = std::complex<float>(0.1f, 0.0f);
        }
        return n;
    }

    const char* name() const override { return "healthy fake"; }
    const char* lastError() const override { return ""; }

private:
    std::atomic<bool> running_{false};
};

// A plugin that throws, to fault the DSP thread on demand.
//
// The DSP-thread fault is not reachable from a fake SOURCE: a source that
// faults does so ON the source thread. The fault has to come from the OTHER
// thread, and the only third-party code the DSP thread runs is a decoder
// plugin — whose ABI says "never throws" and whose catch-all in
// Pipeline::dspThreadMain exists precisely because that promise can be broken.
// So the fault is injected exactly where the product would really take one.
//
// process() waits until the source has parked before it throws. A real decoder
// must not block (the ABI says so), but that rendezvous is what makes the test
// deterministic rather than a race between the DSP thread's next block and the
// source thread's next read.
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

}  // namespace

int main() {
    Pipeline::Config cfg;
    cfg.sampleRateHz = 1000000.0;
    cfg.fftSize = 1024;
    cfg.averagingAlpha = 0.5f;

    // 125 kHz at 1 MHz / 1024 bins is exactly 128 bins above DC, so the peak
    // must land on one bin (fftshifted: DC at 512, tone at 640).
    const double toneHz = 125000.0;
    const std::size_t expectedBin =
        cfg.fftSize / 2 +
        static_cast<std::size_t>(toneHz / cfg.sampleRateHz *
                                 static_cast<double>(cfg.fftSize) + 0.5);

    std::uint64_t lastSeqSeen = 0;

    {
        Pipeline p(cfg);
        p.sigGen().setTone(0, toneHz, 0.0f);   // 0 dB = amplitude 1.0
        p.sigGen().setNoiseFloorDb(-300.0f);   // disabled: deterministic floor

        // Source-abstraction surface (additive checks): the default active
        // source is the built-in generator — free-running, named "Signal
        // generator", reporting the configured rate and the documented
        // 100 MHz nominal center. Everything below this block ran unchanged
        // before the IqSource refactor and must behave identically.
        CHECK(std::strcmp(p.activeSourceName(), "Signal generator") == 0);
        CHECK(p.activeSource().selfPaced() == false);
        CHECK(p.activeSource().sampleRateHz() == cfg.sampleRateHz);
        CHECK_NEAR(p.activeSource().centerFrequencyHz(), 100.0e6, 1e-3);
        CHECK(std::strcmp(p.activeSource().lastError(), "") == 0);

        // Rate-follow surface (additive checks): the chain reports the
        // construction rate, and the constructor applies the documented
        // decimation policy decim = round(rate / 200 kHz) clamped >= 1 —
        // computed here from the contract, not read from the implementation
        // (1 MHz -> decim 5 -> a 200 kHz channel).
        CHECK(p.inputRateHz() == cfg.sampleRateHz);
        {
            double expectDecim = std::round(cfg.sampleRateHz / 200000.0);
            if (expectDecim < 1.0) { expectDecim = 1.0; }
            CHECK(p.channelRateHz() == cfg.sampleRateHz / expectDecim);
        }

        // No frame before start.
        SpectrumFrame frame;
        CHECK(p.getLatestFrame(frame) == false);
        CHECK(p.running() == false);

        // start() -> a frame with seq > 0 arrives.
        p.start();
        CHECK(p.running() == true);
        CHECK(waitForNewFrame(p, frame));
        CHECK(frame.seq > 0);
        CHECK(frame.dbBins.size() == cfg.fftSize);

        // Dominant peak at the configured tone bin, +/- 1, and >= 30 dB above
        // the median bin (an exact-bin tone under Blackman-Harris leaves the
        // median at window-sidelobe/float-noise level, > 80 dB down, so 30 dB
        // holds with huge margin against any reasonable SigGen). Guarded on
        // the size check so a broken pipeline reports FAILs instead of
        // indexing an empty vector.
        if (frame.dbBins.size() == cfg.fftSize) {
            const std::size_t peakBin = argmax(frame.dbBins);
            CHECK(absDiff(peakBin, expectedBin) <= 1);
            CHECK(frame.dbBins[peakBin] - medianOf(frame.dbBins) >= 30.0f);
        }

        // seq strictly increases across successive true returns. Each
        // waitForNewFrame only returns true for seq > frame.seq, so ten
        // consecutive successes prove monotonic publishing.
        for (int i = 0; i < 10; ++i) {
            const std::uint64_t prev = frame.seq;
            CHECK(waitForNewFrame(p, frame));
            CHECK(frame.seq > prev);
        }

        // Pacing sanity over a ~1 s window, measured against actual elapsed
        // time. Nominal publish rate is sampleRate/fftSize ~= 976 frames/s;
        // the loose [0.5x, 1.6x] band tolerates scheduler jitter (well beyond
        // the ~20% pacing spec) while still failing on a busy-spinning source
        // (which publishes at FFT throughput, far above 1.6x) or a stalled one.
        {
            SpectrumFrame f2;
            CHECK(waitForNewFrame(p, f2));
            const std::uint64_t s1 = f2.seq;
            const auto t1 = steady_clock::now();
            std::this_thread::sleep_for(milliseconds(1000));
            CHECK(waitForNewFrame(p, f2));
            const double elapsedSec =
                std::chrono::duration<double>(steady_clock::now() - t1).count();
            const double rate =
                static_cast<double>(f2.seq - s1) / elapsedSec;
            const double nominal =
                cfg.sampleRateHz / static_cast<double>(cfg.fftSize);
            CHECK(rate >= 0.5 * nominal);
            CHECK(rate <= 1.6 * nominal);
            frame = f2;
        }

        // stop() joins within the deadline.
        {
            const auto t0 = steady_clock::now();
            p.stop();
            const double stopSec =
                std::chrono::duration<double>(steady_clock::now() - t0).count();
            CHECK(p.running() == false);
            CHECK(stopSec < 5.0);
        }

        // After stop: drain the (at most one) frame newer than ours, then
        // getLatestFrame must return false — deterministically, since no
        // producer exists any more. Loop is bounded: seq is frozen after the
        // join, so at most one true return is possible.
        {
            SpectrumFrame drain = frame;
            (void)p.getLatestFrame(drain);
            CHECK(p.getLatestFrame(drain) == false);
            CHECK(p.getLatestFrame(drain) == false);
            frame = drain;
        }

        // Idempotence: double start, then double stop, all clean.
        p.start();
        p.start();
        CHECK(p.running() == true);
        {
            SpectrumFrame f3 = frame;
            CHECK(waitForNewFrame(p, f3));
            CHECK(f3.seq > frame.seq);  // seq survives a restart (documented)
            frame = f3;
        }
        p.stop();
        p.stop();
        CHECK(p.running() == false);

        // Full second cycle after a complete stop: frames flow again.
        p.start();
        CHECK(p.running() == true);
        {
            SpectrumFrame f4 = frame;
            CHECK(waitForNewFrame(p, f4));
            CHECK(f4.seq > frame.seq);
            frame = f4;
        }
        p.stop();
        CHECK(p.running() == false);

        lastSeqSeen = frame.seq;
    }

    // Destructor while running does not hang, and a negative tone frequency
    // lands below DC (fftshift orientation through the whole pipeline).
    {
        const auto t0 = steady_clock::now();
        {
            Pipeline p(cfg);
            p.sigGen().setTone(0, -toneHz, 0.0f);
            p.sigGen().setNoiseFloorDb(-300.0f);
            p.start();
            SpectrumFrame frame;
            CHECK(waitForNewFrame(p, frame));
            const std::size_t negBin =
                cfg.fftSize / 2 - (expectedBin - cfg.fftSize / 2);
            CHECK(absDiff(argmax(frame.dbBins), negBin) <= 1);
            // Scoped: destroyed here while running.
        }
        const double dtorSec =
            std::chrono::duration<double>(steady_clock::now() - t0).count();
        CHECK(dtorSec < 5.0);
    }

    // --- setInputRateHz on a FAULTED pipeline ---------------------------------
    //
    // setInputRateHz has the same run_-gated shape Pipeline::setSource had
    // before its fix, so the obvious question is whether it carries the same
    // destroy-under-use bug. It does not, and the reason is worth pinning
    // rather than re-deriving: setInputRateHz rebuilds only the DSP-side
    // chain, entirely under audioMutex_ (which processAudioBlock holds for its
    // whole body), and it never touches the SOURCE — so the parked read the
    // setSource bug destroyed under is simply not in its blast radius.
    //
    // This block therefore pins the BEHAVIOUR of a rate change taken while
    // run_ is false because a DSP-thread fault cleared it, so that a future
    // "harmonise the gates" edit has something to fail against: the call
    // completes, really changes the rate, leaves the parked source untouched,
    // does not launder the fault into a running pipeline, and leaves the
    // pipeline restartable at the new rate. It does NOT distinguish the quiesce
    // gate's two spellings (`run_` vs `dspThread_.joinable()`) — in this state
    // dspRun_ is already false and the DSP thread has already exited, so
    // joining its corpse is unobservable from outside; that gate is a
    // structural consistency choice, not a behaviour the API can see.
    //
    // The sequencing is a rendezvous, not a race: the plugin's process()
    // (running on the DSP thread) waits until the source has parked inside
    // read() before it throws, so at the instant run_ clears there is
    // guaranteed to be a read in flight.
    {
        Pipeline::Config faultCfg = cfg;
        faultCfg.audioEnabled = false;  // no device needed; the chain still runs

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
            runner.rebuild(plugins, 48000.0, faultCfg.sampleRateHz, 0.0);
            CHECK(runner.activeCount() == 1);

            Pipeline p(faultCfg);
            p.setPluginRunner(&runner);
            p.setSource(std::make_unique<LatchSource>(latchSt));
            p.start();

            // The source loop is running and the DSP thread has reached the
            // plugin (where it now waits for the park).
            CHECK(waitUntil([&] { return latchSt.readCalls.load() >= 2; }));
            CHECK(waitUntil([&] { return g_iqDecoder.calls.load() > 0; }));

            // Park the source inside read(), which releases the plugin's throw
            // and takes the DSP thread — and with it run_ — down.
            latchSt.park.store(true);
            CHECK(waitUntil([&] { return latchSt.parked.load(); }));
            CHECK(waitUntil([&] { return p.faulted(); }));
            CHECK(waitUntil([&] { return !p.running(); }));

            // The state the whole block is about: run_ is false because the
            // DSP thread died, the DSP std::thread object is joinable because
            // noteThreadFault cannot join itself, and the source thread is
            // still parked inside read(). Asserted, not assumed.
            CHECK(latchSt.inRead.load() > 0);
            CHECK(p.inputRateHz() == 1000000.0);

            // 2.4 MS/s: decim 12, so the channel rate stays 200 kHz and the
            // whole chain really is rebuilt (every block is reconstructed for
            // the new input rate even when the channel rate is unchanged).
            CHECK(p.setInputRateHz(2400000.0) == true);
            CHECK(p.inputRateHz() == 2400000.0);
            CHECK(p.channelRateHz() == 200000.0);

            // It neither destroyed nor waited for the parked source: the rate
            // change has no business with the source side at all, which is
            // exactly why it needs no source-thread handshake.
            CHECK(latchSt.destroyed.load() == false);
            CHECK(latchSt.tornDestruction.load() == false);
            CHECK(latchSt.readAfterDestroy.load() == false);
            CHECK(latchSt.inRead.load() > 0);

            // And it did not launder a dead pipeline into a live one: a rate
            // change is not a restart, so the fault must survive it verbatim.
            CHECK(p.faulted() == true);
            CHECK(p.running() == false);
            const std::string msg = p.faultMessage();
            CHECK(msg.find("DSP thread") != std::string::npos);
            CHECK(msg.find("decoder plugin exploded") != std::string::npos);

            // setSource's own quiesce still works after an intervening rate
            // change: the parked read leaves before the fake is destroyed.
            p.setSource(nullptr);
            CHECK(latchSt.destroyed.load() == true);
            CHECK(latchSt.tornDestruction.load() == false);
            CHECK(latchSt.readAfterDestroy.load() == false);
            CHECK(latchSt.inRead.load() == 0);

            // Restartable, and the rate chosen while faulted is the rate the
            // restart runs at — a rate change on a stopped/faulted pipeline is
            // documented to apply to the NEXT start().
            p.setPluginRunner(nullptr);
            p.sigGen().setTone(0, toneHz, 0.0f);
            p.sigGen().setNoiseFloorDb(-300.0f);
            p.start();
            CHECK(p.faulted() == false);
            CHECK(p.running() == true);
            CHECK(p.inputRateHz() == 2400000.0);
            CHECK(p.channelRateHz() == 200000.0);

            // Frames flow again. The peak bin is the GENERATOR's own
            // 125 kHz / 1 MHz, not the chain's: setInputRateHz deliberately
            // never retunes the source, and the estimator is rate-agnostic.
            SpectrumFrame f;
            CHECK(waitForPeakAt(p, f, expectedBin));
            p.stop();
            p.setPluginRunner(nullptr);
        }
        g_iqDecoder.st = nullptr;
    }

    // --- Pipeline::stop() bounds the source-thread join (field 4214EAE4) -----
    //
    // Reproduces a vendor driver whose read() never returns and never honours
    // stop(): Pipeline::stop() must give up within its own bound (currently
    // 3 s, plus whatever active_->stop() itself costs) rather than wait for
    // that read forever on the calling thread, exactly as the GUI thread did
    // in the field report. It must then leave the pipeline genuinely usable —
    // a different source can be installed and streamed, and stopping THAT
    // session must not inherit any of the abandoned one's delay.
    {
        Pipeline::Config hungCfg = cfg;
        hungCfg.audioEnabled = false;  // no device needed; only the source side matters here

        Pipeline p(hungCfg);
        auto hungSrc = std::make_unique<HungReadSource>();
        HungReadSource* hung = hungSrc.get();  // raw pointer: object outlives this
                                               // scope once abandoned/leaked below
        p.setSource(std::move(hungSrc));
        p.start();
        CHECK(p.running() == true);

        // Rendezvous, not a sleep: only call stop() once the source thread is
        // demonstrably parked inside the blocking read(), so the abandonment
        // path below is genuinely exercised rather than raced past.
        CHECK(waitUntil([&] { return hung->entered.load(); }));

        const auto t0 = steady_clock::now();
        p.stop();
        const double stopMs =
            std::chrono::duration<double, std::milli>(steady_clock::now() - t0).count();
        std::printf("Pipeline::stop() against a hung source: %.1f ms\n", stopMs);
        CHECK(p.running() == false);
        CHECK(stopMs < 5000.0);
        // The counter, not the clock, is what proves ABANDONMENT happened
        // here and JOINING happened for the healthy session below: mutate
        // kSourceJoinWait to zero and every timing assertion in this block
        // still passes (an instant abandonment is fast too) - only these
        // two counts tell the paths apart.
        CHECK(p.abandonedSourceThreads() == 1);  // the load-bearing assertion: see the class
                                 // comment on HungReadSource for why a
                                 // regression fails THIS, not the suite

        // Not laundered into a driver fault: abandonment is a deliberate
        // shutdown outcome, not a worker-thread exception.
        CHECK(p.faulted() == false);

        // The pipeline survives: a genuinely different source can be
        // installed and streamed, proving setSource()'s zombie-safety
        // (release-not-destroy of the outgoing hung source) did not corrupt
        // anything reachable from here.
        p.setSource(std::make_unique<HealthySource>());
        p.start();
        CHECK(p.running() == true);
        SpectrumFrame f;
        CHECK(waitForNewFrame(p, f));
        CHECK(f.seq > 0);

        // stop() of THIS session must join promptly — no zombie inheritance
        // from the abandoned generation's token/exit-latch bookkeeping. A
        // tight bound (well under kSourceJoinWait) is what actually
        // distinguishes "joined normally" from "also had to wait out the
        // abandonment bound again".
        const auto t1 = steady_clock::now();
        p.stop();
        const double stop2Ms =
            std::chrono::duration<double, std::milli>(steady_clock::now() - t1).count();
        std::printf("Pipeline::stop() of the following healthy session: %.1f ms\n",
                    stop2Ms);
        CHECK(p.running() == false);
        CHECK(stop2Ms < 1000.0);
        CHECK(p.abandonedSourceThreads() == 1);  // healthy stop JOINED: no new abandonment

        // Let HungReadSource's own read() finish unwinding before p (and
        // with it, the Pipeline members its thread's loop still touches once
        // more after read() returns — see sourceThreadBody) goes out of
        // scope below. This is a TEST-HARNESS-ONLY requirement: the shipped
        // app's one Pipeline owner (AppWindow) holds it for the entire
        // process lifetime, so an abandoned thread there only ever wakes (if
        // it wakes at all) against a Pipeline that still exists, or is
        // itself killed by process teardown before it runs another
        // instruction — neither of which this short-lived test scope can
        // promise on its own.
        hung->releaseForTest();
        CHECK(waitUntil([&] { return hung->returned.load(); }));
        std::this_thread::sleep_for(milliseconds(250));  // margin past `returned`
                                                          // for the loop's own
                                                          // final flag re-check
    }


    // --- setSource() WITHOUT stop(): the exact path of field hang 4214EAE4 --
    //
    // The user in the field never pressed Stop - the log shows them switching
    // sources twice while the driver was wedged, which reaches
    // Pipeline::setSource() on the GUI thread with the source thread still
    // parked in read(). The first bounded-join fix covered stop() alone, and
    // an adversarial review proved this path still carried the old unbounded
    // join; the shared quiesce helper is the fix, and this block is what keeps
    // it shared.
    {
        Pipeline::Config hungCfg = cfg;
        hungCfg.audioEnabled = false;

        Pipeline p(hungCfg);
        auto hungSrc = std::make_unique<HungReadSource>();
        HungReadSource* hung = hungSrc.get();  // outlives the swap: leaked below
        p.setSource(std::move(hungSrc));
        p.start();
        CHECK(p.running() == true);
        CHECK(p.abandonedSourceThreads() == 0);
        CHECK(waitUntil([&] { return hung->entered.load(); }));

        const auto t0 = steady_clock::now();
        p.setSource(std::make_unique<HealthySource>());
        const double swapMs =
            std::chrono::duration<double, std::milli>(steady_clock::now() - t0).count();
        std::printf("setSource() against a hung source (no stop): %.1f ms\n", swapMs);
        CHECK(swapMs < 6000.0);
        CHECK(p.abandonedSourceThreads() == 1);

        // run_ was never cleared (nobody called stop()), so setSource resumed
        // the NEW source itself - the swap must leave a live, streaming
        // pipeline, not a stopped one.
        CHECK(p.running() == true);
        SpectrumFrame f;
        CHECK(waitForNewFrame(p, f));

        const auto t1 = steady_clock::now();
        p.stop();
        const double stopMs2 =
            std::chrono::duration<double, std::milli>(steady_clock::now() - t1).count();
        CHECK(p.running() == false);
        CHECK(stopMs2 < 1000.0);
        CHECK(p.abandonedSourceThreads() == 1);  // the healthy session joined

        // Same harness-only unwind as the block above: the leaked source and
        // this stack frame must outlive the zombie's final instructions.
        hung->releaseForTest();
        CHECK(waitUntil([&] { return hung->returned.load(); }));
        std::this_thread::sleep_for(milliseconds(250));
    }

    (void)lastSeqSeen;
    return testSummary("test_pipeline");
}
