// Tests for the impulse noise blanker (dsp/noise_blanker.hpp), its hook in the
// Vfo, and the signal generator's impulses (source/siggen.hpp) it is proven with.
//
// THE FIXTURE. A weak tone (0.003 of full scale, 1 kHz off the centre) under a
// noise floor (about 57 dB below the clicks), and a click train: 100 a second,
// three samples wide, 0.3 of full scale - the kind of thing ignition does to a
// weak signal. The measure is the power left in the tone's own neighbourhood
// once the tone itself is taken away (see residualPower): that is what the
// narrow filter lets through to the listener, and it is where a click that is
// not blanked is heard and where a blanked tone's gap is paid for.
//
// REFERENCES. Nothing is compared against a constant read back from the
// implementation: the tone and the click train are made here by the generator,
// the residual is measured here by mixing, averaging and subtracting a mean,
// and the no-harm claims are identities (output = input delayed) or dB figures
// against the unblanked path.
//
// REAL IMPULSE NOISE IS NOT IN THIS FILE. These are generated clicks only; an
// ignition or fence recording has not been run through this (see the record).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "dsp/noise_blanker.hpp"
#include "dsp/vfo.hpp"
#include "source/siggen.hpp"

#include "test_check.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

namespace {

using cascade::dsp::NoiseBlanker;
using cascade::dsp::Vfo;
using cascade::source::SigGen;
using C = std::complex<float>;

constexpr double kTwoPi = 6.283185307179586476925286766559;
constexpr double kChannelHz = 200000.0;
constexpr double kToneHz = 1000.0;
constexpr float kToneDb = -50.46f;     // 0.003
constexpr float kNoiseDb = -67.4f;     // 3e-4 per component
constexpr float kClickDb = -10.46f;    // 0.3
constexpr double kClickRate = 100.0;
constexpr int kClickWidth = 3;

float amp(float db) { return std::pow(10.0f, db / 20.0f); }

// Mixes the tone to DC, averages `avgLen` samples (a crude 3 kHz-wide filter
// where the stream is still wide; 1 for a stream a filter has already narrowed),
// removes the mean (the tone itself) and returns the power left, skipping the
// first `skip` samples. What is left is whatever is NOT the steady tone:
// noise, unblanked clicks, and the gaps a blanker cuts into the tone.
double residualPower(const std::vector<C>& y, double fs, double toneHz, std::size_t skip,
                     std::size_t avgLen) {
    std::vector<C> m(y.size());
    for (std::size_t i = 0; i < y.size(); ++i) {
        const double ph = -kTwoPi * toneHz * static_cast<double>(i) / fs;
        m[i] = y[i] * C(static_cast<float>(std::cos(ph)), static_cast<float>(std::sin(ph)));
    }
    std::vector<C> a(y.size());
    C run(0.0f, 0.0f);
    for (std::size_t i = 0; i < m.size(); ++i) {
        run += m[i];
        if (i >= avgLen) { run -= m[i - avgLen]; }
        a[i] = run / static_cast<float>(avgLen);
    }
    std::complex<double> mean(0.0, 0.0);
    std::size_t cnt = 0;
    for (std::size_t i = skip + avgLen; i < a.size(); ++i) { mean += std::complex<double>(a[i]); ++cnt; }
    mean /= static_cast<double>(cnt);
    double acc = 0.0;
    for (std::size_t i = skip + avgLen; i < a.size(); ++i) { acc += std::norm(std::complex<double>(a[i]) - mean); }
    return acc / static_cast<double>(cnt);
}

double db(double p) { return 10.0 * std::log10(p); }

bool sameBits(const C& a, const C& b) {
    return std::memcmp(&a, &b, sizeof(C)) == 0;
}

// The fixture stream at `fs`: tone + noise + (optionally) clicks.
std::vector<C> makeFixture(double fs, std::size_t n, bool clicks, double rate = kClickRate,
                           float clickDb = kClickDb, int width = kClickWidth) {
    SigGen g(fs);
    g.setTone(0, kToneHz, kToneDb);
    g.setNoiseFloorDb(kNoiseDb);
    if (clicks) { g.setImpulses(rate, clickDb, width); }
    std::vector<C> x(n);
    g.generate(x.data(), n);
    return x;
}

// ---------------------------------------------------------------------------

void testSigGenImpulses() {
    const double fs = 200000.0;
    // Impulses alone: where they are, how many, how big.
    SigGen g(fs);
    g.setImpulses(1000.0, -6.0f, 4);   // period 200, width 4
    std::vector<C> x(2000);
    g.generate(x.data(), x.size());
    std::size_t nonzero = 0;
    bool placed = true;
    for (std::size_t i = 0; i < x.size(); ++i) {
        const bool want = (i % 200) < 4;
        if (want) { placed = placed && std::fabs(x[i].real() - amp(-6.0f)) < 1e-6f && x[i].imag() == 0.0f; }
        else { placed = placed && x[i] == C(0.0f, 0.0f); }
        if (x[i] != C(0.0f, 0.0f)) { ++nonzero; }
    }
    CHECK(placed);
    CHECK(nonzero == 10 * 4);

    // Chunking invariance, bit for bit, with tones and noise under it.
    SigGen whole(fs);
    whole.setTone(0, 1234.0, -30.0f);
    whole.setNoiseFloorDb(-60.0f);
    whole.setImpulses(333.0, -20.0f, 3);
    SigGen chunked(fs);
    chunked.setTone(0, 1234.0, -30.0f);
    chunked.setNoiseFloorDb(-60.0f);
    chunked.setImpulses(333.0, -20.0f, 3);
    std::vector<C> a(5000);
    std::vector<C> b(5000);
    whole.generate(a.data(), a.size());
    for (std::size_t pos = 0; pos < b.size();) {
        const std::size_t m = std::min<std::size_t>(b.size() - pos, 1 + (pos * 7) % 311);
        chunked.generate(b.data() + pos, m);
        pos += m;
    }
    bool same = true;
    for (std::size_t i = 0; i < a.size(); ++i) { same = same && sameBits(a[i], b[i]); }
    CHECK(same);

    // Off by default: identical to a generator that never heard of them. Off
    // again after clearImpulses and for a nonpositive rate or width < 1.
    SigGen plain(fs);
    plain.setTone(0, 1234.0, -30.0f);
    plain.setNoiseFloorDb(-60.0f);
    SigGen cleared(fs);
    cleared.setTone(0, 1234.0, -30.0f);
    cleared.setNoiseFloorDb(-60.0f);
    cleared.setImpulses(500.0, -10.0f, 3);
    cleared.clearImpulses();
    SigGen zeroRate(fs);
    zeroRate.setTone(0, 1234.0, -30.0f);
    zeroRate.setNoiseFloorDb(-60.0f);
    zeroRate.setImpulses(0.0, -10.0f, 3);
    SigGen zeroWidth(fs);
    zeroWidth.setTone(0, 1234.0, -30.0f);
    zeroWidth.setNoiseFloorDb(-60.0f);
    zeroWidth.setImpulses(500.0, -10.0f, 0);
    std::vector<C> p(3000), q(3000), r(3000), s(3000);
    plain.generate(p.data(), p.size());
    cleared.generate(q.data(), q.size());
    zeroRate.generate(r.data(), r.size());
    zeroWidth.generate(s.data(), s.size());
    bool off = true;
    for (std::size_t i = 0; i < p.size(); ++i) {
        off = off && sameBits(p[i], q[i]) && sameBits(p[i], r[i]) && sameBits(p[i], s[i]);
    }
    CHECK(off);
}

// Which stage the blanker works on, for every shape of Vfo: section 6 of the design.
void testPlacement() {
    // Narrow (3 kHz of a 204.8 kHz channel, below a quarter): the last stage is
    // the narrow filter and the blanker goes in front of it.
    {
        Vfo v(2048000.0, 10, 3000.0);
        CHECK(v.stageCount() == 2);
        CHECK(v.noiseBlankerStage() == v.stageCount() - 2);
    }
    // Not narrow (150 kHz): no extra stage, the blanker is on the final output.
    {
        Vfo v(2048000.0, 10, 150000.0);
        CHECK(v.stageCount() == 1);
        CHECK(v.noiseBlankerStage() == v.stageCount() - 1);
    }
    // Channel rate = input rate (decimation 1): one filter at the input rate,
    // plus the narrow one when narrow; the blanker is after the first.
    {
        Vfo narrow(200000.0, 1, 3000.0);
        CHECK(narrow.stageCount() == 2);
        CHECK(narrow.noiseBlankerStage() == 0);
        Vfo wide(200000.0, 1, 150000.0);
        CHECK(wide.stageCount() == 1);
        CHECK(wide.noiseBlankerStage() == 0);
    }
    // Staged design (>= 4 MS/s) with a narrow bandwidth: several alias stages,
    // the channel stage, the narrow stage; the blanker follows the channel stage.
    {
        Vfo v(10000000.0, 50, 3000.0);
        CHECK(v.stageCount() == 4);
        CHECK(v.noiseBlankerStage() == 2);
    }
    // A redesign moves it: widening the bandwidth takes the narrow stage away.
    {
        Vfo v(2048000.0, 10, 3000.0);
        CHECK(v.noiseBlankerStage() == 0);
        v.setBandwidthHz(150000.0);
        CHECK(v.stageCount() == 1);
        CHECK(v.noiseBlankerStage() == 0);
        v.setBandwidthHz(3000.0);
        CHECK(v.stageCount() == 2);
        CHECK(v.noiseBlankerStage() == 0);
    }
}

void testBlockImprovement() {
    const std::size_t n = static_cast<std::size_t>(1.5 * kChannelHz);
    const std::vector<C> x = makeFixture(kChannelHz, n, true);
    NoiseBlanker nb(kChannelHz);
    CHECK(!nb.enabled());                       // off by default
    CHECK_NEAR(nb.thresholdDb(), 15.0, 1e-6);  // default threshold
    const std::size_t L = nb.lookAheadSamples();
    CHECK(L == 30);  // 0.15 ms at 200 kHz

    const std::size_t M = static_cast<std::size_t>(kChannelHz / 3000.0);
    const double off = residualPower(x, kChannelHz, kToneHz, 20000, M);

    nb.setEnabled(true);
    std::vector<C> y(n);
    nb.process(x.data(), y.data(), n);
    const double on = residualPower(y, kChannelHz, kToneHz, 20000, M);
    const double improvement = db(off) - db(on);
    std::printf("  block fixture: residual off %.2f dB, on %.2f dB, improvement %.2f dB, blanked %.2f%%\n",
                db(off), db(on), improvement, nb.blankedPercent());
    // The floor measured with the 0.025 ms guards (24.2 dB; 20.95 dB with 0.05 ms, pinned at 15), same margin.
    CHECK(improvement > 18.0);
    CHECK(nb.blankedPercent() > 0.5f);
    CHECK(nb.blankedPercent() < 3.0f);
    CHECK(!nb.standingDown());

    // The same fixture with no clicks: nothing is blanked, nothing is lost.
    const std::vector<C> clean = makeFixture(kChannelHz, n, false);
    NoiseBlanker nb2(kChannelHz);
    nb2.setEnabled(true);
    std::vector<C> y2(n);
    nb2.process(clean.data(), y2.data(), n);
    const double cleanOff = residualPower(clean, kChannelHz, kToneHz, 20000, M);
    const double cleanOn = residualPower(y2, kChannelHz, kToneHz, 20000, M);
    std::printf("  no clicks: residual off %.2f dB, on %.2f dB\n", db(cleanOff), db(cleanOn));
    CHECK(std::fabs(db(cleanOff) - db(cleanOn)) < 0.1);

    // And a click the threshold must not see: 0.01 on a 0.003 tone is 11 dB
    // over the average magnitude, under the 15 dB default. It is left alone:
    // the share stays zero.
    const std::vector<C> small = makeFixture(kChannelHz, n, true, kClickRate, -40.0f, 3);
    NoiseBlanker nb3(kChannelHz);
    nb3.setEnabled(true);
    std::vector<C> y3(n);
    nb3.process(small.data(), y3.data(), n);
    CHECK(nb3.blankedPercent() < 0.01f);
}

void testNoHarm() {
    const std::size_t n = static_cast<std::size_t>(0.8 * kChannelHz);
    // (a) A clean tone with noise: output is the input delayed, to the bit,
    // once the first look-ahead and ramp have passed; and within 0.1 dB in power.
    const std::vector<C> clean = makeFixture(kChannelHz, n, false);
    NoiseBlanker nb(kChannelHz);
    nb.setEnabled(true);
    std::vector<C> y(n);
    nb.process(clean.data(), y.data(), n);
    const std::size_t L = nb.lookAheadSamples();
    std::size_t differ = 0;
    double pIn = 0.0;
    double pOut = 0.0;
    for (std::size_t i = 100; i < n; ++i) {
        if (!sameBits(y[i], clean[i - L])) { ++differ; }
        pIn += std::norm(clean[i - L]);
        pOut += std::norm(y[i]);
    }
    CHECK(differ == 0);
    CHECK(std::fabs(db(pOut) - db(pIn)) < 0.1);
    CHECK(nb.blankedPercent() == 0.0f);

    // (b) A strongly modulated AM signal (100% depth, 5 kHz tone) with no
    // impulses: a share of exactly zero.
    std::vector<C> am(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / kChannelHz;
        am[i] = C(static_cast<float>(0.05 * (1.0 + std::cos(kTwoPi * 5000.0 * t))), 0.0f);
    }
    NoiseBlanker nb2(kChannelHz);
    nb2.setEnabled(true);
    std::vector<C> ya(n);
    nb2.process(am.data(), ya.data(), n);
    CHECK(nb2.blankedPercent() == 0.0f);
    CHECK(!nb2.standingDown());
    std::size_t amDiffer = 0;
    for (std::size_t i = 100; i < n; ++i) { if (!sameBits(ya[i], am[i - L])) { ++amDiffer; } }
    CHECK(amDiffer == 0);

    // (c) The same through the Vfo and the narrow filter: switching the blanker
    // on with no clicks about changes the filtered signal by less than 0.1 dB.
    const std::size_t nIn = static_cast<std::size_t>(0.6 * 2048000.0);
    const std::vector<C> in = makeFixture(2048000.0, nIn, false);
    Vfo off(2048000.0, 10, 3000.0);
    Vfo on(2048000.0, 10, 3000.0);
    on.setNoiseBlankerEnabled(true);
    std::vector<C> oa(nIn / 10 + 1), ob(nIn / 10 + 1);
    const std::size_t ma = off.process(in.data(), nIn, oa.data(), oa.size());
    const std::size_t mb = on.process(in.data(), nIn, ob.data(), ob.size());
    CHECK(ma == mb);
    double pa = 0.0;
    double pb = 0.0;
    for (std::size_t i = 5000; i < ma; ++i) { pa += std::norm(oa[i]); pb += std::norm(ob[i]); }
    std::printf("  Vfo no-harm: power off %.3f dB, on %.3f dB (difference %.4f dB)\n", db(pa), db(pb),
                db(pb) - db(pa));
    CHECK(std::fabs(db(pa) - db(pb)) < 0.1);
    CHECK(on.noiseBlankerBlankedPercent() == 0.0f);
}

void testStandDown() {
    const std::size_t M = static_cast<std::size_t>(kChannelHz / 3000.0);
    // Dense: 3000 clicks a second of 3 samples is 4.5% of the samples as clicks,
    // and ~60% of the stream once guards and ramps are added. The signal passes.
    {
        const std::size_t n = static_cast<std::size_t>(1.2 * kChannelHz);
        const std::vector<C> x = makeFixture(kChannelHz, n, true, 3000.0);
        NoiseBlanker nb(kChannelHz);
        nb.setEnabled(true);
        std::vector<C> y(n);
        nb.process(x.data(), y.data(), n);
        CHECK(nb.standingDown());
        CHECK(nb.blankedPercent() == 0.0f);   // the readout says nothing is cut
        // Passing means passing: bit-identical to the delayed input over the
        // last 0.6 s (the first 0.1 s is where it decides).
        const std::size_t L = nb.lookAheadSamples();
        std::size_t differ = 0;
        for (std::size_t i = n / 2; i < n; ++i) { if (!sameBits(y[i], x[i - L])) { ++differ; } }
        CHECK(differ == 0);
    }
    // Resume: dense clicks, then sparse ones - it stands down, then works again.
    {
        const std::size_t half = static_cast<std::size_t>(0.8 * kChannelHz);
        std::vector<C> a = makeFixture(kChannelHz, half, true, 3000.0);
        std::vector<C> b = makeFixture(kChannelHz, 2 * half, true, 100.0);
        NoiseBlanker nb(kChannelHz);
        nb.setEnabled(true);
        std::vector<C> y(half);
        nb.process(a.data(), y.data(), half);
        CHECK(nb.standingDown());
        std::vector<C> y2(b.size());
        nb.process(b.data(), y2.data(), b.size());
        CHECK(!nb.standingDown());
        CHECK(nb.blankedPercent() > 0.5f);
        // ... and the sparse part really is cleaned, to within 6 dB of the
        // fixture's own figure.
        const std::vector<C> tail(y2.begin() + static_cast<std::ptrdiff_t>(half), y2.end());
        const std::vector<C> tailIn(b.begin() + static_cast<std::ptrdiff_t>(half), b.end());
        const double offP = residualPower(tailIn, kChannelHz, kToneHz, 0, M);
        const double onP = residualPower(tail, kChannelHz, kToneHz, 0, M);
        std::printf("  resume after stand-down: improvement %.2f dB\n", db(offP) - db(onP));
        CHECK(db(offP) - db(onP) > 15.2);
    }
    // A strong signal that appears (40 dB step) is not a click: after the
    // average catches up, it is passed whole and the share is zero.
    {
        const std::size_t quiet = static_cast<std::size_t>(0.1 * kChannelHz);
        const std::size_t loud = static_cast<std::size_t>(1.0 * kChannelHz);
        std::vector<C> x(quiet + loud);
        for (std::size_t i = 0; i < x.size(); ++i) {
            const double ph = kTwoPi * 1000.0 * static_cast<double>(i) / kChannelHz;
            const float a = i < quiet ? 0.001f : 0.1f;
            x[i] = C(static_cast<float>(a * std::cos(ph)), static_cast<float>(a * std::sin(ph)));
        }
        NoiseBlanker nb(kChannelHz);
        nb.setEnabled(true);
        std::vector<C> y(x.size());
        nb.process(x.data(), y.data(), x.size());
        const std::size_t L = nb.lookAheadSamples();
        std::size_t differ = 0;
        for (std::size_t i = quiet + static_cast<std::size_t>(0.5 * kChannelHz); i < x.size(); ++i) {
            if (!sameBits(y[i], x[i - L])) { ++differ; }
        }
        CHECK(differ == 0);
        CHECK(!nb.standingDown());
        CHECK(nb.blankedPercent() == 0.0f);
    }
}

void testChunkingAndState() {
    const std::size_t n = 120000;
    const std::vector<C> x = makeFixture(kChannelHz, n, true);
    NoiseBlanker whole(kChannelHz);
    whole.setEnabled(true);
    std::vector<C> a(n);
    whole.process(x.data(), a.data(), n);
    NoiseBlanker parts(kChannelHz);
    parts.setEnabled(true);
    std::vector<C> b(n);
    for (std::size_t pos = 0; pos < n;) {
        const std::size_t m = std::min<std::size_t>(n - pos, 1 + (pos * 13) % 997);
        parts.process(x.data() + pos, b.data() + pos, m);
        pos += m;
    }
    std::size_t differ = 0;
    for (std::size_t i = 0; i < n; ++i) { if (!sameBits(a[i], b[i])) { ++differ; } }
    CHECK(differ == 0);
    // In place gives the same samples.
    NoiseBlanker inPlace(kChannelHz);
    inPlace.setEnabled(true);
    std::vector<C> c = x;
    inPlace.process(c.data(), c.data(), n);
    differ = 0;
    for (std::size_t i = 0; i < n; ++i) { if (!sameBits(a[i], c[i])) { ++differ; } }
    CHECK(differ == 0);

    // Threshold: clamped, NaN is the default.
    NoiseBlanker t(kChannelHz);
    t.setThresholdDb(2.0f);
    CHECK_NEAR(t.thresholdDb(), 6.0, 1e-6);
    t.setThresholdDb(80.0f);
    CHECK_NEAR(t.thresholdDb(), 30.0, 1e-6);
    t.setThresholdDb(20.0f);
    CHECK_NEAR(t.thresholdDb(), 20.0, 1e-6);
    t.setThresholdDb(std::numeric_limits<float>::quiet_NaN());
    CHECK_NEAR(t.thresholdDb(), 15.0, 1e-6);

    // A higher threshold leaves a click that a lower one takes: the 0.3 click
    // is 40 dB over the 0.003 tone, so 30 dB still sees it and a click 20 dB
    // down (0.03, 20 dB over the tone) is seen at 15 and not at 30.
    const std::vector<C> mid = makeFixture(kChannelHz, n, true, kClickRate, -30.5f, 3);
    NoiseBlanker lo(kChannelHz);
    lo.setEnabled(true);
    lo.setThresholdDb(15.0f);
    NoiseBlanker hi(kChannelHz);
    hi.setEnabled(true);
    hi.setThresholdDb(30.0f);
    std::vector<C> o1(n), o2(n);
    lo.process(mid.data(), o1.data(), n);
    hi.process(mid.data(), o2.data(), n);
    CHECK(lo.blankedPercent() > 0.5f);
    CHECK(hi.blankedPercent() < 0.01f);

    // Disabling and re-enabling starts clean (no old delay line, no old average).
    NoiseBlanker cycle(kChannelHz);
    cycle.setEnabled(true);
    cycle.process(x.data(), a.data(), 50000);
    cycle.setEnabled(false);
    CHECK(cycle.blankedPercent() == 0.0f);   // off reads zero
    cycle.setEnabled(true);
    cycle.process(x.data(), b.data(), 50000);
    NoiseBlanker fresh(kChannelHz);
    fresh.setEnabled(true);
    std::vector<C> f(50000);
    fresh.process(x.data(), f.data(), 50000);
    differ = 0;
    for (std::size_t i = 0; i < 50000; ++i) { if (!sameBits(b[i], f[i])) { ++differ; } }
    CHECK(differ == 0);

    // A different rate: lengths follow it, the look-ahead is still ~0.15 ms.
    NoiseBlanker r(150000.0);
    CHECK(r.lookAheadSamples() == 23 || r.lookAheadSamples() == 22);
    r.setSampleRateHz(300000.0);
    CHECK(r.lookAheadSamples() == 45);
}

void testNonFinite() {
    const std::size_t n = 60000;
    std::vector<C> x = makeFixture(kChannelHz, n, false);
    x[20000] = C(std::numeric_limits<float>::quiet_NaN(), 0.0f);
    x[20001] = C(std::numeric_limits<float>::infinity(), 1.0f);
    x[30000] = C(0.0f, -std::numeric_limits<float>::infinity());
    NoiseBlanker nb(kChannelHz);
    nb.setEnabled(true);
    std::vector<C> y(n);
    nb.process(x.data(), y.data(), n);
    std::size_t bad = 0;
    for (const C& v : y) { if (!std::isfinite(v.real()) || !std::isfinite(v.imag())) { ++bad; } }
    CHECK(bad == 0);
    // The blanker is still tracking afterwards: the tail is the delayed input.
    const std::size_t L = nb.lookAheadSamples();
    std::size_t differ = 0;
    for (std::size_t i = 40000; i < n; ++i) { if (!sameBits(y[i], x[i - L])) { ++differ; } }
    CHECK(differ == 0);
}

// The hook in the Vfo, through the narrow filter.
void testVfoHook() {
    const double fs = 2048000.0;
    const std::size_t n = static_cast<std::size_t>(1.2 * fs);
    // Clicks at the INPUT rate: 20 samples (10 us) of 0.3 at 100 a second.
    SigGen g(fs);
    g.setTone(0, kToneHz, kToneDb);
    g.setNoiseFloorDb(kNoiseDb);
    g.setImpulses(100.0, kClickDb, 20);
    std::vector<C> x(n);
    g.generate(x.data(), n);

    Vfo off(fs, 10, 3000.0);
    Vfo on(fs, 10, 3000.0);
    on.setNoiseBlankerEnabled(true);
    CHECK(on.noiseBlankerEnabled());
    CHECK(!off.noiseBlankerEnabled());
    std::vector<C> a(n / 10 + 1), b(n / 10 + 1);
    const std::size_t ma = off.process(x.data(), n, a.data(), a.size());
    const std::size_t mb = on.process(x.data(), n, b.data(), b.size());
    CHECK(ma == mb);
    a.resize(ma);
    b.resize(mb);
    const double chan = fs / 10.0;
    const double offP = residualPower(a, chan, kToneHz, 20000, 1);
    const double onP = residualPower(b, chan, kToneHz, 20000, 1);
    std::printf("  Vfo through the 3 kHz filter: residual off %.2f dB, on %.2f dB, improvement %.2f dB, blanked %.2f%%\n",
                db(offP), db(onP), db(offP) - db(onP), on.noiseBlankerBlankedPercent());
    CHECK(db(offP) - db(onP) > 17.9);
    CHECK(on.noiseBlankerBlankedPercent() > 0.5f);
    CHECK(on.noiseBlankerBlankedPercent() < 5.0f);

    // Disabled, the Vfo is what it was: a Vfo that had the blanker switched on
    // and off again gives the same samples as one that never did.
    Vfo never(fs, 10, 3000.0);
    Vfo toggled(fs, 10, 3000.0);
    toggled.setNoiseBlankerEnabled(true);
    toggled.setNoiseBlankerEnabled(false);
    std::vector<C> c(n / 10 + 1), d(n / 10 + 1);
    const std::size_t mc = never.process(x.data(), n, c.data(), c.size());
    const std::size_t md = toggled.process(x.data(), n, d.data(), d.size());
    CHECK(mc == md);
    std::size_t differ = 0;
    for (std::size_t i = 0; i < mc; ++i) { if (!sameBits(c[i], d[i])) { ++differ; } }
    CHECK(differ == 0);

    // Not narrow: the same clicks on the wide (150 kHz) channel output.
    Vfo wideOff(fs, 10, 150000.0);
    Vfo wideOn(fs, 10, 150000.0);
    wideOn.setNoiseBlankerEnabled(true);
    std::vector<C> e(n / 10 + 1), f(n / 10 + 1);
    const std::size_t me = wideOff.process(x.data(), n, e.data(), e.size());
    const std::size_t mf = wideOn.process(x.data(), n, f.data(), f.size());
    e.resize(me);
    f.resize(mf);
    const std::size_t Mw = static_cast<std::size_t>(chan / 3000.0);
    const double wOff = residualPower(e, chan, kToneHz, 20000, Mw);
    const double wOn = residualPower(f, chan, kToneHz, 20000, Mw);
    std::printf("  Vfo wide (no narrow stage): improvement %.2f dB\n", db(wOff) - db(wOn));
    CHECK(db(wOff) - db(wOn) > 11.8);

    // The settings survive a bandwidth change and a reset.
    on.setNoiseBlankerThresholdDb(22.0f);
    on.setBandwidthHz(6000.0);
    CHECK(on.noiseBlankerEnabled());
    CHECK_NEAR(on.noiseBlankerThresholdDb(), 22.0, 1e-6);
    on.reset();
    CHECK(on.noiseBlankerEnabled());
    CHECK_NEAR(on.noiseBlankerThresholdDb(), 22.0, 1e-6);
    CHECK(on.noiseBlankerBlankedPercent() == 0.0f);   // the share restarted
}

}  // namespace

int main() {
    testSigGenImpulses();
    testPlacement();
    testBlockImprovement();
    testNoHarm();
    testStandDown();
    testChunkingAndState();
    testNonFinite();
    testVfoHook();
    return testSummary("test_noise_blanker");
}
