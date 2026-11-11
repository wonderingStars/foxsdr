// Stereo placement (src/dsp/stereo_place.hpp): constant-power pan, routing, pseudo-stereo and binaural placement of generated
// mono audio. The interaural delay is measured by cross-correlating the two outputs for white noise, and compared with the
// Woodworth formula; the pseudo-stereo correlation is compared with the one derived in the header. No recording.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <cstdio>
#include <limits>

#include "alloc_probe.hpp"
#include "blk4_util.hpp"
#include "dsp/stereo_place.hpp"
#include "signal_block_gen.hpp"
#include "test_check.hpp"

using cascade::dsp::StereoMode;
using cascade::dsp::StereoPlacer;
using cascade::dsp::StereoRoute;

namespace {
constexpr double kFs = 48000.0;

struct Out {
    std::vector<float> l, r;
};
Out run(StereoPlacer& p, const std::vector<float>& in, std::size_t block = 480) {
    Out o;
    o.l.resize(in.size());
    o.r.resize(in.size());
    for (std::size_t i = 0; i < in.size(); i += block) {
        const std::size_t n = std::min(block, in.size() - i);
        p.process(in.data() + i, n, o.l.data() + i, o.r.data() + i);
    }
    return o;
}

void testLawAndRouting() {
    const std::vector<float> in = blk4::tone(48000, kFs, 1000.0, 0.5);
    double worst = 0;
    for (int k = -10; k <= 10; ++k) {
        const double pan = k / 10.0;
        StereoPlacer p(kFs);
        p.setPan(pan);
        const Out o = run(p, in);
        const double al = blk4::toneAmp(o.l, 24000, 48000, kFs, 1000.0) / 0.5, ar = blk4::toneAmp(o.r, 24000, 48000, kFs, 1000.0) / 0.5;
        const double th = (pan + 1.0) * 3.14159265358979323846 / 4.0;
        worst = std::max({worst, std::fabs(al - std::cos(th)), std::fabs(ar - std::sin(th)), std::fabs(al * al + ar * ar - 1.0)});
        if (k == -10 || k == 0 || k == 10) {
            std::printf("MEAS pan %+.1f: left %.4f (%.2f dB), right %.4f (%.2f dB), power %.5f\n", pan, al, blk4::db(al), ar, blk4::db(ar), al * al + ar * ar);
        }
    }
    std::printf("MEAS pan law, 21 positions: worst deviation from cos/sin and from constant power %.2e\n", worst);
    CHECK(worst < 1e-4);
    // Routing.
    StereoPlacer pl(kFs);
    pl.setRoute(StereoRoute::Left);
    pl.setPan(0.7);  // routing wins
    Out o = run(pl, in);
    bool exactL = true, zeroR = true;
    for (std::size_t i = 24000; i < in.size(); ++i) { exactL = exactL && o.l[i] == in[i]; zeroR = zeroR && o.r[i] == 0.0f; }
    CHECK(exactL && zeroR);
    StereoPlacer pr(kFs);
    pr.setRoute(StereoRoute::Right);
    o = run(pr, in);
    bool exactR = true, zeroL = true;
    for (std::size_t i = 24000; i < in.size(); ++i) { exactR = exactR && o.r[i] == in[i]; zeroL = zeroL && o.l[i] == 0.0f; }
    CHECK(exactR && zeroL);
    // Makeup: +3.01 dB brings the centre to unity.
    StereoPlacer pm(kFs);
    pm.setMakeupDb(3.0103);
    o = run(pm, in);
    CHECK_NEAR(blk4::toneAmp(o.l, 24000, 48000, kFs, 1000.0) / 0.5, 1.0, 1e-3);
    // Disabled: both channels are the input, bit-exact.
    StereoPlacer pd(kFs);
    pd.setEnabled(false);
    o = run(pd, in);
    CHECK(o.l == in && o.r == in);
}

void testClicks() {
    const std::vector<float> in = blk4::tone(48000, kFs, 1000.0, 0.5);
    StereoPlacer p(kFs);
    p.setPan(-1.0);
    Out o;
    o.l.resize(in.size());
    o.r.resize(in.size());
    p.process(in.data(), 12013, o.l.data(), o.r.data());  // (not on a zero crossing of the tone)
    p.setPan(1.0);
    p.process(in.data() + 12013, 35987, o.l.data() + 12013, o.r.data() + 12013);
    const double own = 0.5 * blk4::kTwoPi * 1000.0 / kFs;
    const double sl = blk4::maxStep(o.l, 11000, 48000), sr = blk4::maxStep(o.r, 11000, 48000);
    std::printf("MEAS pan hard left to hard right under a 1 kHz tone: largest step left %.4f, right %.4f, tone's own %.4f\n", sl, sr, own);
    CHECK(sl <= own * 1.02 && sr <= own * 1.02);
    // Mode changes under a tone: Pan -> Pseudo -> Binaural -> Pan.
    StereoPlacer q(kFs);
    q.setPseudoAmount(0.5);
    q.setAzimuthDeg(60.0);
    Out m;
    m.l.resize(in.size());
    m.r.resize(in.size());
    q.process(in.data(), 8000, m.l.data(), m.r.data());
    q.setMode(StereoMode::Pseudo);
    q.process(in.data() + 8000, 14000, m.l.data() + 8000, m.r.data() + 8000);
    q.setMode(StereoMode::Binaural);
    q.process(in.data() + 22000, 14000, m.l.data() + 22000, m.r.data() + 22000);
    q.setMode(StereoMode::Pan);
    q.process(in.data() + 36000, 12000, m.l.data() + 36000, m.r.data() + 36000);
    const double sml = blk4::maxStep(m.l, 4000, 48000), smr = blk4::maxStep(m.r, 4000, 48000);
    std::printf("MEAS mode changes Pan/Pseudo/Binaural/Pan under a 1 kHz tone: largest step left %.4f, right %.4f against the tone's own %.4f\n", sml, smr, own);
    CHECK(sml < own * 1.6 && smr < own * 1.6);
}

double corr0(const Out& o, std::size_t from) {
    double lr = 0, ll = 0, rr = 0;
    for (std::size_t i = from; i < o.l.size(); ++i) { lr += o.l[i] * o.r[i]; ll += o.l[i] * o.l[i]; rr += o.r[i] * o.r[i]; }
    return lr / std::sqrt(ll * rr);
}

void testPseudo() {
    const std::vector<float> noise = sbgen::whiteNoise(480000, 0.2, 3);
    for (double g : {0.0, 0.25, 0.5, 1.0}) {
        StereoPlacer p(kFs);
        p.setMode(StereoMode::Pseudo);
        p.setPseudoAmount(g);
        const Out o = run(p, noise);
        const double rho = corr0(o, 24000), want = (1.0 - g * g) / (1.0 + g * g);
        // Mono compatibility: L + R = 2 x / sqrt(1 + g^2).
        double worst = 0;
        for (std::size_t i = 24000; i < noise.size(); ++i) { worst = std::max(worst, std::fabs(o.l[i] + o.r[i] - 2.0 * noise[i] / std::sqrt(1.0 + g * g))); }
        std::printf("MEAS pseudo-stereo g %.2f: L/R correlation %.4f (derived %.4f), mono-sum deviation %.2e\n", g, rho, want, worst);
        CHECK_NEAR(rho, want, 0.02);
        CHECK(worst < 1e-5);
    }
    // Comb ripple: at a multiple of 125 Hz the channel is boosted, half way between it is cut, and the other channel is the opposite.
    StereoPlacer p(kFs);
    p.setMode(StereoMode::Pseudo);
    p.setPseudoAmount(0.5);
    const Out pk = run(p, blk4::tone(48000, kFs, 1000.0, 0.2));
    StereoPlacer p2(kFs);
    p2.setMode(StereoMode::Pseudo);
    p2.setPseudoAmount(0.5);
    const Out nt = run(p2, blk4::tone(48000, kFs, 1062.5, 0.2));
    const double k = 1.0 / std::sqrt(1.25);
    const double lp = blk4::toneAmp(pk.l, 24000, 48000, kFs, 1000.0) / 0.2, rp = blk4::toneAmp(pk.r, 24000, 48000, kFs, 1000.0) / 0.2;
    const double ln = blk4::toneAmp(nt.l, 24000, 48000, kFs, 1062.5) / 0.2, rn = blk4::toneAmp(nt.r, 24000, 48000, kFs, 1062.5) / 0.2;
    std::printf("MEAS pseudo-stereo g 0.5, 8 ms: at 1000 Hz left %.3f right %.3f, at 1062.5 Hz left %.3f right %.3f (derived %.3f / %.3f)\n", lp, rp, ln, rn, 1.5 * k, 0.5 * k);
    CHECK_NEAR(lp, 1.5 * k, 0.01);
    CHECK_NEAR(rp, 0.5 * k, 0.01);
    CHECK_NEAR(ln, 0.5 * k, 0.01);
    CHECK_NEAR(rn, 1.5 * k, 0.01);
}

// Lag (samples) of the right channel behind the left, from the cross-correlation peak of white noise, with parabolic refinement.
double measuredLag(const Out& o, std::size_t from) {
    double best = -1e300;
    long bk = 0;
    std::vector<double> c(81);
    for (long k = -40; k <= 40; ++k) {
        double s = 0;
        for (std::size_t i = from + 100; i + 100 < o.l.size(); ++i) { s += static_cast<double>(o.l[i]) * o.r[static_cast<std::size_t>(static_cast<long>(i) + k)]; }
        c[static_cast<std::size_t>(k + 40)] = s;
        if (s > best) { best = s; bk = k; }
    }
    const double a = c[static_cast<std::size_t>(bk + 39)], b = c[static_cast<std::size_t>(bk + 40)], d = c[static_cast<std::size_t>(bk + 41)];
    return static_cast<double>(bk) + 0.5 * (a - d) / (a - 2 * b + d);
}

void testBinaural() {
    const std::vector<float> noise = sbgen::whiteNoise(96000, 0.2, 4);
    const std::vector<float> tone = blk4::tone(48000, kFs, 1000.0, 0.2);
    for (double az : {0.0, 30.0, 60.0, 90.0, -60.0, -90.0}) {
        StereoPlacer p(kFs);
        p.setMode(StereoMode::Binaural);
        p.setAzimuthDeg(az);
        const Out o = run(p, noise);
        const double lag = measuredLag(o, 24000);
        const double thr = std::fabs(az) * 3.14159265358979323846 / 180.0;
        const double itdUs = (0.0875 / 343.0) * (thr + std::sin(thr)) * 1e6;  // Woodworth, written out here independently of the header
        const double want = (az >= 0 ? 1.0 : -1.0) * itdUs * 1e-6 * kFs;  // right lags left when the source is on the left
        // Source on the right (az > 0): the LEFT ear is far and lags, so the right channel leads: lag of right behind left is NEGATIVE.
        std::printf("MEAS azimuth %+.0f deg: right channel lags left by %.2f samples (Woodworth: %.2f, %.0f us)\n", az, lag, -want, itdUs);
        CHECK_NEAR(lag, -want, 0.35);
        StereoPlacer q(kFs);
        q.setMode(StereoMode::Binaural);
        q.setAzimuthDeg(az);
        const Out ot = run(q, tone);
        const double al = blk4::toneAmp(ot.l, 24000, 48000, kFs, 1000.0) / 0.2, ar = blk4::toneAmp(ot.r, 24000, 48000, kFs, 1000.0) / 0.2;
        const double ild = blk4::db(ar) - blk4::db(al);
        const double wantIld = 6.0 * std::sin(az * 3.14159265358979323846 / 180.0);
        std::printf("MEAS azimuth %+.0f deg, 1 kHz: level difference right minus left %.2f dB (6 dB x sin: %.2f)\n", az, ild, wantIld);
        CHECK_NEAR(ild, wantIld, 0.25);
    }
}

void testBasics() {
    // NaN input, clamps.
    StereoPlacer p(kFs);
    p.setPan(std::nan(""));
    p.setPan(5.0);
    p.setAzimuthDeg(500.0);
    p.setPseudoAmount(-1.0);
    p.setPseudoDelayMs(1e6);
    std::vector<float> bad(4800, std::numeric_limits<float>::quiet_NaN()), l(4800), r(4800);
    for (auto m : {StereoMode::Pan, StereoMode::Pseudo, StereoMode::Binaural}) {
        p.setMode(m);
        p.process(bad.data(), bad.size(), l.data(), r.data());
        p.process(bad.data(), bad.size(), l.data(), r.data());
        p.process(bad.data(), bad.size(), l.data(), r.data());
        bool fin = true;
        for (std::size_t i = 0; i < l.size(); ++i) { fin = fin && std::isfinite(l[i]) && std::isfinite(r[i]); }
        CHECK(fin);
    }
    // Empty block.
    p.process(nullptr, 0, nullptr, nullptr);
    // Chunk invariance in each mode.
    const std::vector<float> in = sbgen::speechLike(48000, kFs, 0.4, 9);
    for (auto m : {StereoMode::Pan, StereoMode::Pseudo, StereoMode::Binaural}) {
        StereoPlacer a(kFs);
        a.setMode(m);
        a.setPan(0.3);
        a.setAzimuthDeg(40.0);
        a.setPseudoAmount(0.7);
        const Out whole = run(a, in, in.size());
        for (std::size_t blk : {1u, 37u, 1000u}) {
            StereoPlacer b(kFs);
            b.setMode(m);
            b.setPan(0.3);
            b.setAzimuthDeg(40.0);
            b.setPseudoAmount(0.7);
            const Out o = run(b, in, blk);
            CHECK(o.l == whole.l && o.r == whole.r);
        }
    }
    // Allocation, with setting changes; cost.
    StereoPlacer al(kFs);
    std::vector<float> blk(480, 0.1f), bl(480), br(480);
    long news = 0;
    for (int i = 0; i < 40; ++i) {
        AllocProbe pr;
        al.setMode(static_cast<StereoMode>(i % 3));
        al.setRoute(static_cast<StereoRoute>(i % 3));
        al.setPan((i % 20 - 10) / 10.0);
        al.setAzimuthDeg(i * 4.0 - 80.0);
        al.setPseudoAmount(i / 40.0);
        al.setEnabled(i % 7 != 0);
        al.process(blk.data(), blk.size(), bl.data(), br.data());
        news += pr.news();
    }
    std::printf("MEAS operator new in 40 process calls with setting changes: %ld\n", news);
    CHECK(news == 0);
    std::vector<float> big = sbgen::whiteNoise(480000, 0.2, 1), bL(480000), bR(480000);
    double ns[3];
    int idx = 0;
    for (auto m : {StereoMode::Pan, StereoMode::Pseudo, StereoMode::Binaural}) {
        StereoPlacer c(kFs);
        c.setMode(m);
        c.setAzimuthDeg(50.0);
        auto t0 = std::chrono::steady_clock::now();
        c.process(big.data(), big.size(), bL.data(), bR.data());
        auto t1 = std::chrono::steady_clock::now();
        ns[idx++] = std::chrono::duration<double, std::nano>(t1 - t0).count() / 480000.0;
    }
    std::printf("MEAS cost per sample: pan %.1f ns, pseudo-stereo %.1f ns, binaural %.1f ns\n", ns[0], ns[1], ns[2]);
    CHECK(ns[0] < 1000.0 && ns[1] < 1000.0 && ns[2] < 1000.0);
}
}  // namespace

int main() {
    testLawAndRouting();
    testClicks();
    testPseudo();
    testBinaural();
    testBasics();
    return testSummary("test_stereo_place");
}
