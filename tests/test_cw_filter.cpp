// CW audio peak filter and channel-filter shape design (src/dsp/cw_filter.hpp). Generated tones only.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <cstdio>
#include <limits>

#include "alloc_probe.hpp"
#include "blk4_util.hpp"
#include "dsp/cw_filter.hpp"
#include "test_check.hpp"

using namespace cascade::dsp;

namespace {
constexpr double kFs = 48000.0;

void settle(CwPeakFilter& f, double pitch, double width, double gain) {
    f.setPitchHz(pitch);
    f.setWidthHz(width);
    f.setGainDb(gain);
    f.setEnabled(true);
    std::vector<float> z(24000, 0.0f);
    f.process(z.data(), z.size());
}
double settledResp(double pitch, double width, double gain, double hz) {
    CwPeakFilter f(kFs);
    settle(f, pitch, width, gain);
    return f.responseDb(hz);
}

// Width between the points where the response is half the boost, found on the coefficient response.
double halfGainWidth(const CwPeakFilter& f, double pitch, double gainDb) {
    const double half = gainDb / 2.0;
    double lo = pitch, hi = pitch;
    for (double a = pitch, b = 1.0; b > 0; ) {  // walk down to below `half`, then bisect
        if (f.responseDb(a) < half) { lo = a; b = 0; } else { a -= 0.5; if (a < 5) { lo = 5; b = 0; } }
    }
    double l0 = lo, l1 = lo + 0.5;
    for (int i = 0; i < 40; ++i) { const double m = 0.5 * (l0 + l1); if (f.responseDb(m) < half) { l0 = m; } else { l1 = m; } }
    lo = 0.5 * (l0 + l1);
    for (double a = pitch; ; a += 0.5) { if (f.responseDb(a) < half || a > 0.49 * kFs) { hi = a; break; } }
    double h0 = hi - 0.5, h1 = hi;
    for (int i = 0; i < 40; ++i) { const double m = 0.5 * (h0 + h1); if (f.responseDb(m) < half) { h1 = m; } else { h0 = m; } }
    hi = 0.5 * (h0 + h1);
    return hi - lo;
}

void testPeak() {
    for (double pitch : {400.0, 700.0, 1000.0}) {
        for (double width : {30.0, 100.0, 500.0}) {
            CwPeakFilter f(kFs); settle(f, pitch, width, 12.0);
            const double w = halfGainWidth(f, pitch, 12.0);
            std::vector<float> t = blk4::tone(48000, kFs, pitch, 0.1);
            f.process(t.data(), t.size());
            const double g = blk4::db(blk4::toneAmp(t, 24000, 48000, kFs, pitch) / 0.1);
            std::vector<float> far = blk4::tone(48000, kFs, pitch + 4.0 * width, 0.1);
            CwPeakFilter f2(kFs); settle(f2, pitch, width, 12.0);
            f2.process(far.data(), far.size());
            const double gf = blk4::db(blk4::toneAmp(far, 24000, 48000, kFs, pitch + 4.0 * width) / 0.1);
            std::printf("MEAS pitch %.0f width %.0f: centre %.2f dB, half-gain width %.1f Hz, 4 widths away %.2f dB\n", pitch, width, g, w, gf);
            CHECK_NEAR(g, 12.0, 0.1);
            CHECK_NEAR(w, width, width * 0.06);
            CHECK(gf < 3.0);
        }
    }
    // Gain setting and range: 6 and 24 dB, and a wider width is flatter at the shoulders.
    CHECK_NEAR(settledResp(700, 100, 6.0, 700), 6.0, 0.05);
    CHECK_NEAR(settledResp(700, 100, 24.0, 700), 24.0, 0.05);
    CHECK(settledResp(700, 500, 12.0, 900) > settledResp(700, 30, 12.0, 900) + 5.0);
}

void testFollowAndClicks() {
    // The pitch is changed: the boost moves with it.
    {
        CwPeakFilter f(kFs); settle(f, 600, 60, 12.0);
        f.setPitchHz(900);
        std::vector<float> z(24000, 0.0f);
        f.process(z.data(), z.size());
        CHECK_NEAR(f.responseDb(900), 12.0, 0.1);
        CHECK(f.responseDb(600) < 0.5);
    }
    // No click while the pitch glides under the tone it leaves, nor while the filter is switched on or off.
    {
        CwPeakFilter f(kFs); settle(f, 700, 30, 24.0);
        std::vector<float> x = blk4::tone(48000, kFs, 700.0, 0.05);
        f.process(x.data(), 12000);
        f.setPitchHz(1000);
        f.process(x.data() + 12000, 12000);
        f.setPitchHz(700);
        f.process(x.data() + 24000, 24000);
        const double own = 0.05 * 15.85 * blk4::kTwoPi * 700.0 / kFs;
        const double st = blk4::maxStep(x, 0, 48000);
        std::printf("MEAS pitch 700 -> 1000 -> 700 Hz under a 700 Hz tone: largest step %.4f, loudest state's own %.4f (ratio %.3f)\n", st, own, st / own);
        CHECK(st <= own * 1.03);
    }
    {
        CwPeakFilter f(kFs);
        f.setPitchHz(700); f.setWidthHz(40); f.setGainDb(24);
        std::vector<float> x = blk4::tone(72000, kFs, 700.0, 0.05);
        f.process(x.data(), 12000);
        f.setEnabled(true);
        f.process(x.data() + 12000, 30000);
        f.setEnabled(false);
        f.process(x.data() + 42000, 30000);
        const double own = 0.05 * 16.0 * blk4::kTwoPi * 700.0 / kFs;  // +24 dB: 15.8 x
        const double st = blk4::maxStep(x, 0, 72000);
        std::printf("MEAS switched on (+24 dB) and off under a 700 Hz tone: largest step %.4f, loudest state's own %.4f (ratio %.3f)\n", st, own, st / own);
        CHECK(st <= own * 1.03);
        // After settling off, the filter is bypassed again.
        CHECK(f.isBypassed());
    }
}

void testBasics() {
    std::vector<float> a = blk4::tone(9600, kFs, 700.0, 0.2), b = a;
    CwPeakFilter off(kFs);
    off.process(b.data(), b.size());
    CHECK(a == b);  // off: bit-exact
    CHECK(off.isBypassed());
    CwPeakFilter off2(kFs);
    off2.setGainDb(12);  // not enabled
    off2.process(b.data(), b.size());
    CHECK(a == b);
    // Cost: off against on (a measurement; off must be far cheaper).
    {
        std::vector<float> big = blk4::tone(480000, kFs, 700.0, 0.2);
        CwPeakFilter o(kFs), n(kFs);
        n.setEnabled(true);
        auto t0 = std::chrono::steady_clock::now();
        o.process(big.data(), big.size());
        auto t1 = std::chrono::steady_clock::now();
        n.process(big.data(), big.size());
        auto t2 = std::chrono::steady_clock::now();
        const double nsOff = std::chrono::duration<double, std::nano>(t1 - t0).count() / 480000.0;
        const double nsOn = std::chrono::duration<double, std::nano>(t2 - t1).count() / 480000.0;
        std::printf("MEAS CW peak cost per sample: off %.3f ns, on %.2f ns\n", nsOff, nsOn);
        CHECK(nsOff * 10.0 < nsOn);
    }
    // Clamps and NaN.
    CwPeakFilter c(kFs);
    c.setWidthHz(1.0);  CHECK(c.widthHz() == 30.0);
    c.setWidthHz(9999); CHECK(c.widthHz() == 500.0);
    c.setPitchHz(50);   CHECK(c.pitchHz() == 200.0);
    c.setPitchHz(std::nan("")); CHECK(c.pitchHz() == 200.0);
    c.setGainDb(99);    CHECK(c.gainDb() == 24.0);
    c.setGainDb(-5);    CHECK(c.gainDb() == 0.0);
    // Chunk invariance, non-finite input.
    std::vector<float> in = blk4::tone(20000, kFs, 800.0, 0.2);
    CwPeakFilter w(kFs);
    w.setPitchHz(800); w.setWidthHz(80); w.setGainDb(15); w.setEnabled(true);
    std::vector<float> whole = in;
    w.process(whole.data(), whole.size());
    for (std::size_t blk : {1u, 13u, 500u}) {
        CwPeakFilter v(kFs);
        v.setPitchHz(800); v.setWidthHz(80); v.setGainDb(15); v.setEnabled(true);
        CHECK(blk4::runBlocks([&](float* p, std::size_t n) { v.process(p, n); }, in, blk) == whole);
    }
    std::vector<float> bad = in;
    bad[50] = std::numeric_limits<float>::quiet_NaN();
    w.process(bad.data(), bad.size());
    bool fin = true;
    for (float v : bad) { fin = fin && std::isfinite(v); }
    CHECK(fin);
    // No allocation, with setting changes.
    CwPeakFilter z(kFs);
    std::vector<float> blk = blk4::tone(480, kFs, 700.0, 0.2);
    long news = 0;
    for (int i = 0; i < 40; ++i) {
        AllocProbe p;
        z.setEnabled(i % 7 != 0);
        z.setPitchHz(500 + 20 * i);
        z.setWidthHz(30 + 10 * i);
        z.setGainDb(i % 25);
        z.process(blk.data(), blk.size());
        news += p.news();
    }
    std::printf("MEAS operator new in 40 CW peak process calls with setting changes: %ld\n", news);
    CHECK(news == 0);
}

void testChannelShape() {
    struct Row { const char* name; ChannelShape s; double want; };
    const Row rows[] = {{"sharp", ChannelShape::Sharp, 1.3}, {"medium", ChannelShape::Medium, 1.8}, {"soft", ChannelShape::Soft, 3.157}};
    for (double fs : {48000.0, 12000.0}) {
        for (double bw : {500.0, 1500.0, 2400.0, 3000.0}) {
            if (bw >= 0.8 * fs / 2) { continue; }
            if (fs > 20000.0 && bw < 1000.0) { continue; }  // 500 Hz sharp at 48 kHz needs about 4000 taps: the wiring runs it decimated
            for (const Row& r : rows) {
                ChannelDesign d = designChannelFilter(fs, bw, r.s);
                double dc = 0;
                for (float t : d.taps) { dc += t; }
                std::printf("MEAS fs %.0f bw6 %.0f %-6s: %4zu taps, measured -6 dB width %.1f, -60 dB width %.1f, shape factor %.3f (wanted %.2f)%s\n",
                            fs, bw, r.name, d.taps.size(), d.bw6Hz, d.bw60Hz, d.shapeFactor, r.want, d.capped ? " CAPPED" : "");
                CHECK(!d.capped);
                CHECK_NEAR(d.shapeFactor, r.want, r.want * 0.03);
                CHECK_NEAR(d.bw6Hz, bw, bw * 0.01);
                CHECK_NEAR(dc, 1.0, 1e-5);
                // Symmetric (linear phase) and the -60 dB claim holds everywhere above the measured edge.
                bool sym = true;
                for (std::size_t k = 0; k < d.taps.size() / 2; ++k) { sym = sym && d.taps[k] == d.taps[d.taps.size() - 1 - k]; }
                CHECK(sym);
                double worst = -300;
                for (double f = d.bw60Hz * 0.5 * 1.01; f < fs / 2; f += fs / 4000.0) { worst = std::max(worst, tapsResponseDb(d.taps, fs, f)); }
                CHECK(worst < -59.5);
            }
        }
    }
    // The ordering and the price: sharper means more taps and a longer ring.
    const ChannelDesign sh = designChannelFilter(48000.0, 2400.0, ChannelShape::Sharp);
    const ChannelDesign me = designChannelFilter(48000.0, 2400.0, ChannelShape::Medium);
    const ChannelDesign so = designChannelFilter(48000.0, 2400.0, ChannelShape::Soft);
    CHECK(sh.taps.size() > me.taps.size() && me.taps.size() > so.taps.size());
    CHECK(sh.shapeFactor < me.shapeFactor && me.shapeFactor < so.shapeFactor);
    auto settleMs = [](const ChannelDesign& d, double fs) {  // step response: last time it is more than 5 % from 1
        double c = 0; std::size_t last = 0;
        for (std::size_t k = 0; k < d.taps.size(); ++k) { c += d.taps[k]; if (std::fabs(c - 1.0) > 0.05) { last = k; } }
        return 1000.0 * static_cast<double>(last) / fs;
    };
    std::printf("MEAS 2400 Hz at 48 kHz: step response within 5 %% after sharp %.2f ms (%zu taps, delay %zu), medium %.2f ms (%zu, %zu), soft %.2f ms (%zu, %zu)\n",
                settleMs(sh, 48000.0), sh.taps.size(), sh.delaySamples, settleMs(me, 48000.0), me.taps.size(), me.delaySamples,
                settleMs(so, 48000.0), so.taps.size(), so.delaySamples);
    CHECK(settleMs(sh, 48000.0) > settleMs(so, 48000.0));
    // Gaussian: no overshoot in the step response (the cumulative sum never passes 1); Kaiser sharp does overshoot.
    auto overshoot = [](const ChannelDesign& d) { double c = 0, m = 0; for (float t : d.taps) { c += t; m = std::max(m, c); } return m - 1.0; };
    std::printf("MEAS step overshoot: sharp %.4f, medium %.4f, soft %.6f\n", overshoot(sh), overshoot(me), overshoot(so));
    CHECK(overshoot(so) < 1e-4);
    CHECK(overshoot(sh) > 0.01);
    // Between 2.0 and 2.6 is not offered: the request is clamped to the Kaiser limit; the measured factor comes back.
    const ChannelDesign mid = designChannelFilter(48000.0, 2400.0, 2.3);
    CHECK_NEAR(mid.shapeFactor, 2.0, 0.06);
    // A run through the taps: tone inside passes at about -1 dB or better, tone beyond the -60 dB edge is gone.
    const ChannelDesign d = designChannelFilter(48000.0, 2400.0, ChannelShape::Medium);
    for (double hz : {300.0, 600.0}) {
        CHECK(tapsResponseDb(d.taps, 48000.0, hz) > -1.0);
    }
    // Cap: 100 Hz at 1.3 would need thousands of taps; with the default cap the design says so and reports what it got.
    const ChannelDesign cap = designChannelFilter(48000.0, 100.0, ChannelShape::Sharp);
    std::printf("MEAS 100 Hz sharp at 48 kHz: %zu taps, capped %d, shape factor %.2f\n", cap.taps.size(), cap.capped ? 1 : 0, cap.shapeFactor);
    CHECK(cap.capped);
    CHECK(cap.taps.size() == 2047);
    CHECK(cap.shapeFactor > 1.3);
    // Bad input.
    CHECK(designChannelFilter(48000.0, 0.0, 1.3).taps.empty());
    CHECK(designChannelFilter(48000.0, std::nan(""), 1.3).taps.empty());
    CHECK(designChannelFilter(0.0, 2400.0, 1.3).taps.empty());
    CHECK(designChannelFilter(48000.0, -5.0, 1.3).taps.empty());
    // A silly shape factor is clamped and still gives a filter.
    CHECK(!designChannelFilter(48000.0, 2400.0, 0.5).taps.empty());
}
}  // namespace

int main() {
    testPeak();
    testFollowAndClicks();
    testBasics();
    testChannelShape();
    return testSummary("test_cw_filter");
}
