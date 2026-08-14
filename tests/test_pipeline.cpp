// Tests for core/pipeline.hpp — the threaded render pipeline.
//
// Every wait loop is deadline-bounded on steady_clock (<= 30 s) so a liveness
// bug fails the test instead of hanging it; ctest's 120 s per-test timeout is
// the backstop for a hung join inside stop()/~Pipeline. Expected peak bins are
// computed in-test from the configured tone frequency and the documented
// fftshift layout (DC at fftSize/2), never from implementation output.
//
// SPDX-License-Identifier: MIT
#include "core/pipeline.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

#include "test_check.hpp"

using cascade::core::Pipeline;
using cascade::core::SpectrumFrame;
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

    (void)lastSeqSeen;
    return testSummary("test_pipeline");
}
