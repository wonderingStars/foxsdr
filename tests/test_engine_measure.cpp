// test_engine_measure.cpp - the arithmetic behind tools/measure_engine.ps1's
// tune-to-audio figure (core/engine_measure.hpp): the tone power, where the
// tone is, and the crossing scan that turns an audio tap into a latency.
//
// These are the parts of the measurement that can be wrong without anything
// looking wrong - a scan that judges a block twice, or skips one, or counts
// from the wrong end of the tap, still prints a plausible number of
// milliseconds. So each is pinned against a stream whose answer is known to
// the sample.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "core/engine_measure.hpp"
#include "test_check.hpp"

namespace m = cascade::core::measure;

namespace {

constexpr double kRate = 48000.0;
constexpr double kPi = 3.14159265358979323846;

// An absolute audio stream: silence before `toneStart`, a sine of amplitude
// `amp` at `hz` from it on. Absolute indices, as audioSamplesProduced counts.
float streamAt(std::uint64_t i, std::uint64_t toneStart, double hz, double amp) {
    if (i < toneStart) { return 0.0f; }
    return static_cast<float>(amp * std::sin(2.0 * kPi * hz * static_cast<double>(i) / kRate));
}

// The tap as Pipeline::audioTap returns it: the newest n samples ending just
// before absolute index `end`.
std::vector<float> tapEndingAt(std::uint64_t end, std::size_t n, std::uint64_t toneStart,
                               double hz, double amp) {
    std::vector<float> t(n);
    for (std::size_t k = 0; k < n; ++k) { t[k] = streamAt(end - n + k, toneStart, hz, amp); }
    return t;
}

void testTonePower() {
    // 1 kHz over 480 samples is exactly ten cycles: a unit sine's mean square.
    std::vector<float> x(480);
    for (std::size_t i = 0; i < x.size(); ++i) {
        x[i] = static_cast<float>(std::sin(2.0 * kPi * 1000.0 * static_cast<double>(i) / kRate));
    }
    CHECK_NEAR(m::tonePower(x.data(), x.size(), 1000.0, kRate), 0.5, 1e-4);
    // Power goes as the square of the amplitude.
    std::vector<float> half(x);
    for (float& v : half) { v *= 0.5f; }
    CHECK_NEAR(m::tonePower(half.data(), half.size(), 1000.0, kRate), 0.125, 1e-4);
    // Five bins away the sine contributes nothing.
    CHECK(m::tonePower(x.data(), x.size(), 1500.0, kRate) < 1e-4);
    // Degenerate inputs are zero, not a division by zero.
    CHECK(m::tonePower(x.data(), 0, 1000.0, kRate) == 0.0);
    CHECK(m::tonePower(nullptr, 10, 1000.0, kRate) == 0.0);
    CHECK(m::tonePower(x.data(), x.size(), 1000.0, 0.0) == 0.0);
}

void testDominantFrequency() {
    std::vector<float> x(4096);
    for (std::size_t i = 0; i < x.size(); ++i) {
        x[i] = static_cast<float>(0.3 * std::sin(2.0 * kPi * 1235.0 * static_cast<double>(i) / kRate));
    }
    CHECK_NEAR(m::dominantFrequencyHz(x.data(), x.size(), kRate, 200.0, 3500.0, 5.0), 1235.0, 0.1);
    // A backwards range answers its low end rather than looping.
    CHECK(m::dominantFrequencyHz(x.data(), x.size(), kRate, 500.0, 100.0, 5.0) == 500.0);
}

void testCrossingInOneWindow() {
    // Command at 5000; the tone appears 1000 samples later. Blocks of 256 are
    // laid from the command: [5000,5256) ... block 3 is [5768,6024), which
    // holds only 24 samples of tone; block 4, [6024,6280), is the first that
    // is all tone. Its END is the answer.
    const std::uint64_t cmd = 5000, toneStart = 6000;
    const std::uint64_t end = 9000;
    const std::vector<float> tap = tapEndingAt(end, 4096, toneStart, 1000.0, 1.0);
    std::uint64_t next = cmd;
    bool missed = false;
    const std::int64_t cross =
        m::scanForCrossing(tap.data(), tap.size(), end, next, 256, 1000.0, kRate, 0.25, missed);
    CHECK(cross == 6280);
    CHECK(next == 6280);  // judged up to the crossing, not beyond
    CHECK(!missed);
}

void testCrossingAcrossReads() {
    // The same stream read a frame at a time: the first read ends before the
    // tone, and must judge only COMPLETE blocks and leave the cursor on the
    // first unjudged one; the second finds the crossing. The answer must not
    // depend on where the reads fell.
    const std::uint64_t cmd = 5000, toneStart = 6000;
    std::uint64_t next = cmd;
    bool missed = false;
    const std::vector<float> a = tapEndingAt(5900, 4096, toneStart, 1000.0, 1.0);
    CHECK(m::scanForCrossing(a.data(), a.size(), 5900, next, 256, 1000.0, kRate, 0.25, missed) == -1);
    CHECK(next == 5768);  // [5768,6024) is not complete at 5900: left for the next read
    const std::vector<float> b = tapEndingAt(7000, 4096, toneStart, 1000.0, 1.0);
    CHECK(m::scanForCrossing(b.data(), b.size(), 7000, next, 256, 1000.0, kRate, 0.25, missed) == 6280);
    CHECK(!missed);
}

void testQuietNeverCrosses() {
    // A tone at a tenth of the amplitude is a hundredth of the power: never
    // half of the steady value however long it is watched.
    const std::uint64_t cmd = 100;
    std::uint64_t next = cmd;
    bool missed = false;
    const std::vector<float> t = tapEndingAt(4196, 4096, 0, 1000.0, 0.1);
    CHECK(m::scanForCrossing(t.data(), t.size(), 4196, next, 256, 1000.0, kRate, 0.25, missed) == -1);
    CHECK(next == 100 + 16 * 256);  // every complete block judged exactly once
    CHECK(!missed);
}

void testMissedWhenTheTapHasMoved() {
    // The reader fell behind: the tap now starts 1000 samples after the
    // command, so the first blocks can never be judged. That must be SAID -
    // a latency measured from a later block would read short.
    const std::uint64_t cmd = 5000, toneStart = 20000;
    const std::uint64_t end = 5000 + 1000 + 4096;
    std::uint64_t next = cmd;
    bool missed = false;
    const std::vector<float> t = tapEndingAt(end, 4096, toneStart, 1000.0, 1.0);
    CHECK(m::scanForCrossing(t.data(), t.size(), end, next, 256, 1000.0, kRate, 0.25, missed) == -1);
    CHECK(missed);
    // A tap shorter than its own end index claims is refused outright.
    std::uint64_t n2 = 0;
    bool m2 = false;
    CHECK(m::scanForCrossing(t.data(), t.size(), 100, n2, 256, 1000.0, kRate, 0.25, m2) == -1);
    CHECK(m::scanForCrossing(t.data(), t.size(), end, n2, 0, 1000.0, kRate, 0.25, m2) == -1);
}

}  // namespace

int main() {
    testTonePower();
    testDominantFrequency();
    testCrossingInOneWindow();
    testCrossingAcrossReads();
    testQuietNeverCrosses();
    testMissedWhenTheTapHasMoved();
    return testSummary("test_engine_measure");
}
