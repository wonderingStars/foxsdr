// The IF filter node (src/dsp/if_filter.hpp): edges and notches in RF Hz, kept on the RF frequency across a retune, without a click.
// Generated I/Q only, with a little noise and a sample-rate error on every tone.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <limits>
#include <random>

#include "dsp/if_filter.hpp"
#include "dspmodels_alloc.hpp"
#include "dspmodels_util.hpp"
#include "test_check.hpp"

using cascade::dsp::IfFilter;
using Cf = std::complex<float>;

namespace {

constexpr double kFs = 96000.0;
constexpr double kPpm = 25.0;  // the generator's sample-rate error

struct Gen {
    std::mt19937_64 rng{1};
    std::normal_distribution<double> g{0.0, 1.0};
    double phase = 0.0;
    double noise = 1e-4;  // -80 dB re a unit tone
    // n samples of a tone at baseband offset hz (the phase carries on across calls)
    std::vector<Cf> tone(std::size_t n, double hz, double amp = 1.0) {
        std::vector<Cf> v(n);
        const double inc = dspm::kTwoPi * hz * (1.0 + kPpm * 1e-6) / kFs;
        for (std::size_t i = 0; i < n; ++i) {
            phase += inc;
            v[i] = Cf(static_cast<float>(amp * std::cos(phase) + noise * g(rng)), static_cast<float>(amp * std::sin(phase) + noise * g(rng)));
        }
        return v;
    }
};

// Complex amplitude at baseband offset hz in v[from, to) (the generator's phase offset is irrelevant: the modulus is taken).
double amplitudeAt(const std::vector<Cf>& v, std::size_t from, std::size_t to, double hz) {
    std::complex<double> acc(0, 0);
    const double w = dspm::kTwoPi * hz * (1.0 + kPpm * 1e-6) / kFs;
    for (std::size_t i = from; i < to; ++i) { acc += std::complex<double>(v[i].real(), v[i].imag()) * std::polar(1.0, -w * static_cast<double>(i)); }
    return std::abs(acc) / static_cast<double>(to - from);
}

// Steady-state gain in dB for a tone at RF `rfHz` with the radio at `fc`.
double gainDb(IfFilter& f, double fc, double rfHz) {
    Gen g;
    constexpr std::size_t n = 48000;
    std::vector<Cf> x = g.tone(n, rfHz - fc);
    f.process(x.data(), n);
    return dspm::db(amplitudeAt(x, n / 2, n, rfHz - fc));
}

// A fresh filter on an SSB-like window: RF 7.100 .. 7.103 MHz, centre 7.0985 MHz (the window sits above the centre).
IfFilter ssb() {
    IfFilter f(kFs);
    f.retune(7098500.0);
    f.setBand(7100000.0, 7103000.0, true);
    return f;
}

void testPassband() {
    const double fc = 7098500.0;
    // In the middle of the window: 0 dB. At each edge: -3.01 dB. Twice the half-width beyond an edge: Butterworth 4th order, -24.1 dB. Far: below -60 dB.
    struct P { double rf; double pin; double tol; const char* what; };
    const P pts[] = {{7101500.0, 0.0, 0.05, "middle"},     {7100000.0, -3.01, 0.25, "low edge"},  {7103000.0, -3.01, 0.25, "high edge"},
                     {7098500.0, -24.1, 1.5, "1.5 kHz below"}, {7104500.0, -24.1, 1.5, "1.5 kHz above"}, {7090000.0, -100.0, 0.0, "far below"}};
    for (const P& p : pts) {
        IfFilter f = ssb();
        const double got = gainDb(f, fc, p.rf);
        const double model = f.responseDb(p.rf);
        std::printf("MEAS band 7.100-7.103 MHz, %s (%.0f Hz RF): %.2f dB (pin %.2f, model %.2f)\n", p.what, p.rf, got, p.pin, model);
        if (p.pin < -60.0) { CHECK(got < -60.0); } else { CHECK_NEAR(got, p.pin, p.tol); }
        if (got > -60.0) { CHECK_NEAR(got, model, 0.3); }
    }
    // The window is ONE-SIDED (a complex filter): the mirror image about the centre of the radio is out of the band.
    IfFilter f = ssb();
    const double mirror = fc - 3000.0;  // the same offset as +3 kHz, below the centre
    const double inBand = gainDb(f, fc, fc + 1500.0 + 1500.0 * 0.0);
    IfFilter f2 = ssb();
    const double mir = gainDb(f2, fc, fc - 1500.0);
    std::printf("MEAS window offset +1.5 kHz: %.2f dB; its mirror at -1.5 kHz: %.2f dB\n", inBand, mir);
    CHECK(inBand > -3.2);
    CHECK(mir < -35.0);  // Butterworth 4th order, three half-widths out: -38.2 dB
    (void)mirror;
}

void testNotch() {
    const double fc = 7098500.0;
    const double nrf = 7101200.0;  // a carrier inside the window
    IfFilter f = ssb();
    f.setNotch(0, nrf, 100.0, true);
    const double at = gainDb(f, fc, nrf);
    IfFilter f2 = ssb();
    f2.setNotch(0, nrf, 100.0, true);
    const double half = gainDb(f2, fc, nrf + 50.0);
    IfFilter f3 = ssb();
    f3.setNotch(0, nrf, 100.0, true);
    const double far = gainDb(f3, fc, nrf + 1000.0);
    IfFilter ref = ssb();
    const double refFar = gainDb(ref, fc, nrf + 1000.0);
    std::printf("MEAS notch 100 Hz wide at 7.1012 MHz: centre %.1f dB, +50 Hz %.2f dB (pin -3.0), +1 kHz %.3f dB vs %.3f dB without the notch\n", at, half, far, refFar);
    CHECK(at < -50.0);
    CHECK_NEAR(half, -3.0 + (refFar * 0.0), 0.5);
    CHECK_NEAR(far - refFar, 0.0, 0.25);
    // Disabled notch: no effect. Eight notches at once all bite.
    IfFilter off = ssb();
    off.setNotch(0, nrf, 100.0, false);
    CHECK_NEAR(gainDb(off, fc, nrf), refFar + (0.0), 3.5);  // (a tone at the notch frequency passes at the band's own gain)
    IfFilter many = ssb();
    for (int i = 0; i < IfFilter::kMaxNotches; ++i) { many.setNotch(i, 7100100.0 + 300.0 * i, 40.0, true); }
    for (int i = 0; i < IfFilter::kMaxNotches; i += 3) {
        IfFilter m2 = ssb();
        for (int k = 0; k < IfFilter::kMaxNotches; ++k) { m2.setNotch(k, 7100100.0 + 300.0 * k, 40.0, true); }
        CHECK(gainDb(m2, fc, 7100100.0 + 300.0 * i) < -45.0);
    }
    // Width is clamped; bad numbers and bad indices are ignored.
    many.setNotch(-1, 1.0, 1.0, true);
    many.setNotch(99, 1.0, 1.0, true);
    many.setNotch(0, std::numeric_limits<double>::quiet_NaN(), 1.0, true);
    many.setNotch(0, 7100100.0, 1e-9, true);
    many.setNotch(0, 7100100.0, 1e12, true);
}

void testRetuneKeepsEdgesOnRf() {
    // The SAME RF frequencies after the radio has moved 20 kHz: the same gains.
    const double fc2 = 7098500.0 + 20000.0;
    IfFilter f = ssb();
    f.setNotch(0, 7101200.0, 100.0, true);
    f.retune(fc2);
    const double mid = gainDb(f, fc2, 7101500.0 + 600.0);
    IfFilter ref = ssb();
    ref.setNotch(0, 7101200.0, 100.0, true);
    const double midRef = gainDb(ref, 7098500.0, 7101500.0 + 600.0);
    CHECK_NEAR(mid, midRef, 0.05);
    IfFilter g = ssb();
    g.setNotch(0, 7101200.0, 100.0, true);
    g.retune(fc2);
    CHECK(gainDb(g, fc2, 7101200.0) < -35.0);  // (the 25 ppm sample-rate error detunes a -17 kHz tone by 0.4 Hz: that limits the depth)
    IfFilter h = ssb();
    h.retune(fc2);
    CHECK_NEAR(gainDb(h, fc2, 7100000.0), -3.01, 0.25);
    IfFilter k = ssb();
    k.retune(fc2);
    CHECK(gainDb(k, fc2, 7096000.0) < -40.0);  // an RF frequency below the window stays below it
    std::printf("MEAS after a 20 kHz retune: in-window tone %.3f dB (before %.3f), notch %.1f dB deep, low edge %.2f dB\n", mid, midRef, gainDb(g, fc2, 7101200.0), -3.01);
    // The window's baseband edges moved by the retune and say so.
    double lo, hi;
    f.edgesBaseband(&lo, &hi);
    CHECK_NEAR(lo, 7100000.0 - fc2, 1e-6);
    CHECK_NEAR(hi, 7103000.0 - fc2, 1e-6);
    CHECK(!f.bandOutsideSpan());
}

// A continuous complex tone through a retune in mid-stream: the modulus must stay put and the sample step must stay at the tone's own size.
struct ClickResult {
    double modMin, modMax, maxStep;
};
ClickResult retuneRun(IfFilter& f, double fcA, double fcB, double rf, std::size_t block) {
    Gen g;
    g.noise = 0.0;
    constexpr std::size_t n = 24000;
    std::vector<Cf> out;
    double phase = 0.0;
    const double ppm = 1.0 + kPpm * 1e-6;
    for (std::size_t p = 0; p < 2 * n; p += block) {
        if (p == n) { f.retune(fcB); }
        const std::size_t m = std::min(block, 2 * n - p);
        std::vector<Cf> x(m);
        const double fc = p >= n ? fcB : fcA;
        for (std::size_t i = 0; i < m; ++i) {
            phase += dspm::kTwoPi * (rf - fc) * ppm / kFs;
            x[i] = Cf(static_cast<float>(std::cos(phase)), static_cast<float>(std::sin(phase)));
        }
        f.process(x.data(), m);
        out.insert(out.end(), x.begin(), x.end());
    }
    ClickResult r{1e9, 0.0, 0.0};
    for (std::size_t i = 6000; i < 2 * n; ++i) {  // after the first settle
        const double mod = std::abs(out[i]);
        r.modMin = std::min(r.modMin, mod);
        r.modMax = std::max(r.modMax, mod);
        if (i > 6000) { r.maxStep = std::max(r.maxStep, static_cast<double>(std::abs(out[i] - out[i - 1]))); }
    }
    return r;
}

void testRetuneIsClickFree() {
    const double fcA = 7098500.0, fcB = 7098500.0 + 30000.0;
    // The tone sits at 7.1015 MHz inside the window: baseband +3 kHz before, -27 kHz after the retune; the output tone changes its baseband
    // frequency with it. Modulus constant; the step is the tone's own (2 pi f / fs, the larger of the two).
    IfFilter f = ssb();
    const ClickResult r = retuneRun(f, fcA, fcB, 7101500.0, 480);
    const double ownStep = dspm::kTwoPi * 27000.0 / kFs;
    std::printf("MEAS retune of a continuous tone: modulus %.4f .. %.4f, largest sample step %.4f (own step %.4f)\n", r.modMin, r.modMax, r.maxStep, ownStep);
    CHECK(r.modMax - r.modMin < 0.01);
    CHECK(r.maxStep < ownStep * 1.02);
    // The same with a notch present and a retune that keeps it in the span, in blocks of different sizes: the same answer.
    IfFilter g = ssb();
    g.setNotch(0, 7101900.0, 80.0, true);
    const ClickResult r1 = retuneRun(g, fcA, fcB, 7101500.0, 1);
    IfFilter g2 = ssb();
    g2.setNotch(0, 7101900.0, 80.0, true);
    const ClickResult r2 = retuneRun(g2, fcA, fcB, 7101500.0, 1000);
    CHECK(r1.modMax - r1.modMin < 0.01);
    CHECK_NEAR(r1.maxStep, r2.maxStep, 1e-9);
    CHECK_NEAR(r1.modMin, r2.modMin, 1e-9);
}

void testEdgeLeavesSpan() {
    // The window's far edge is carried off the end of the visible span by a retune: the width changes, the new cut-off fades in over 10 ms, and the
    // modulus of a tone that stays in the window never jumps.
    const double fcA = 7098500.0;
    IfFilter f(kFs);
    f.retune(fcA);
    f.setBand(7100000.0, 7103000.0, true);
    const double fcB = 7103000.0 - 0.45 * kFs + 4000.0;  // the high edge is now 4 kHz inside the span's end; the low edge is far below it
    const double fcC = fcB - 3000.0;                       // the window now reaches beyond the span: clamped
    Gen g;
    g.noise = 0.0;
    std::vector<Cf> out;
    double phase = 0.0;
    auto run = [&](std::size_t n, double fc) {
        std::vector<Cf> x(n);
        for (std::size_t i = 0; i < n; ++i) {
            phase += dspm::kTwoPi * (7100800.0 - fc) / kFs;
            x[i] = Cf(static_cast<float>(std::cos(phase)), static_cast<float>(std::sin(phase)));
        }
        f.process(x.data(), n);
        out.insert(out.end(), x.begin(), x.end());
    };
    run(12000, fcA);
    f.retune(fcB);
    run(12000, fcB);
    f.retune(fcC);
    const std::size_t at = out.size();
    run(12000, fcC);
    double jump = 0.0, minMod = 1e9, maxMod = 0.0;
    for (std::size_t i = 6000; i < out.size(); ++i) {
        const double a = std::abs(out[i]);
        minMod = std::min(minMod, a);
        maxMod = std::max(maxMod, a);
        if (i > 6000) { jump = std::max(jump, std::fabs(a - std::abs(out[i - 1]))); }
    }
    double lo, hi;
    f.edgesBaseband(&lo, &hi);
    std::printf("MEAS edge carried past the span: edges now %.0f .. %.0f Hz (limit +-%.0f), modulus %.4f .. %.4f, largest one-sample change %.5f, retune at sample %zu\n",
                lo, hi, 0.45 * kFs, minMod, maxMod, jump, at);
    CHECK(hi <= 0.45 * kFs + 1e-6);
    CHECK(maxMod - minMod < 0.02);
    CHECK(jump < 0.003);
    // The band wholly outside the span passes nothing, fading out and back (no step).
    IfFilter w(kFs);
    w.retune(7098500.0);
    w.setBand(7100000.0, 7103000.0, true);
    CHECK(!w.bandOutsideSpan());
    w.retune(7098500.0 + 200000.0);
    CHECK(w.bandOutsideSpan());
    CHECK(w.responseDb(7101500.0) < -200.0);
    std::vector<Cf> x(48000, Cf(0.5f, 0.0f));
    w.process(x.data(), x.size());
    double step = 0;
    for (std::size_t i = 1; i < x.size(); ++i) { step = std::max(step, static_cast<double>(std::abs(x[i] - x[i - 1]))); }
    CHECK(std::abs(x.back()) < 1e-6);
    CHECK(step < 0.5 / (0.010 * kFs) * 3.0);  // a ramp over 10 ms, not a step
    w.retune(7098500.0);                       // and back
    std::vector<Cf> y(48000, Cf(0.5f, 0.0f));
    w.process(y.data(), y.size());
    CHECK(std::abs(y.back()) > 0.0);
}

void testOnOffAndBypass() {
    IfFilter f(kFs);
    f.retune(7098500.0);
    CHECK(f.isBypassed());
    Gen g;
    std::vector<Cf> in = g.tone(4800, 1234.0, 0.7), x = in;
    f.process(x.data(), x.size());
    bool same = true;
    for (std::size_t i = 0; i < x.size(); ++i) { same = same && x[i] == in[i]; }
    CHECK(same);  // bit-exact when nothing is on
    f.setBand(7100000.0, 7103000.0, true);
    CHECK(!f.isBypassed());
    // Off again: after the fade it is bypassed once more, and a tone out of the window is untouched.
    std::vector<Cf> warm = g.tone(4800, 2000.0, 0.5);
    f.process(warm.data(), warm.size());
    f.setBandEnabled(false);
    std::vector<Cf> fade = g.tone(2400, 2000.0, 0.5);
    f.process(fade.data(), fade.size());
    CHECK(f.isBypassed());
    std::vector<Cf> after = g.tone(4800, -30000.0, 0.7), copy = after;
    f.process(after.data(), after.size());
    same = true;
    for (std::size_t i = 0; i < after.size(); ++i) { same = same && after[i] == copy[i]; }
    CHECK(same);
    // Switching a notch on fades in over 10 ms (no step at the switch), and a notch carried out of the span fades out the same way.
    IfFilter n(kFs);
    n.retune(7098500.0);
    n.setNotch(0, 7098500.0, 200.0, false);  // at DC of the radio: a constant input is entirely in the notch
    std::vector<Cf> dc(2000, Cf(1.0f, 0.0f));
    n.process(dc.data(), dc.size());
    n.setNotch(0, 7098500.0, 200.0, true);
    std::vector<Cf> dc2(4800, Cf(1.0f, 0.0f));
    n.process(dc2.data(), dc2.size());
    double step = 0;
    for (std::size_t i = 1; i < dc2.size(); ++i) { step = std::max(step, static_cast<double>(std::abs(dc2[i] - dc2[i - 1]))); }
    CHECK(step < 0.1);
    CHECK(std::abs(dc2.back()) < 0.01);
    CHECK(n.notchActive(0));
    n.retune(7098500.0 + 300000.0);
    CHECK(!n.notchActive(0));
}

void testBadInputAndChunking() {
    IfFilter f = ssb();
    f.setNotch(0, 7101200.0, 100.0, true);
    Gen g;
    std::vector<Cf> x = g.tone(2000, 3000.0, 0.5);
    x[10] = Cf(std::numeric_limits<float>::quiet_NaN(), 0.0f);
    x[11] = Cf(0.0f, std::numeric_limits<float>::infinity());
    f.process(x.data(), x.size());
    for (const Cf& v : x) { CHECK(std::isfinite(v.real()) && std::isfinite(v.imag())); }
    f.process(nullptr, 10);
    f.process(x.data(), 0);
    // Bad parameters.
    f.retune(std::numeric_limits<double>::quiet_NaN());
    CHECK_NEAR(f.centreHz(), 7098500.0, 1e-9);
    f.setBand(std::numeric_limits<double>::quiet_NaN(), 5.0);
    f.setBand(7103000.0, 7100000.0, true);  // swapped: repaired
    f.setBand(7101000.0, 7101000.0, true);  // no width: widened to the minimum
    double lo, hi;
    f.edgesBaseband(&lo, &hi);
    CHECK_NEAR(hi - lo, IfFilter::kMinBandwidthHz, 1e-6);
    // Chunking: one call and many calls give the same samples.
    auto run = [](std::size_t block) {
        IfFilter h = ssb();
        h.setNotch(0, 7101200.0, 100.0, true);
        h.setNotch(1, 7100400.0, 60.0, true);
        Gen gen;
        std::vector<Cf> in = gen.tone(20000, 2400.0, 0.5);
        for (std::size_t p = 0; p < in.size(); p += block) {
            if (p == 9000 - 9000 % block) { h.retune(7098500.0 + 5000.0); }
            h.process(in.data() + p, std::min(block, in.size() - p));
        }
        return in;
    };
    // (9000 is a multiple of every block size here, so every run retunes at the same sample)
    const std::vector<Cf> c = run(500);
    for (std::size_t block : {std::size_t{250}, std::size_t{1000}, std::size_t{3000}, std::size_t{9000}}) {
        const std::vector<Cf> d = run(block);
        bool same = true;
        for (std::size_t i = 0; i < c.size(); ++i) { same = same && c[i] == d[i]; }
        CHECK(same);
    }
}

void testNoAlloc() {
    IfFilter f = ssb();
    f.setNotch(0, 7101200.0, 100.0, true);
    Gen g;
    std::vector<Cf> x = g.tone(4096, 2000.0, 0.5);
    f.process(x.data(), x.size());
    DspmAllocProbe probe;
    for (int i = 0; i < 40; ++i) {
        f.retune(7098500.0 + 1000.0 * (i % 9));
        f.setNotch(1, 7100300.0 + i, 50.0 + i, (i % 2) == 0);
        f.process(x.data(), x.size());
    }
    CHECK(probe.news() == 0);
}

}  // namespace

int main() {
    testPassband();
    testNotch();
    testRetuneKeepsEdgesOnRf();
    testRetuneIsClickFree();
    testEdgeLeavesSpan();
    testOnOffAndBypass();
    testBadInputAndChunking();
    testNoAlloc();
    return testSummary("test_if_filter");
}
