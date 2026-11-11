// Tests for the notch bank (dsp/notch_bank.hpp): depth at the centre, flatness away from it, two close notches, ten at once, moving /
// switching a notch while audio runs without a click, the zero-cost path, chunk invariance, allocation, and a control thread hammering the
// settings while the DSP thread runs.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "dsp/notch_bank.hpp"

#include "alloc_probe.hpp"
#include "signal_block_gen.hpp"
#include "test_check.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
#include <thread>

using cascade::dsp::NotchBank;

namespace {

constexpr double kFs = 48000.0;

std::vector<float> sine(double hz, double amp, std::size_t n, std::size_t from = 0) {
    std::vector<float> v(n);
    for (std::size_t i = 0; i < n; ++i) { v[i] = static_cast<float>(amp * std::sin(sbgen::kTwoPi * hz * static_cast<double>(i + from) / kFs)); }
    return v;
}

double rms(const float* x, std::size_t n) {
    double s = 0;
    for (std::size_t i = 0; i < n; ++i) { s += static_cast<double>(x[i]) * x[i]; }
    return std::sqrt(s / static_cast<double>(std::max<std::size_t>(n, 1)));
}

// Gain in dB of the bank for a steady tone: run 3 s, measure the last second.
double gainDb(NotchBank& b, double hz) {
    std::vector<float> x = sine(hz, 0.5, 3 * 48000);
    const std::vector<float> ref = x;
    b.process(x.data(), x.size());
    const std::size_t tail = 48000;
    return 20.0 * std::log10(std::max(rms(x.data() + x.size() - tail, tail), 1e-30) / rms(ref.data() + ref.size() - tail, tail));
}

double maxStep(const std::vector<float>& v, std::size_t from, std::size_t to) {
    double m = 0;
    for (std::size_t i = std::max<std::size_t>(from, 1); i < to && i < v.size(); ++i) { m = std::max(m, std::fabs(static_cast<double>(v[i]) - v[i - 1])); }
    return m;
}

void testDepthAndFlatness() {
    double worstDepth = 1e9;
    for (double f0 : {300.0, 1000.0, 3000.0, 10000.0}) {
        for (double w : {10.0, 50.0, 200.0}) {
            NotchBank b(kFs);
            b.setNotch(0, f0, w, true);
            const double d = gainDb(b, f0);
            worstDepth = std::min(worstDepth, -d);
            CHECK(d < -100.0);
            // three widths away on both sides: under 1 dB (measured: about 0.1 dB)
            for (double sgn : {-1.0, 1.0}) {
                const double fp = f0 + sgn * 3.0 * w;
                if (fp < 50.0 || fp > 0.4 * kFs) { continue; }  // the probe tone must exist (a negative frequency aliases onto the notch)
                NotchBank c(kFs);
                c.setNotch(0, f0, w, true);
                const double g = gainDb(c, fp);
                CHECK(g > -1.0 && g < 0.05);
            }
        }
    }
    std::printf("notch depth at centre: worst %.1f dB over 12 settings\n", worstDepth);
    // bandwidth: the -3 dB points are width apart
    NotchBank b(kFs);
    b.setNotch(0, 1000.0, 100.0, true);
    NotchBank lo(kFs), hi(kFs);
    lo.setNotch(0, 1000.0, 100.0, true);
    hi.setNotch(0, 1000.0, 100.0, true);
    const double gl = gainDb(lo, 950.0), gh = gainDb(hi, 1050.0);
    std::printf("-3 dB check: gain at f0-w/2 %.2f dB, f0+w/2 %.2f dB\n", gl, gh);
    CHECK_NEAR(gl, -3.0, 0.3);
    CHECK_NEAR(gh, -3.0, 0.3);
    // a notch that is off does nothing, bit for bit
    NotchBank off(kFs);
    off.setNotch(2, 1000.0, 50.0, false);
    std::vector<float> x = sine(1000, 0.5, 4800), y = x;
    off.process(y.data(), y.size());
    CHECK(std::memcmp(x.data(), y.data(), x.size() * sizeof(float)) == 0);
}

void testCloseNotches() {
    NotchBank b(kFs);
    b.setNotch(0, 1000.0, 40.0, true);
    b.setNotch(1, 1100.0, 40.0, true);
    CHECK(b.activeCount() == 2);
    NotchBank c1 = NotchBank(kFs);
    // each centre is nulled by the pair (a notch passes the other's null almost untouched; both together hold)
    const double d0 = gainDb(b, 1000.0);
    NotchBank b2(kFs);
    b2.setNotch(0, 1000.0, 40.0, true);
    b2.setNotch(1, 1100.0, 40.0, true);
    const double d1 = gainDb(b2, 1100.0);
    NotchBank b3(kFs);
    b3.setNotch(0, 1000.0, 40.0, true);
    b3.setNotch(1, 1100.0, 40.0, true);
    const double mid = gainDb(b3, 1050.0);
    NotchBank b4(kFs);
    b4.setNotch(0, 1000.0, 40.0, true);
    b4.setNotch(1, 1100.0, 40.0, true);
    const double away = gainDb(b4, 1400.0);
    std::printf("two notches 1000 and 1100 Hz, width 40: centre1 %.1f dB, centre2 %.1f dB, between %.1f dB, 1400 Hz %.2f dB\n", d0, d1, mid, away);
    CHECK(d0 < -40.0 && d1 < -40.0);
    CHECK(mid > -12.0 && mid < -1.0);  // the skirts overlap: said, not hidden
    CHECK(away > -0.5);
    // the skirts overlap only a little at 1000 / 1300
    (void)c1;
}

void testAllTen() {
    NotchBank b(kFs);
    const double f[10] = {300, 600, 900, 1200, 1500, 1800, 2100, 2400, 2700, 3000};
    for (int i = 0; i < 10; ++i) { CHECK(b.setNotch(i, f[i], 30.0, true)); }
    CHECK(b.activeCount() == 10);
    // one pass with all ten: probe each centre and each midpoint with its own copy
    double worst = 1e9, worstMid = 0;
    for (int i = 0; i < 10; ++i) {
        NotchBank c(kFs);
        for (int j = 0; j < 10; ++j) { c.setNotch(j, f[j], 30.0, true); }
        worst = std::min(worst, -gainDb(c, f[i]));
        NotchBank m(kFs);
        for (int j = 0; j < 10; ++j) { m.setNotch(j, f[j], 30.0, true); }
        worstMid = std::min(worstMid, gainDb(m, f[i] + 150.0));
    }
    std::printf("ten notches width 30 Hz: shallowest centre %.1f dB, worst midpoint %.2f dB\n", -worst, worstMid);
    CHECK(worst > 40.0);
    CHECK(worstMid > -0.5);
    // broadband out of band untouched
    NotchBank c(kFs);
    for (int j = 0; j < 10; ++j) { c.setNotch(j, f[j], 30.0, true); }
    CHECK(gainDb(c, 6000.0) > -0.1);
    CHECK(gainDb(c, 100.0) > -0.3);
}

// A steady tone far from the notch while the notch is dragged around, switched on and off: no sample step bigger than the tone's own.
void testNoClick() {
    const double tone = 2500.0;
    const std::size_t N = 48000 * 4;
    const std::vector<float> in = sine(tone, 0.5, N);
    const double ownStep = maxStep(in, 0, N);
    // 1. moved between 300 and 1900 Hz (below the 2.5 kHz tone), width 10 Hz, every 20 ms
    {
        NotchBank b(kFs);
        b.setNotch(0, 300.0, 30.0, true);
        std::vector<float> y = in;
        std::size_t pos = 0;
        int k = 0;
        while (pos < N) {
            b.setNotch(0, 300.0 + 1600.0 * ((k++ % 2) != 0 ? 1.0 : 0.0), 10.0, true);
            const std::size_t m = std::min<std::size_t>(960, N - pos);
            b.process(y.data() + pos, m);
            pos += m;
        }
        const double step = maxStep(y, 24000, N);  // after the switch-on fade has finished
        std::printf("click check, moved notch: largest step %.5f against the tone's own %.5f (ratio %.3f)\n", step, ownStep, step / ownStep);
        CHECK(step <= 1.02 * ownStep);
    }
    // 2. switched on and off every 100 ms (the fade must hide the ring)
    {
        NotchBank b(kFs);
        std::vector<float> y = in;
        std::size_t pos = 0;
        bool on = false;
        while (pos < N) {
            on = !on;
            b.setNotch(0, 1000.0, 20.0, on);
            const std::size_t m = std::min<std::size_t>(4800, N - pos);
            b.process(y.data() + pos, m);
            pos += m;
        }
        const double step = maxStep(y, 1, N);
        std::printf("click check, switched on/off every 100 ms: largest step %.5f (ratio %.3f)\n", step, step / ownStep);
        CHECK(step <= 1.02 * ownStep);
    }
    // 2b. a tone exactly AT the notch, switched on and off: the fade turns it down over 10 ms instead of stepping
    {
        const std::vector<float> t1 = sine(1000.0, 0.5, N);
        const double own1 = maxStep(t1, 0, N);
        NotchBank b(kFs);
        std::vector<float> y = t1;
        std::size_t pos = 0;
        bool on = false;
        while (pos < N) {
            on = !on;
            b.setNotch(0, 1000.0, 20.0, on);
            const std::size_t m = std::min<std::size_t>(4800, N - pos);
            b.process(y.data() + pos, m);
            pos += m;
        }
        const double step = maxStep(y, 1, N);
        std::printf("click check, notch ON the tone, switched every 100 ms: largest step %.5f (ratio %.3f)\n", step, step / own1);
        CHECK(step <= 1.05 * own1);
    }
    // 3. the notch sweeps THROUGH the tone: an honest dip, still no step
    {
        NotchBank b(kFs);
        b.setNotch(0, 1500.0, 40.0, true);
        std::vector<float> y = in;
        std::size_t pos = 0;
        int k = 0;
        while (pos < N) {
            b.setNotch(0, 1500.0 + 1000.0 * static_cast<double>(k++) / 400.0, 40.0, true);
            const std::size_t m = std::min<std::size_t>(480, N - pos);
            b.process(y.data() + pos, m);
            pos += m;
        }
        const double step = maxStep(y, 24000, N);
        std::printf("click check, sweep through the tone: largest step %.5f (ratio %.3f)\n", step, step / ownStep);
        CHECK(step <= 1.02 * ownStep);
    }
    // 4. WITHOUT the glide and fade the same moves do click: the bare Notch with coefficients jumped (the reason for the bank)
    {
        cascade::dsp::Notch n(kFs, 300.0, 10.0);
        std::vector<float> y = in;
        std::size_t pos = 0;
        int k = 0;
        while (pos < N) {
            n.setFrequencyHz(300.0 + 2400.0 * ((k++ % 2) != 0 ? 1.0 : 0.0));
            const std::size_t m = std::min<std::size_t>(960, N - pos);
            n.process(y.data() + pos, m);
            pos += m;
        }
        const double step = maxStep(y, 24000, N);
        std::printf("for comparison, bare Notch with jumped frequency: largest step %.5f (ratio %.3f)\n", step, step / ownStep);
        CHECK(step > 1.2 * ownStep);
    }
}

void testZeroCost() {
    NotchBank b(kFs);
    std::vector<float> x(48000, 0.25f), y = x;
    for (int rep = 0; rep < 3; ++rep) {
        b.process(y.data(), y.size());
        CHECK(std::memcmp(x.data(), y.data(), x.size() * sizeof(float)) == 0);
    }
    CHECK(b.blocksWorked() == 0);  // nothing on: not one block was worked on
    // timing, for the record: ns per sample with 0, 1 and 10 notches on
    std::vector<float> w = sbgen::whiteNoise(48000 * 20, 0.3, 1);
    double ns[3];
    int idx = 0;
    for (int count : {0, 1, 10}) {
        NotchBank c(kFs);
        for (int i = 0; i < count; ++i) { c.setNotch(i, 300.0 + 250.0 * i, 30.0, true); }
        std::vector<float> t = w;
        c.process(t.data(), 5000);
        const auto t0 = std::chrono::steady_clock::now();
        for (int rep = 0; rep < 5; ++rep) { c.process(t.data(), t.size()); }
        ns[idx++] = 1e9 * std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() / (5.0 * static_cast<double>(t.size()));
    }
    std::printf("cost: %.2f ns/sample with none on, %.2f with one, %.2f with ten (48 kHz = 20833 ns per sample budget)\n", ns[0], ns[1], ns[2]);
    CHECK(ns[0] < 1.0);
    // a switched-off notch stops costing once its fade has finished
    NotchBank d(kFs);
    d.setNotch(0, 1000.0, 50.0, true);
    std::vector<float> t = sine(1000, 0.5, 4800);
    d.process(t.data(), t.size());
    d.setOn(0, false);
    std::vector<float> u = sine(1000, 0.5, 4800);
    d.process(u.data(), u.size());  // 100 ms: fade (10 ms) long finished
    std::vector<float> v = sine(1000, 0.5, 4800), v2 = v;
    d.process(v.data(), v.size());
    CHECK(std::memcmp(v.data(), v2.data(), v.size() * sizeof(float)) == 0);
}

void testChunking() {
    const std::vector<float> in = sbgen::whiteNoise(48000, 0.3, 4);
    // every chunk size divides 4800, where the notches are retuned and one is switched off, so all runs see the same stream positions
    auto run = [&](std::size_t chunk) {
        NotchBank b(kFs);
        b.setNotch(0, 1000.0, 50.0, true);
        b.setNotch(3, 2200.0, 80.0, true);
        std::vector<float> y = in;
        std::size_t pos = 0;
        while (pos < y.size()) {
            if (pos == 4800) {
                b.setNotch(0, 1800.0, 20.0, true);
                b.setNotch(3, 2200.0, 80.0, false);
                b.setNotch(5, 700.0, 30.0, true);
            }
            const std::size_t m = std::min(chunk, y.size() - pos);
            b.process(y.data() + pos, m);
            pos += m;
            if (pos < 4800 && pos + chunk > 4800) { pos = pos; }
        }
        return y;
    };
    const std::vector<float> a = run(4800);
    for (std::size_t c : {std::size_t(1), std::size_t(3), std::size_t(5), std::size_t(32), std::size_t(100), std::size_t(480), std::size_t(1600)}) {
        const std::vector<float> d = run(c);
        CHECK(std::memcmp(a.data(), d.data(), a.size() * sizeof(float)) == 0);
    }
}

void testBounds() {
    NotchBank b(kFs);
    CHECK(!b.setNotch(-1, 1000, 50, true));
    CHECK(!b.setNotch(10, 1000, 50, true));
    CHECK(!b.setNotch(0, std::nan(""), 50, true));
    CHECK(!b.setNotch(0, 1000, INFINITY, true));
    CHECK(!b.setOn(11, true));
    CHECK(b.activeCount() == 0);
    // clamps: a notch at 0 Hz, above Nyquist, with width 0 and a huge width all stay stable and finite
    for (double f : {0.0, -5.0, 1e9}) {
        for (double w : {0.0, 1e9}) {
            NotchBank c(kFs);
            c.setNotch(0, f, w, true);
            std::vector<float> x = sbgen::whiteNoise(48000, 0.3, 2);
            c.process(x.data(), x.size());
            bool fin = true;
            for (float v : x) { fin = fin && std::isfinite(v) && std::fabs(v) < 5.0f; }
            CHECK(fin);
        }
    }
    // empty input and clear()
    b.setNotch(0, 1000, 50, true);
    b.process(nullptr, 0);
    b.clear();
    CHECK(b.activeCount() == 0);
    // a non-48 kHz rate: 8 kHz low bound; a silly rate falls back to 48 kHz
    CHECK(NotchBank(1000.0).sampleRateHz() == 48000.0);
    NotchBank c(16000.0);
    c.setNotch(0, 1000.0, 50.0, true);
    std::vector<float> x(16000 * 3), ref;
    for (std::size_t i = 0; i < x.size(); ++i) { x[i] = static_cast<float>(0.5 * std::sin(sbgen::kTwoPi * 1000.0 * static_cast<double>(i) / 16000.0)); }
    c.process(x.data(), x.size());
    CHECK(rms(x.data() + 32000, 16000) < 1e-3);
}

void testNoAllocation() {
    NotchBank b(kFs);
    for (int i = 0; i < 10; ++i) { b.setNotch(i, 300.0 + 280.0 * i, 40.0, true); }
    std::vector<float> x = sbgen::whiteNoise(48000, 0.3, 3);
    b.process(x.data(), x.size());
    AllocProbe probe;
    for (int rep = 0; rep < 5; ++rep) {
        b.setNotch(rep, 500.0 + 100.0 * rep, 60.0, rep % 2 == 0);  // changes happen on the control side but allocate nothing either
        b.process(x.data(), x.size());
    }
    b.clear();
    b.process(x.data(), x.size());
    CHECK(probe.news() == 0);
}

void testConcurrent() {
    NotchBank b(kFs);
    std::atomic<bool> stop{false};
    std::atomic<bool> bad{false};
    std::thread dsp([&] {
        std::vector<float> x = sbgen::whiteNoise(4800, 0.3, 8);
        while (!stop.load()) {
            std::vector<float> y = x;
            b.process(y.data(), y.size());
            for (float v : y) {
                if (!std::isfinite(v) || std::fabs(v) > 6.0f) { bad = true; }
            }
        }
    });
    std::mt19937 g(5);
    for (int i = 0; i < 20000; ++i) {
        b.setNotch(static_cast<int>(g() % 10), 20.0 + static_cast<double>(g() % 20000), 1.0 + static_cast<double>(g() % 2000), (g() % 3) != 0);
        if ((i % 500) == 0) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
    }
    stop = true;
    dsp.join();
    CHECK(!bad.load());
}

}  // namespace

int main() {
    testDepthAndFlatness();
    testCloseNotches();
    testAllTen();
    testNoClick();
    testZeroCost();
    testChunking();
    testBounds();
    testNoAllocation();
    testConcurrent();
    return testSummary("test_notch_bank");
}
