/*
 * THE DEMOD SCOPE'S TWO TAPS ARE ACTUALLY CONNECTED TO THE PIPELINE.
 *
 * WHY THIS IS A SEPARATE FILE FROM tests/test_scope_tap.cpp. That one proves
 * the RING is correct - ordering, overwrite, the tear check - against a writer
 * the test itself drives. It would pass just as happily if nothing in the
 * product ever pushed a sample into one, which is the whole failure this file
 * exists to catch: the page drew a graticule and the words "NO SAMPLES -
 * RECEIVER STOPPED" over a receiver that was plainly running, and neither the
 * ring's own suite nor 114 other tests could tell the difference between a
 * broken ring and an unconnected one.
 *
 * SO WHAT IS MEASURED HERE IS THE WIRING, and only that: run the real chain
 * off the built-in generator and require BOTH counters to advance and BOTH
 * windows to come back full of finite samples. It is the same "a unit test is
 * not a wiring test" argument tests/test_pipeline_audio.cpp opens with.
 *
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <thread>
#include <vector>

#include "core/pipeline.hpp"
#include "dsp/demod.hpp"
#include "test_check.hpp"

namespace {

// Poll until `pred` or the deadline. A stalled chain has to fail the suite,
// not hang it.
template <typename F>
bool waitFor(F pred, int ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return pred();
}

void testTapsAdvance() {
    cascade::core::Pipeline::Config cfg;
    cfg.sampleRateHz = 2000000.0;
    cfg.fftSize = 1024;
    cfg.audioEnabled = false;  // headless: the taps are what get measured
    cascade::core::Pipeline pipeline(cfg);
    pipeline.setDemodMode(cascade::dsp::DemodMode::WFM);
    pipeline.setSquelchDb(-120.0f);  // in the path, never gating

    // NOTHING HAS BEEN WRITTEN BEFORE start(), and that matters as much as the
    // rest: a counter that began somewhere other than zero would make the
    // page's "has anything arrived since the last frame" test answer by
    // accident on its first frame.
    CHECK(pipeline.scopeAudio().written() == 0);
    CHECK(pipeline.scopeIq().written() == 0);

    // A carrier 40 kHz off centre, well inside the channel the VFO keeps, at
    // full scale: the taps have to carry something a peak measurement can see.
    pipeline.sigGen().setTone(0, 40000.0, 0.0f);
    pipeline.sigGen().setNoiseFloorDb(-300.0f);
    pipeline.start();

    CHECK(waitFor([&] { return pipeline.scopeIq().written() > 0; }, 15000));
    CHECK(waitFor([&] { return pipeline.scopeAudio().written() > 4096; }, 15000));

    // AND THEY KEEP ADVANCING. One nonzero reading could be a single block
    // pushed during start-up; what the page actually asks is "did anything
    // arrive since the last frame", so the counters have to move again.
    const std::uint64_t a0 = pipeline.scopeAudio().written();
    const std::uint64_t i0 = pipeline.scopeIq().written();
    CHECK(waitFor([&] { return pipeline.scopeAudio().written() > a0; }, 15000));
    CHECK(waitFor([&] { return pipeline.scopeIq().written() > i0; }, 15000));

    // THE SAMPLES ARE REAL. A tap that advanced while handing back silence
    // would draw a flat line under a caption claiming a measurement.
    std::vector<float> audio(4096, 0.0f);
    const std::size_t got = pipeline.scopeAudio().snapshot(audio.data(), audio.size());
    CHECK(got == audio.size());
    float peak = 0.0f;
    int nonFinite = 0;
    for (std::size_t i = 0; i < got; ++i) {
        if (!std::isfinite(audio[i])) { ++nonFinite; }
        const float a = std::fabs(audio[i]);
        if (a > peak) { peak = a; }
    }
    CHECK(nonFinite == 0);
    CHECK(peak > 0.0f);

    std::vector<std::complex<float>> iq(4096);
    const std::size_t gotIq = pipeline.scopeIq().snapshot(iq.data(), iq.size());
    CHECK(gotIq == iq.size());
    float iqPeak = 0.0f;
    int iqNonFinite = 0;
    for (std::size_t i = 0; i < gotIq; ++i) {
        if (!std::isfinite(iq[i].real()) || !std::isfinite(iq[i].imag())) { ++iqNonFinite; }
        const float m = std::abs(iq[i]);
        if (m > iqPeak) { iqPeak = m; }
    }
    CHECK(iqNonFinite == 0);
    CHECK(iqPeak > 0.0f);

    pipeline.stop();

    // STOPPED MEANS STOPPED. The counters freeze, which is exactly what the
    // page reads to letter "NO SAMPLES - RECEIVER STOPPED" on the glass rather
    // than drawing a flat trace at zero volts.
    const std::uint64_t a1 = pipeline.scopeAudio().written();
    const std::uint64_t i1 = pipeline.scopeIq().written();
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    CHECK(pipeline.scopeAudio().written() == a1);
    CHECK(pipeline.scopeIq().written() == i1);
}

}  // namespace

int main() {
    testTapsAdvance();
    return testSummary("test_scope_taps_wired");
}
