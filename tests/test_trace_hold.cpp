// Tests for core/trace_hold.hpp - the spectrum's peak hold and average.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/trace_hold.hpp"

#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

#include "test_check.hpp"

using cascade::core::TraceHold;
using cascade::core::TraceMode;

namespace {

void testNormalKeepsNothing() {
    std::printf("  normal keeps nothing\n");
    TraceHold h;
    const float f[3] = {-50.0f, -40.0f, -30.0f};
    h.push(f, 3, 0.0, 1);
    CHECK(h.trace().empty());
    CHECK(h.frames() == 0u);
}

void testPeak() {
    std::printf("  peak holds each bin's largest value until reset\n");
    TraceHold h;
    h.setMode(TraceMode::Peak);
    const float a[3] = {-50.0f, -20.0f, -60.0f};
    const float b[3] = {-40.0f, -70.0f, -65.0f};
    h.push(a, 3, 0.0, 7);
    h.push(b, 3, 0.1, 7);
    CHECK(h.trace().size() == 3u);
    if (h.trace().size() == 3u) {
        CHECK(h.trace()[0] == -40.0f);  // the later, higher value
        CHECK(h.trace()[1] == -20.0f);  // the burst seen once stays
        CHECK(h.trace()[2] == -60.0f);
    }
    CHECK(h.frames() == 2u);
    // A non-finite bin is the floor, never a peak.
    const float bad[3] = {std::numeric_limits<float>::infinity(), std::nanf(""), -10.0f};
    h.push(bad, 3, 0.2, 7);
    CHECK(h.trace()[0] == -40.0f && h.trace()[1] == -20.0f && h.trace()[2] == -10.0f);
    h.reset();
    CHECK(h.trace().empty());
    h.push(b, 3, 0.3, 7);
    CHECK(h.trace()[1] == -70.0f);  // started over
}

void testKeyAndSize() {
    std::printf("  a new axis (retune, new rate, new bin count) starts the trace again\n");
    TraceHold h;
    h.setMode(TraceMode::Peak);
    const float hi[2] = {-10.0f, -10.0f};
    const float lo[2] = {-80.0f, -80.0f};
    h.push(hi, 2, 0.0, 1);
    h.push(lo, 2, 0.1, 2);  // retuned
    CHECK(h.trace().size() == 2u && h.trace()[0] == -80.0f);
    const float three[3] = {-5.0f, -5.0f, -5.0f};
    h.push(three, 3, 0.2, 2);  // same key, another size
    CHECK(h.trace().size() == 3u && h.trace()[0] == -5.0f);
    // Another mode starts again too.
    h.setMode(TraceMode::Average);
    CHECK(h.trace().empty());
}

void testAverageIsPowerOverAWindow() {
    std::printf("  average: power, not decibels, over exactly the last N ms\n");
    TraceHold h;
    h.setMode(TraceMode::Average);
    h.setAverageMs(1000.0);
    // Two frames, 0 dB and -inf-ish (-200 dB): the power mean is half of 1,
    // i.e. -3.01 dB - NOT the -100 dB a mean of the decibels would draw.
    const float zero[1] = {0.0f};
    const float none[1] = {-200.0f};
    h.push(zero, 1, 0.0, 1);
    h.push(none, 1, 0.1, 1);
    CHECK(h.frames() == 2u);
    CHECK_NEAR(h.trace()[0], -3.0103f, 0.001);
    // 1.5 s later only the newest frame is inside a 1 s window.
    const float ten[1] = {-10.0f};
    h.push(ten, 1, 1.6, 1);
    CHECK(h.frames() == 1u);
    CHECK_NEAR(h.trace()[0], -10.0f, 1e-4);
    // Frames 250 ms apart over 1 s: five of them count (0, .25, .5, .75, 1.0).
    TraceHold w;
    w.setMode(TraceMode::Average);
    w.setAverageMs(1000.0);
    for (int i = 0; i <= 8; ++i) {
        const float v[1] = {static_cast<float>(-i)};
        w.push(v, 1, 0.25 * i, 3);
    }
    CHECK(w.frames() == 5u);
    CHECK_NEAR(w.heldSeconds(), 1.0, 1e-9);
    // Shortening the window drops frames, without starting over.
    w.setAverageMs(500.0);
    CHECK(w.frames() == 3u);
    CHECK(!w.trace().empty());
    // Out-of-range lengths are clamped.
    w.setAverageMs(1.0);
    CHECK(w.averageMs() == cascade::core::kTraceAverageMinMs);
    w.setAverageMs(1.0e9);
    CHECK(w.averageMs() == cascade::core::kTraceAverageMaxMs);
}

void testWindowCapAndDrift() {
    std::printf("  the window is capped in frames, and the running sum does not drift\n");
    TraceHold h;
    h.setMode(TraceMode::Average);
    h.setAverageMs(cascade::core::kTraceAverageMaxMs);
    const float v[2] = {-30.0f, -90.0f};
    for (int i = 0; i < 5000; ++i) { h.push(v, 2, i * 0.001, 1); }
    CHECK(h.frames() == cascade::core::kTraceHoldMaxFrames);
    CHECK_NEAR(h.trace()[0], -30.0f, 1e-3);
    CHECK_NEAR(h.trace()[1], -90.0f, 1e-3);
}

void testNames() {
    std::printf("  the config's names round-trip; anything unknown is normal\n");
    using cascade::core::traceModeFromName;
    using cascade::core::traceModeName;
    for (TraceMode m : {TraceMode::Normal, TraceMode::Peak, TraceMode::Average}) {
        CHECK(traceModeFromName(traceModeName(m)) == m);
    }
    CHECK(traceModeFromName("sideways") == TraceMode::Normal);
    CHECK(cascade::core::clampTraceAverageMs(std::nan("")) == cascade::core::kTraceAverageDefaultMs);
}

}  // namespace

int main() {
    testNormalKeepsNothing();
    testPeak();
    testKeyAndSize();
    testAverageIsPowerOverAWindow();
    testWindowCapAndDrift();
    testNames();
    return testSummary("test_trace_hold");
}
