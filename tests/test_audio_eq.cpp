// Audio equaliser and high-pass / low-pass (src/dsp/audio_eq.hpp). Generated tones only; no recording.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <cstdio>
#include <functional>
#include <limits>

#include "alloc_probe.hpp"
#include "blk4_util.hpp"
#include "dsp/audio_eq.hpp"
#include "test_check.hpp"

using cascade::dsp::AudioEq;
using cascade::dsp::AudioHighPass;
using cascade::dsp::AudioLowPass;

namespace {
constexpr double kFs = 48000.0;

// Steady-state gain in dB at `hz` of an equaliser set up by `setup`.
double gainAt(const std::function<void(AudioEq&)>& setup, double hz) {
    AudioEq eq(kFs);
    setup(eq);
    std::vector<float> x = blk4::tone(72000, kFs, hz, 0.1);
    eq.process(x.data(), x.size());
    return blk4::db(blk4::toneAmp(x, 36000, 72000, kFs, hz) / 0.1);
}

void testResponse() {
    // Tone controls: shelf asymptotes and the peak centre.
    const double bassLow = gainAt([](AudioEq& e) { e.setBassDb(9); }, 25.0);
    const double bassHigh = gainAt([](AudioEq& e) { e.setBassDb(9); }, 3000.0);
    const double bassMid = gainAt([](AudioEq& e) { e.setBassDb(9); }, 150.0);
    const double mid = gainAt([](AudioEq& e) { e.setMidDb(6); }, 1000.0);
    const double midFar = gainAt([](AudioEq& e) { e.setMidDb(6); }, 100.0);
    const double trebHigh = gainAt([](AudioEq& e) { e.setTrebleDb(-9); }, 12000.0);
    const double trebLow = gainAt([](AudioEq& e) { e.setTrebleDb(-9); }, 200.0);
    std::printf("MEAS bass +9: 25 Hz %.2f dB, 150 Hz %.2f dB, 3 kHz %.2f dB; mid +6 at 1 kHz %.2f dB (100 Hz %.2f); treble -9: 12 kHz %.2f dB, 200 Hz %.2f dB\n",
                bassLow, bassMid, bassHigh, mid, midFar, trebHigh, trebLow);
    CHECK_NEAR(bassLow, 9.0, 0.3);
    CHECK_NEAR(bassMid, 4.5, 0.3);  // a shelf is half its gain (in dB) at its corner
    CHECK(std::fabs(bassHigh) < 0.3);
    CHECK_NEAR(mid, 6.0, 0.1);
    CHECK(std::fabs(midFar) < 0.4);
    CHECK_NEAR(trebHigh, -9.0, 0.4);
    CHECK(std::fabs(trebLow) < 0.3);
    // Graphic: each band at its own centre, boost and cut, the others flat.
    for (int b = 0; b < AudioEq::kGraphic; ++b) {
        const double hz = AudioEq::graphicHz(b);
        const double up = gainAt([b](AudioEq& e) { e.setGraphicDb(b, 12); }, hz);
        const double dn = gainAt([b](AudioEq& e) { e.setGraphicDb(b, -12); }, hz);
        std::printf("MEAS graphic band %d (%.0f Hz): +12 -> %.2f dB, -12 -> %.2f dB\n", b, hz, up, dn);
        CHECK_NEAR(up, 12.0, 0.15);
        CHECK_NEAR(dn, -12.0, 0.15);
    }
    // The gain one octave away is well below the band's (the bands do not smear into each other).
    const double off = gainAt([](AudioEq& e) { e.setGraphicDb(2, 12); }, 4000.0);
    std::printf("MEAS graphic 1 kHz band +12 dB seen at 4 kHz: %.2f dB\n", off);
    CHECK(off < 2.0);
    // A cut and a boost of the same size cancel (series of two bands): flat within a fraction of a dB.
    const double flat = gainAt([](AudioEq& e) { e.setMidDb(8); e.setGraphicDb(2, -8); }, 1000.0);
    CHECK(std::fabs(flat) < 0.6);
}

void testHighLowPass() {
    for (auto c : {AudioHighPass::Hz100, AudioHighPass::Hz300}) {
        const double fc = static_cast<double>(static_cast<int>(c));
        const double atFc = gainAt([c](AudioEq& e) { e.setHighPass(c); }, fc);
        const double inband = gainAt([c](AudioEq& e) { e.setHighPass(c); }, 1500.0);
        const double third = gainAt([c](AudioEq& e) { e.setHighPass(c); }, fc / 3.0);
        std::printf("MEAS high-pass %.0f Hz: at corner %.2f dB, 1.5 kHz %.2f dB, corner/3 %.2f dB\n", fc, atFc, inband, third);
        CHECK_NEAR(atFc, -3.0, 0.3);
        CHECK(std::fabs(inband) < 0.1);
        CHECK(third < -30.0);  // 4th order: 24 dB per octave
    }
    for (auto c : {AudioLowPass::Hz2400, AudioLowPass::Hz3000, AudioLowPass::Hz6000}) {
        const double fc = static_cast<double>(static_cast<int>(c));
        const double atFc = gainAt([c](AudioEq& e) { e.setLowPass(c); }, fc);
        const double inband = gainAt([c](AudioEq& e) { e.setLowPass(c); }, 300.0);
        const double twice = gainAt([c](AudioEq& e) { e.setLowPass(c); }, std::min(2.0 * fc, 0.45 * kFs));
        std::printf("MEAS low-pass %.0f Hz: at corner %.2f dB, 300 Hz %.2f dB, 2x corner %.2f dB\n", fc, atFc, inband, twice);
        CHECK_NEAR(atFc, -3.0, 0.3);
        CHECK(std::fabs(inband) < 0.1);
        CHECK(twice < -20.0);
    }
}

void testBypassAndCost() {
    std::vector<float> x = blk4::tone(9600, kFs, 700.0, 0.3);
    std::vector<float> y = x;
    AudioEq eq(kFs);
    eq.process(y.data(), y.size());
    CHECK(eq.isBypassed());
    CHECK(x == y);  // bit-exact
    // Switched on and off again: settles to the exact bypass.
    eq.setMidDb(10);
    eq.process(y.data(), y.size());
    CHECK(!eq.isBypassed());
    eq.setMidDb(0);
    std::vector<float> z = blk4::tone(48000, kFs, 700.0, 0.3);
    eq.process(z.data(), z.size());
    CHECK(eq.isBypassed());
    std::vector<float> w = x;
    eq.process(w.data(), w.size());
    CHECK(x == w);
    // Cost: off against everything on (a measurement, not a pin beyond "off is far cheaper").
    std::vector<float> big = blk4::tone(480000, kFs, 700.0, 0.3);
    AudioEq off(kFs);
    auto t0 = std::chrono::steady_clock::now();
    off.process(big.data(), big.size());
    auto t1 = std::chrono::steady_clock::now();
    AudioEq on(kFs);
    on.setBassDb(3); on.setMidDb(3); on.setTrebleDb(3);
    for (int b = 0; b < 5; ++b) { on.setGraphicDb(b, 2); }
    on.setHighPass(AudioHighPass::Hz300);
    on.setLowPass(AudioLowPass::Hz3000);
    auto t2 = std::chrono::steady_clock::now();
    on.process(big.data(), big.size());
    auto t3 = std::chrono::steady_clock::now();
    const double nsOff = std::chrono::duration<double, std::nano>(t1 - t0).count() / 480000.0;
    const double nsOn = std::chrono::duration<double, std::nano>(t3 - t2).count() / 480000.0;
    std::printf("MEAS cost per sample: off %.3f ns, all eight bands + both filters %.1f ns\n", nsOff, nsOn);
    CHECK(nsOff * 20.0 < nsOn);
}

void testAllocation() {
    AudioEq eq(kFs);
    std::vector<float> x = blk4::tone(480, kFs, 700.0, 0.3);
    long news = 0;
    for (int i = 0; i < 40; ++i) {
        {
            AllocProbe probe;
            eq.setMidDb((i % 2) ? 6.0 : -6.0);
            eq.setBassDb(i % 5);
            eq.setGraphicDb(i % 5, 3);
            eq.setHighPass((i % 3 == 0) ? AudioHighPass::Hz100 : AudioHighPass::Hz300);
            eq.setLowPass((i % 4 == 0) ? AudioLowPass::Off : AudioLowPass::Hz3000);
            eq.process(x.data(), x.size());
            news += probe.news();
        }
    }
    std::printf("MEAS operator new in 40 process calls with setting changes: %ld\n", news);
    CHECK(news == 0);
}

void testClicks() {
    const double amp = 0.25;
    // Mid +12 dB switched on under a steady 1 kHz tone: no step larger than the final tone's own steepest slope.
    {
        AudioEq eq(kFs);
        std::vector<float> x = blk4::tone(48000, kFs, 1000.0, amp);
        eq.process(x.data(), 24000);
        eq.setMidDb(12);
        eq.process(x.data() + 24000, 24000);
        const double own = amp * 4.0 * blk4::kTwoPi * 1000.0 / kFs;
        const double step = blk4::maxStep(x, 24000, 26400);
        std::printf("MEAS mid 0 -> +12 dB under a 1 kHz tone: largest step %.4f, final tone's own %.4f (ratio %.3f)\n", step, own, step / own);
        CHECK(step <= own * 1.02);
    }
    // The hard case: mid +12 dB straight to -12 dB (and back) under a tone on the peak, treble across the same range at 3.5 kHz.
    for (double hz : {1000.0, 3500.0}) {
        AudioEq eq(kFs);
        std::vector<float> x = blk4::tone(60000, kFs, hz, 0.2);
        const bool mid = hz < 2000.0;
        auto set = [&](double db) { if (mid) { eq.setMidDb(db); } else { eq.setTrebleDb(db); } };
        set(12);
        eq.process(x.data(), 20000);
        set(-12);
        eq.process(x.data() + 20000, 20000);
        set(12);
        eq.process(x.data() + 40000, 20000);
        const double own = 0.2 * (mid ? 4.0 : 4.0) * blk4::kTwoPi * hz / kFs;  // steepest slope of the loudest state
        const double step = blk4::maxStep(x, 18000, 60000);
        std::printf("MEAS %s +12 <-> -12 dB under a %.0f Hz tone: largest step %.4f, loudest tone's own %.4f (ratio %.3f)\n",
                    mid ? "mid" : "treble", hz, step, own, step / own);
        CHECK(step <= own * 1.02);
    }
    // High-pass switched on, then to another cutoff, then off, under a 1 kHz tone.
    {
        AudioEq eq(kFs);
        std::vector<float> x = blk4::tone(96000, kFs, 1000.0, 0.5);
        const double own = 0.5 * blk4::kTwoPi * 1000.0 / kFs;
        eq.process(x.data(), 12000);
        eq.setHighPass(AudioHighPass::Hz300);
        eq.process(x.data() + 12000, 24000);
        eq.setHighPass(AudioHighPass::Hz100);
        eq.process(x.data() + 36000, 24000);
        eq.setHighPass(AudioHighPass::Off);
        eq.process(x.data() + 60000, 36000);
        const double step = blk4::maxStep(x, 12000, 96000);
        std::printf("MEAS high-pass Off/300/100/Off under a 1 kHz tone: largest step %.4f against the tone's %.4f (ratio %.3f)\n", step, own, step / own);
        CHECK(step <= own * 1.05);
    }
    // Low-pass switched in under a 2.5 kHz tone (the output falls to a tenth): steps stay within the original tone's.
    {
        AudioEq eq(kFs);
        std::vector<float> x = blk4::tone(48000, kFs, 2500.0, 0.5);
        const double own = 0.5 * blk4::kTwoPi * 2500.0 / kFs;
        eq.process(x.data(), 12000);
        eq.setLowPass(AudioLowPass::Hz2400);
        eq.process(x.data() + 12000, 36000);
        const double step = blk4::maxStep(x, 12000, 48000);
        std::printf("MEAS low-pass in under a 2.5 kHz tone: largest step %.4f against %.4f (ratio %.3f)\n", step, own, step / own);
        CHECK(step <= own * 1.05);
    }
}

void testRobustness() {
    // Any chunking gives the same bits.
    std::vector<float> in = blk4::tone(20000, kFs, 900.0, 0.3);
    auto make = [](AudioEq& e) {
        e.setBassDb(5); e.setMidDb(-4); e.setTrebleDb(3); e.setGraphicDb(1, 6);
        e.setHighPass(AudioHighPass::Hz100); e.setLowPass(AudioLowPass::Hz6000);
    };
    AudioEq a(kFs);
    make(a);
    std::vector<float> whole = in;
    a.process(whole.data(), whole.size());
    for (std::size_t blk : {1u, 7u, 64u, 333u}) {
        AudioEq b(kFs);
        make(b);
        std::vector<float> o = blk4::runBlocks([&](float* p, std::size_t n) { b.process(p, n); }, in, blk);
        CHECK(o == whole);
    }
    // Non-finite input reads as 0 and does not poison the state.
    AudioEq eq(kFs);
    make(eq);
    std::vector<float> x = in;
    x[100] = std::numeric_limits<float>::quiet_NaN();
    x[101] = std::numeric_limits<float>::infinity();
    eq.process(x.data(), x.size());
    bool finite = true;
    for (float v : x) { finite = finite && std::isfinite(v); }
    CHECK(finite);
    // Out-of-range and non-finite gains: clamped, ignored.
    CHECK_NEAR(gainAt([](AudioEq& e) { e.setMidDb(100); }, 1000.0), 12.0, 0.2);
    CHECK_NEAR(gainAt([](AudioEq& e) { e.setMidDb(6); e.setMidDb(std::nan("")); }, 1000.0), 6.0, 0.1);
    CHECK_NEAR(gainAt([](AudioEq& e) { e.setGraphicDb(9, 12); e.setGraphicDb(-1, 12); }, 1000.0), 0.0, 1e-6);
    // Empty block.
    AudioEq e0(kFs);
    e0.setMidDb(3);
    e0.process(nullptr, 0);
    CHECK(true);
    // Other rates.
    AudioEq e96(96000.0);
    e96.setMidDb(6);
    std::vector<float> t = blk4::tone(96000, 96000.0, 1000.0, 0.1);
    e96.process(t.data(), t.size());
    CHECK_NEAR(blk4::db(blk4::toneAmp(t, 48000, 96000, 96000.0, 1000.0) / 0.1), 6.0, 0.15);
}
}  // namespace

int main() {
    testResponse();
    testHighLowPass();
    testBypassAndCost();
    testAllocation();
    testClicks();
    testRobustness();
    return testSummary("test_audio_eq");
}
