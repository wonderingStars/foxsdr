// Spectrum trace averaging (linear, RMS, log, exponential), smoothing (moving average, Savitzky-Golay) and de-comb (src/dsp/trace_average.hpp).
// Numbers are pinned against closed forms; generated signals only.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <limits>

#include "dsp/trace_average.hpp"
#include "dspmodels_alloc.hpp"
#include "dspmodels_util.hpp"
#include "test_check.hpp"

using namespace cascade::dsp;

namespace {

constexpr std::uint64_t kKey = 7;

std::vector<float> run(TraceAverager& a, const std::vector<std::vector<float>>& frames, double dt = 0.01) {
    double t = 0.0;
    for (const auto& f : frames) {
        a.push(f.data(), static_cast<int>(f.size()), t, kKey);
        t += dt;
    }
    return a.trace();
}

void testClosedForms() {
    // Constant frames: every mode returns the constant.
    for (AvgMode m : {AvgMode::Linear, AvgMode::Rms, AvgMode::Log, AvgMode::Exponential}) {
        TraceAverager a;
        a.setMode(m);
        a.setFrameCount(5);
        const std::vector<float> f(64, -73.5f);
        const std::vector<float> t = run(a, {f, f, f, f, f, f, f});
        CHECK(t.size() == 64);
        CHECK_NEAR(t[10], -73.5, 1e-3);
    }
    // Alternating 0 dB and -20 dB over a window of two: amplitudes 1 and 0.1, powers 1 and 0.01.
    const std::vector<float> hi(8, 0.0f), lo(8, -20.0f);
    {
        TraceAverager a;
        a.setMode(AvgMode::Linear);
        a.setFrameCount(2);
        CHECK_NEAR(run(a, {hi, lo})[3], 20.0 * std::log10(0.55), 1e-4);  // -5.1927
    }
    {
        TraceAverager a;
        a.setMode(AvgMode::Rms);
        a.setFrameCount(2);
        CHECK_NEAR(run(a, {hi, lo})[3], 10.0 * std::log10(0.505), 1e-4);  // -2.9672
    }
    {
        TraceAverager a;
        a.setMode(AvgMode::Log);
        a.setFrameCount(2);
        CHECK_NEAR(run(a, {hi, lo})[3], -10.0, 1e-4);
    }
    std::printf("MEAS alternating 0/-20 dB, window 2: linear %.4f (pin -5.1927), rms %.4f (pin -2.9672), log -10\n", 20.0 * std::log10(0.55), 10.0 * std::log10(0.505));
}

void testWindowSlides() {
    TraceAverager a;
    a.setMode(AvgMode::Log);
    a.setFrameCount(3);
    std::vector<std::vector<float>> fr;
    for (int i = 0; i < 5; ++i) { fr.emplace_back(4, static_cast<float>(-10 * (i + 1))); }  // -10 .. -50
    CHECK_NEAR(run(a, fr)[0], (-30.0 - 40.0 - 50.0) / 3.0, 1e-4);  // the last three only
    CHECK(a.frames() == 3);
}

void testNoiseStatistics() {
    std::mt19937_64 rng(12345);
    constexpr std::size_t n = 16384;
    // One frame: the mean of the dB values of exponentially distributed power sits 2.507 dB under the mean power (Euler's constant).
    const std::vector<float> one = dspm::noiseFrameDb(rng, n, 0.0);
    const double logOne = dspm::meanOf(one, 0, n);
    const double lin1 = [&] { double s = 0; for (float v : one) { s += std::pow(10.0, v / 20.0); } return 20.0 * std::log10(s / n); }();
    std::printf("MEAS one noise frame: mean dB %.3f (pin -2.507), mean-amplitude %.3f dB (pin -1.053), mean power %.3f dB (pin 0)\n", logOne, lin1,
                dspm::meanPowerDb(one, 0, n));
    CHECK_NEAR(logOne, -2.507, 0.06);
    CHECK_NEAR(lin1, 20.0 * std::log10(std::sqrt(3.14159265358979 / 4.0)), 0.06);  // -1.053 dB
    CHECK_NEAR(dspm::meanPowerDb(one, 0, n), 0.0, 0.08);

    constexpr int N = 64;
    std::vector<std::vector<float>> frames;
    for (int i = 0; i < N; ++i) { frames.push_back(dspm::noiseFrameDb(rng, n, 0.0)); }
    struct Row { AvgMode m; double pin; const char* name; };
    for (const Row& r : {Row{AvgMode::Rms, 0.0, "rms"}, Row{AvgMode::Linear, -1.053, "linear"}, Row{AvgMode::Log, -2.507, "log"}}) {
        TraceAverager a;
        a.setMode(r.m);
        a.setFrameCount(N);
        const std::vector<float> t = run(a, frames);
        const double m = dspm::meanOf(t, 0, n);
        std::printf("MEAS %s over %d noise frames: mean trace %.3f dB (pin %.3f)\n", r.name, N, m, r.pin);
        CHECK_NEAR(m, r.pin, 0.1);
    }
    // RMS over 16 frames: the relative spread of the power falls as 1/sqrt(16) = 0.25.
    TraceAverager a;
    a.setMode(AvgMode::Rms);
    a.setFrameCount(16);
    std::vector<std::vector<float>> f16(frames.begin(), frames.begin() + 16);
    const std::vector<float> t = run(a, f16);
    double s = 0, s2 = 0;
    for (float v : t) { const double p = std::pow(10.0, v / 10.0); s += p; s2 += p * p; }
    const double mean = s / n, sd = std::sqrt(s2 / n - mean * mean);
    std::printf("MEAS rms 16 frames: relative power spread %.4f (pin 0.25)\n", sd / mean);
    CHECK_NEAR(sd / mean, 0.25, 0.015);
}

void testExponential() {
    // Closed form: power seed 1, then a step to 0.01 at dt = 0.01 s with tau = 0.1 s: p_k = 0.01 + 0.99 exp(-0.1 k) (alpha = 1 - exp(-dt / tau)).
    TraceAverager a;
    a.setMode(AvgMode::Exponential);
    a.setTimeConstantS(0.1);
    const std::vector<float> hi(4, 0.0f), lo(4, -20.0f);
    double t = 0.0;
    a.push(hi.data(), 4, t, kKey);
    for (int k = 1; k <= 10; ++k) {
        t += 0.01;
        a.push(lo.data(), 4, t, kKey);
        const double p = 0.01 + 0.99 * std::exp(-0.1 * k);
        CHECK_NEAR(a.trace()[0], 10.0 * std::log10(p), 1e-3);
    }
    // FRAME-RATE INDEPENDENCE: one second of the same step at 100 frames/s and at 20 frames/s gives the same trace.
    auto after = [&](double dt) {
        TraceAverager b;
        b.setMode(AvgMode::Exponential);
        b.setTimeConstantS(0.25);
        double tt = 0.0;
        b.push(hi.data(), 4, tt, kKey);
        const int steps = static_cast<int>(std::lround(1.0 / dt));
        for (int k = 0; k < steps; ++k) { tt += dt; b.push(lo.data(), 4, tt, kKey); }
        return b.trace()[0];
    };
    const double fast = after(0.01), slow = after(0.05);
    std::printf("MEAS exponential after 1 s, tau 0.25 s: 100 fps %.4f dB, 20 fps %.4f dB\n", fast, slow);
    CHECK_NEAR(fast, slow, 1e-3);
    // Time standing still or going back: the frame is not counted.
    TraceAverager c;
    c.setMode(AvgMode::Exponential);
    c.setTimeConstantS(0.1);
    c.push(hi.data(), 4, 1.0, kKey);
    c.push(lo.data(), 4, 1.0, kKey);
    c.push(lo.data(), 4, 0.5, kKey);
    CHECK_NEAR(c.trace()[0], 0.0, 1e-4);
    c.push(lo.data(), 4, std::numeric_limits<double>::quiet_NaN(), kKey);
    CHECK_NEAR(c.trace()[0], 0.0, 1e-4);
    // Tau is clamped and NaN is ignored.
    c.setTimeConstantS(std::numeric_limits<double>::quiet_NaN());
    CHECK_NEAR(c.timeConstantS(), 0.1, 1e-12);
    c.setTimeConstantS(1e6);
    CHECK_NEAR(c.timeConstantS(), TraceAverager::kMaxTauS, 1e-9);
    c.setTimeConstantS(-3);
    CHECK_NEAR(c.timeConstantS(), TraceAverager::kMinTauS, 1e-12);
}

void testAxisAndEdges() {
    TraceAverager a;
    a.setMode(AvgMode::Rms);
    a.setFrameCount(4);
    std::vector<float> f(32, -50.0f);
    a.push(f.data(), 32, 0.0, 1);
    a.push(f.data(), 32, 0.1, 1);
    CHECK(a.frames() == 2);
    a.push(f.data(), 32, 0.2, 2);  // another axis: starts again
    CHECK(a.frames() == 1);
    std::vector<float> g(48, -50.0f);
    a.push(g.data(), 48, 0.3, 2);  // another bin count: starts again
    CHECK(a.frames() == 1 && a.trace().size() == 48);
    // Non-finite bins count as the floor, never as a peak or NaN in the trace.
    std::vector<float> bad(8, std::numeric_limits<float>::quiet_NaN());
    bad[2] = std::numeric_limits<float>::infinity();
    TraceAverager b;
    b.setMode(AvgMode::Linear);
    b.setFrameCount(2);
    b.push(bad.data(), 8, 0.0, 1);
    for (float v : b.trace()) { CHECK(std::isfinite(v) && v <= -199.0f); }
    // Off, null and empty input leave it empty.
    TraceAverager off;
    off.push(f.data(), 32, 0.0, 1);
    CHECK(off.trace().empty());
    TraceAverager on;
    on.setMode(AvgMode::Log);
    on.push(nullptr, 5, 0.0, 1);
    on.push(f.data(), 0, 0.0, 1);
    on.push(f.data(), -3, 0.0, 1);
    CHECK(on.trace().empty() && on.frames() == 0);
    // Count clamp.
    on.setFrameCount(0);
    on.push(f.data(), 32, 0.0, 1);
    on.push(g.data(), 32, 0.0, 1);  // (a window of one frame)
    CHECK(on.frames() == 1);
    on.setFrameCount(100000);
    // Memory bound: four million bins keep at most kAvgMaxCells / bins frames.
    TraceAverager big;
    big.setMode(AvgMode::Rms);
    big.setFrameCount(kAvgMaxFrames);
    std::vector<float> huge(4u * 1024u * 1024u, -60.0f);
    for (int i = 0; i < 5; ++i) { big.push(huge.data(), static_cast<int>(huge.size()), 0.1 * i, 3); }
    CHECK(big.frames() <= kAvgMaxCells / huge.size());
    CHECK(big.frames() >= 1);
    // Names.
    for (AvgMode m : {AvgMode::Off, AvgMode::Linear, AvgMode::Rms, AvgMode::Log, AvgMode::Exponential}) { CHECK(avgModeFromName(avgModeName(m)) == m); }
    CHECK(avgModeFromName("nonsense") == AvgMode::Off && avgModeFromName(nullptr) == AvgMode::Off);
}

void testNoDriftAndNoAlloc() {
    std::mt19937_64 rng(99);
    constexpr std::size_t n = 256;
    TraceAverager a;
    a.setMode(AvgMode::Rms);
    a.setFrameCount(8);
    std::vector<std::vector<float>> all;
    for (int i = 0; i < 5000; ++i) {
        all.push_back(dspm::noiseFrameDb(rng, n, -80.0));
        a.push(all.back().data(), n, 0.01 * i, kKey);
    }
    TraceAverager fresh;
    fresh.setMode(AvgMode::Rms);
    fresh.setFrameCount(8);
    std::vector<std::vector<float>> last(all.end() - 8, all.end());
    const std::vector<float> ref = run(fresh, last);
    double worst = 0;
    for (std::size_t i = 0; i < n; ++i) { worst = std::max(worst, std::fabs(static_cast<double>(a.trace()[i]) - ref[i])); }
    std::printf("MEAS 5000 frames through an 8-frame window: worst difference from a fresh average %.2e dB\n", worst);
    CHECK(worst < 1e-3);
    // No allocation once the size is settled.
    const std::vector<float>& f = all.back();
    DspmAllocProbe probe;
    for (int i = 0; i < 50; ++i) { a.push(f.data(), n, 100.0 + i, kKey); }
    CHECK(probe.news() == 0);
}

// ------------------------------------------------------------ smoothing

void testMovingAverage() {
    std::vector<float> x(21, 0.0f), y(21);
    x[10] = 21.0f;
    smoothMovingAverage(x.data(), y.data(), 21, 7);
    for (int i = 0; i < 21; ++i) { CHECK_NEAR(y[i], (i >= 7 && i <= 13) ? 3.0 : 0.0, 1e-5); }  // the impulse spreads to 1/7 each
    // A ramp is unchanged in the interior; the edges average what exists.
    std::vector<float> r(30), o(30);
    for (int i = 0; i < 30; ++i) { r[i] = static_cast<float>(i); }
    smoothMovingAverage(r.data(), o.data(), 30, 5);
    for (int i = 2; i < 28; ++i) { CHECK_NEAR(o[i], i, 1e-5); }
    CHECK_NEAR(o[0], (0 + 1 + 2) / 3.0, 1e-5);
    CHECK_NEAR(o[29], (27 + 28 + 29) / 3.0, 1e-5);
    // In place; even widths rise by one; width 0/1 copies; NaN counts as the floor; power domain.
    std::vector<float> ip = r;
    smoothMovingAverage(ip.data(), ip.data(), 30, 4);  // = width 5
    for (int i = 2; i < 28; ++i) { CHECK_NEAR(ip[i], i, 1e-5); }
    smoothMovingAverage(r.data(), o.data(), 30, 1);
    CHECK_NEAR(o[17], 17.0, 1e-6);
    std::vector<float> nn(9, -50.0f);
    nn[4] = std::numeric_limits<float>::quiet_NaN();
    std::vector<float> no(9);
    smoothMovingAverage(nn.data(), no.data(), 9, 3);
    for (float v : no) { CHECK(std::isfinite(v)); }
    std::vector<float> pw = {0.0f, -20.0f, 0.0f, -20.0f, 0.0f};
    std::vector<float> po(5);
    smoothMovingAverage(pw.data(), po.data(), 5, 3, true);
    CHECK_NEAR(po[2], 10.0 * std::log10((0.01 + 1.0 + 0.01) / 3.0), 1e-4);
    smoothMovingAverage(nullptr, o.data(), 3, 3);  // nothing to do, nothing thrown
    smoothMovingAverage(r.data(), o.data(), 0, 3);
}

void testSavitzkyGolay() {
    const SavitzkyGolay s5(5, 2);
    const double e5[5] = {-3.0 / 35, 12.0 / 35, 17.0 / 35, 12.0 / 35, -3.0 / 35};
    for (int j = 0; j < 5; ++j) { CHECK_NEAR(s5.centreRow()[j], e5[j], 1e-9); }
    const SavitzkyGolay s7(7, 2);
    const double e7[7] = {-2.0 / 21, 3.0 / 21, 6.0 / 21, 7.0 / 21, 6.0 / 21, 3.0 / 21, -2.0 / 21};
    for (int j = 0; j < 7; ++j) { CHECK_NEAR(s7.centreRow()[j], e7[j], 1e-9); }
    std::printf("MEAS SG centre rows 5/2 and 7/2 match (-3,12,17,12,-3)/35 and (-2,3,6,7,6,3,-2)/21 to 1e-9\n");
    // Order 3 gives the same centre row as order 2 (even/odd pairs).
    const SavitzkyGolay s5o3(5, 3);
    for (int j = 0; j < 5; ++j) { CHECK_NEAR(s5o3.centreRow()[j], e5[j], 1e-9); }
    // Every row sums to one (a constant passes), at every window position.
    const SavitzkyGolay s11(11, 4);
    for (std::size_t pos = 0; pos < 11; ++pos) {
        double sum = 0;
        for (double v : s11.row(pos)) { sum += v; }
        CHECK_NEAR(sum, 1.0, 1e-9);
    }
    // A polynomial of degree <= order passes through unchanged, EDGES INCLUDED.
    std::vector<float> p(60), o(60);
    for (int i = 0; i < 60; ++i) {
        const double x = (i - 30) / 30.0;
        p[i] = static_cast<float>(-70.0 + 20.0 * x - 15.0 * x * x + 8.0 * x * x * x);
    }
    const SavitzkyGolay s9(9, 3);
    s9.apply(p.data(), o.data(), 60);
    double worst = 0;
    for (int i = 0; i < 60; ++i) { worst = std::max(worst, std::fabs(static_cast<double>(o[i]) - p[i])); }
    std::printf("MEAS SG 9/3 on a cubic: worst error %.2e dB (edges included)\n", worst);
    CHECK(worst < 2e-4);
    // White-noise variance falls by the sum of the squared weights (closed form); a peak is kept better than a moving average keeps it.
    std::mt19937_64 rng(5);
    std::normal_distribution<double> g(0.0, 1.0);
    std::vector<float> nz(40000), no(40000);
    for (auto& v : nz) { v = static_cast<float>(g(rng)); }
    s5.apply(nz.data(), no.data(), nz.size());
    double var = 0;
    for (std::size_t i = 10; i < nz.size() - 10; ++i) { var += static_cast<double>(no[i]) * no[i]; }
    var /= static_cast<double>(nz.size() - 20);
    double sw2 = 0;
    for (double v : s5.centreRow()) { sw2 += v * v; }
    std::printf("MEAS SG 5/2 on white noise: variance ratio %.4f (pin %.4f)\n", var, sw2);
    CHECK_NEAR(var, sw2, 0.02);
    std::vector<float> peak(101), sg(101), ma(101);
    for (int i = 0; i < 101; ++i) { peak[i] = static_cast<float>(40.0 * std::exp(-0.5 * (i - 50) * (i - 50) / 9.0)); }  // sigma 3 bins
    SavitzkyGolay(11, 4).apply(peak.data(), sg.data(), 101);
    smoothMovingAverage(peak.data(), ma.data(), 101, 11);
    std::printf("MEAS peak 40 dB sigma 3 bins: SG 11/4 keeps %.2f, moving average 11 keeps %.2f\n", sg[50], ma[50]);
    CHECK(40.0 - sg[50] < 0.5);
    CHECK(40.0 - ma[50] > 3.0);
    // Configuration repair, in place, short input, NaN, empty.
    SavitzkyGolay bad(2, 99);
    CHECK(bad.window() == 3 && bad.order() == 2);
    SavitzkyGolay even(10, 2);
    CHECK(even.window() == 11);
    std::vector<float> in2 = p;
    s9.apply(in2.data(), in2.data(), 60);
    CHECK_NEAR(in2[30], p[30], 2e-4);
    std::vector<float> shortIn = {1, 2, 3, 4, 5, 6, 7}, shortOut(7);
    SavitzkyGolay(21, 2).apply(shortIn.data(), shortOut.data(), 7);  // window shrinks to 7
    for (int i = 0; i < 7; ++i) { CHECK_NEAR(shortOut[i], shortIn[i], 1e-4); }
    std::vector<float> tiny = {5, 6}, tinyOut(2);
    s9.apply(tiny.data(), tinyOut.data(), 2);
    CHECK_NEAR(tinyOut[0], 5.0, 1e-6);
    CHECK_NEAR(tinyOut[1], 6.0, 1e-6);
    std::vector<float> nan(30, -40.0f), nanOut(30);
    nan[15] = std::numeric_limits<float>::quiet_NaN();
    s9.apply(nan.data(), nanOut.data(), 30);
    for (float v : nanOut) { CHECK(std::isfinite(v)); }
    s9.apply(nullptr, nanOut.data(), 30);
    s9.apply(nan.data(), nanOut.data(), 0);
}

// ------------------------------------------------------------- de-comb

constexpr std::size_t kN = 4096;

std::vector<float> combTrace(std::mt19937_64& rng, std::size_t period, double spurDb, double noiseMeanDb, bool averagedNoise) {
    std::vector<float> t;
    if (averagedNoise) {
        // The noise of a trace that was already averaged 16 frames: a spread near 1.4 dB.
        TraceAverager a;
        a.setMode(AvgMode::Rms);
        a.setFrameCount(16);
        for (int i = 0; i < 16; ++i) {
            const std::vector<float> f = dspm::noiseFrameDb(rng, kN, noiseMeanDb);
            a.push(f.data(), static_cast<int>(kN), 0.01 * i, 1);
        }
        t = a.trace();
    } else {
        t = dspm::noiseFrameDb(rng, kN, noiseMeanDb);
    }
    for (std::size_t i = 0; i < kN; i += period) {
        // a spur adds power on top of the noise in its bin
        t[i] = static_cast<float>(10.0 * std::log10(std::pow(10.0, t[i] / 10.0) + std::pow(10.0, (noiseMeanDb + spurDb) / 10.0)));
    }
    return t;
}

double spurExcess(const std::vector<float>& t, std::size_t period, double noiseMeanDb) {
    // mean level at the spur bins minus the mean level of the others, in dB
    double a = 0, b = 0;
    std::size_t na = 0, nb = 0;
    for (std::size_t i = 100; i < kN - 100; ++i) {
        if (i % period == 0) { a += t[i]; ++na; } else { b += t[i]; ++nb; }
    }
    (void)noiseMeanDb;
    return a / static_cast<double>(na) - b / static_cast<double>(nb);
}

void testDeCombRemovesComb() {
    std::mt19937_64 rng(2024);
    for (const bool averaged : {true, false}) {
        const std::vector<float> in = combTrace(rng, 64, 18.0, -100.0, averaged);
        // A real carrier (a wide hump at bin 1777, 25 dB up) must survive.
        std::vector<float> withSig = in;
        for (int k = -3; k <= 3; ++k) { withSig[1777 + k] = std::max(withSig[1777 + k], static_cast<float>(-75.0 - 3.0 * std::abs(k))); }
        DeComb dc;
        std::vector<float> out(kN);
        dc.process(withSig.data(), out.data(), kN);
        const double before = spurExcess(withSig, 64, -100.0), after = spurExcess(out, 64, -100.0);
        std::printf("MEAS de-comb (%s noise): period found %zu, spur excess %.2f dB -> %.2f dB, carrier %.2f -> %.2f dB\n", averaged ? "16-frame" : "single-frame",
                    dc.lastPeriod(), before, after, withSig[1777], out[1777]);
        CHECK(dc.lastPeriod() == 64);
        CHECK(before > 8.0);
        CHECK(after < 0.35 * before);
        CHECK_NEAR(out[1777], withSig[1777], 0.5);  // 1777 is not a multiple of 64 (1777 = 27 x 64 + 49): a signal is not a spur
        CHECK_NEAR(out[1775], withSig[1775], 0.5);
        // The floor between the spurs is untouched on average.
        double dm = 0;
        for (std::size_t i = 100; i < kN - 100; ++i) { if (i % 64 != 0 && (i < 1770 || i > 1784)) { dm += out[i] - withSig[i]; } }
        CHECK(std::fabs(dm / static_cast<double>(kN - 200)) < 0.3);
    }
}

void testDeCombRealFft() {
    // Real FFT frames: noise, a carrier that is not on a bin (with a sample-rate error), and 16 equal tones on exact bins (a comb of period 256).
    std::mt19937_64 rng(77);
    constexpr std::size_t n = 4096;
    std::vector<dspm::ToneSpec> tones;
    for (int k = 0; k < 16; ++k) { tones.push_back({static_cast<double>(k * 256) / n, 0.003}); }
    tones.push_back({0.1234, 0.5});
    TraceAverager avg;
    avg.setMode(AvgMode::Rms);
    avg.setFrameCount(8);
    for (int i = 0; i < 8; ++i) {
        const std::vector<float> f = dspm::fftFrameDb(rng, n, 0.002, tones, 30.0);
        avg.push(f.data(), static_cast<int>(n), 0.02 * i, 5);
    }
    const std::vector<float>& tr = avg.trace();
    DeComb dc;
    DeCombSettings s;
    s.dcBins = 1;
    dc.setSettings(s);
    std::vector<float> out(n);
    dc.process(tr.data(), out.data(), n);
    double sa = 0, sb = 0;
    std::size_t ca = 0, cb = 0;
    double outA = 0, outB = 0;
    for (std::size_t i = 300; i < n - 300; ++i) {
        const bool spur = (i % 256 == 0) && i != n / 2;
        if (std::abs(static_cast<int>(i) - static_cast<int>(0.1234 * n) - static_cast<int>(n / 2)) < 6) { continue; }
        if (spur) { sa += tr[i]; outA += out[i]; ++ca; } else { sb += tr[i]; outB += out[i]; ++cb; }
    }
    const double before = sa / ca - sb / cb, after = outA / ca - outB / cb;
    std::printf("MEAS de-comb on real FFT frames: period %zu, spur excess %.2f dB -> %.2f dB\n", dc.lastPeriod(), before, after);
    CHECK(dc.lastPeriod() == 256);
    CHECK(before > 6.0);
    CHECK(after < 0.3 * before);
}

void testDeCombNoCombPassesExactly() {
    std::mt19937_64 rng(31);
    const std::vector<float> in = dspm::noiseFrameDb(rng, kN, -90.0);
    DeComb dc;
    std::vector<float> out(kN);
    dc.process(in.data(), out.data(), kN);
    CHECK(dc.lastPeriod() == 0);
    bool same = true;
    for (std::size_t i = 0; i < kN; ++i) { same = same && out[i] == in[i]; }
    CHECK(same);  // bit-exact
    // A flat trace (no variance), a short trace and a tiny trace pass through too.
    std::vector<float> flat(kN, -90.0f), fo(kN);
    dc.process(flat.data(), fo.data(), kN);
    CHECK(fo[100] == -90.0f && dc.lastPeriod() == 0);
    std::vector<float> sh(40, -50.0f), so(40);
    dc.process(sh.data(), so.data(), 40);
    CHECK(so[3] == -50.0f);
    dc.process(sh.data(), so.data(), 0);
    dc.process(nullptr, so.data(), 40);
}

void testDeCombOptions() {
    std::mt19937_64 rng(8);
    const std::vector<float> in = combTrace(rng, 100, 20.0, -100.0, true);
    std::vector<float> o1(kN), o2(kN), o0(kN);
    DeComb dc;
    DeCombSettings s;
    s.period = 100;  // told, not found
    dc.setSettings(s);
    dc.process(in.data(), o1.data(), kN);
    CHECK(dc.lastPeriod() == 100);
    s.strength = 0.5;
    dc.setSettings(s);
    dc.process(in.data(), o2.data(), kN);
    s.strength = 0.0;
    dc.setSettings(s);
    dc.process(in.data(), o0.data(), kN);
    const double e0 = spurExcess(o0, 100, -100.0), e1 = spurExcess(o1, 100, -100.0), e2 = spurExcess(o2, 100, -100.0);
    std::printf("MEAS de-comb strength 0 / 0.5 / 1: spur excess %.2f / %.2f / %.2f dB\n", e0, e2, e1);
    CHECK(e0 > 15.0);                       // strength 0 leaves it alone
    CHECK(e2 < 0.65 * e0 && e2 > e1);       // half the profile is taken out
    CHECK(e1 < 0.2 * e0);
    // Out-of-range settings are repaired.
    DeCombSettings bad;
    bad.strength = 7;
    bad.dcBins = 99;
    bad.profileMemory = 5;
    bad.minSpurDb = std::numeric_limits<double>::quiet_NaN();
    dc.setSettings(bad);
    CHECK(dc.settings().strength == 1.0 && dc.settings().dcBins == 9 && dc.settings().profileMemory == 0.99 && dc.settings().minSpurDb == 1.5);
    // DC spike repair: the middle bin and one either side become the line between the bins around them.
    std::vector<float> dcIn(kN, -90.0f), dcOut(kN);
    dcIn[kN / 2] = -20.0f;
    dcIn[kN / 2 - 1] = -60.0f;
    DeCombSettings d;
    d.dcBins = 1;
    dc.setSettings(d);
    dc.process(dcIn.data(), dcOut.data(), kN);
    CHECK_NEAR(dcOut[kN / 2], -90.0, 1e-5);
    CHECK_NEAR(dcOut[kN / 2 - 1], -90.0, 1e-5);
    CHECK_NEAR(dcOut[kN / 2 + 1], -90.0, 1e-5);
    // In place gives the same answer.
    std::vector<float> ip = in;
    DeComb d2;
    DeCombSettings s2;
    s2.period = 100;
    d2.setSettings(s2);
    d2.process(ip.data(), ip.data(), kN);
    bool same = true;
    for (std::size_t i = 0; i < kN; ++i) { same = same && ip[i] == o1[i]; }
    CHECK(same);
    // Non-finite bins are read as the floor.
    std::vector<float> nanIn = in;
    nanIn[500] = std::numeric_limits<float>::quiet_NaN();
    d2.process(nanIn.data(), ip.data(), kN);
    for (float v : ip) { CHECK(std::isfinite(v)); }
}

void testDeCombProfileMemoryAndNoAlloc() {
    std::mt19937_64 rng(404);
    DeComb dc;
    DeCombSettings s;
    s.period = 64;
    s.profileMemory = 0.9;
    dc.setSettings(s);
    std::vector<float> out(kN);
    std::vector<std::vector<float>> frames;
    for (int i = 0; i < 40; ++i) { frames.push_back(combTrace(rng, 64, 15.0, -100.0, false)); }
    for (int i = 0; i < 30; ++i) { dc.process(frames[i].data(), out.data(), kN); }
    // With a long profile memory the single-frame noise averages out of the profile: the spurs go further down than frame by frame.
    DeComb single;
    DeCombSettings s1;
    s1.period = 64;
    single.setSettings(s1);
    std::vector<float> out1(kN);
    single.process(frames[30].data(), out1.data(), kN);
    dc.process(frames[30].data(), out.data(), kN);
    const double eMem = std::fabs(spurExcess(out, 64, -100.0)), eOne = std::fabs(spurExcess(out1, 64, -100.0));
    std::printf("MEAS de-comb after 30 frames with memory 0.9: residual spur excess %.2f dB; frame alone %.2f dB\n", eMem, eOne);
    CHECK(eMem <= eOne + 0.1);
    dc.reset();
    CHECK(dc.lastPeriod() == 0);
    // No allocation once the size is settled.
    DspmAllocProbe probe;
    for (int i = 0; i < 5; ++i) { dc.process(frames[31].data(), out.data(), kN); }
    CHECK(probe.news() == 0);
}

}  // namespace

int main() {
    testClosedForms();
    testWindowSlides();
    testNoiseStatistics();
    testExponential();
    testAxisAndEdges();
    testNoDriftAndNoAlloc();
    testMovingAverage();
    testSavitzkyGolay();
    testDeCombRemovesComb();
    testDeCombRealFft();
    testDeCombNoCombPassesExactly();
    testDeCombOptions();
    testDeCombProfileMemoryAndNoAlloc();
    return testSummary("test_trace_average");
}
