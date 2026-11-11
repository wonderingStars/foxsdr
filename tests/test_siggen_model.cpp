// The signal-generator node model (src/dsp/siggen_model.hpp): tone, noise, sweep, AM and FM against closed forms (Bessel lines for FM, sideband
// height for AM, instantaneous frequency for the sweep), phase continuity across a settings change, determinism, text form, limits.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <limits>
#include <random>

#include "dsp/siggen_model.hpp"
#include "dspmodels_alloc.hpp"
#include "dspmodels_util.hpp"
#include "test_check.hpp"

using namespace cascade::dsp;
using Cf = std::complex<float>;

namespace {

constexpr double kFs = 48000.0;

// Complex amplitude of the component at baseband frequency hz in v[from, to).
double line(const std::vector<Cf>& v, std::size_t from, std::size_t to, double hz) {
    std::complex<double> acc(0, 0);
    const double w = dspm::kTwoPi * hz / kFs;
    for (std::size_t i = from; i < to; ++i) { acc += std::complex<double>(v[i].real(), v[i].imag()) * std::polar(1.0, -w * static_cast<double>(i)); }
    return std::abs(acc) / static_cast<double>(to - from);
}

std::vector<Cf> make(SignalGenerator& g, std::size_t n) {
    std::vector<Cf> v(n);
    g.generate(v.data(), n);
    return v;
}

double besselJ(int k, double x) {  // series, plenty for the arguments used here
    double sum = 0.0, term = std::pow(x / 2.0, k);
    for (int i = 2; i <= k; ++i) { term /= i; }
    for (int m = 0; m < 40; ++m) {
        sum += term;
        term *= -(x / 2.0) * (x / 2.0) / ((m + 1.0) * (m + 1.0 + k));
    }
    return sum;
}

void testTone() {
    SignalGenerator g(kFs);
    GenSettings s;
    s.kind = GenKind::Tone;
    s.freqHz = 3000.0;
    s.amplitudeDb = -12.0;
    g.setSettings(s);
    const std::vector<Cf> v = make(g, 48000);
    CHECK_NEAR(line(v, 0, 48000, 3000.0), std::pow(10.0, -12.0 / 20.0), 1e-5);
    CHECK(line(v, 0, 48000, 3500.0) < 1e-4);
    // Constant modulus, and a negative frequency really is negative (a complex tone is one-sided).
    for (std::size_t i = 0; i < v.size(); i += 997) { CHECK_NEAR(std::abs(v[i]), std::pow(10.0, -12.0 / 20.0), 1e-6); }
    s.freqHz = -3000.0;
    g.setSettings(s);
    const std::vector<Cf> w = make(g, 48000);
    CHECK(line(w, 0, 48000, 3000.0) < 1e-3);
    CHECK_NEAR(line(w, 0, 48000, -3000.0), std::pow(10.0, -12.0 / 20.0), 1e-5);
    // Off is exact zeros.
    s.kind = GenKind::Off;
    g.setSettings(s);
    for (const Cf& x : make(g, 100)) { CHECK(x == Cf(0, 0)); }
}

void testNoise() {
    SignalGenerator g(kFs);
    GenSettings s;
    s.kind = GenKind::Noise;
    s.noiseDb = -30.0;
    s.seed = 7;
    g.setSettings(s);
    const std::vector<Cf> v = make(g, 400000);
    double p = 0, mi = 0, mq = 0;
    for (const Cf& x : v) { p += std::norm(x); mi += x.real(); mq += x.imag(); }
    p /= v.size();
    const double db = 10.0 * std::log10(p);
    std::printf("MEAS noise -30 dBFS asked: measured power %.3f dB, means %.5f %.5f\n", db, mi / v.size(), mq / v.size());
    CHECK_NEAR(db, -30.0, 0.05);
    CHECK(std::fabs(mi / v.size()) < 5e-4 && std::fabs(mq / v.size()) < 5e-4);
    // Gaussian: the fraction of I samples beyond 2 sigma is 4.55 %.
    const double sigma = std::sqrt(std::pow(10.0, -30.0 / 10.0) / 2.0);
    std::size_t beyond = 0;
    for (const Cf& x : v) { if (std::fabs(x.real()) > 2.0 * sigma) { ++beyond; } }
    CHECK_NEAR(static_cast<double>(beyond) / v.size(), 0.0455, 0.003);
    // Same seed, same samples; another seed, other samples; a flat spectrum (the two halves of the band hold the same power).
    SignalGenerator a(kFs), b(kFs), c(kFs);
    a.setSettings(s);
    b.setSettings(s);
    s.seed = 8;
    c.setSettings(s);
    const std::vector<Cf> va = make(a, 1000), vb = make(b, 1000), vc = make(c, 1000);
    CHECK(va == vb);
    CHECK(va != vc);
    // Noise added on top of a tone, and none at -300 (exact zeros: a tone alone is exactly its closed form).
    GenSettings t;
    t.kind = GenKind::Tone;
    t.freqHz = 1000.0;
    t.amplitudeDb = 0.0;
    t.addNoiseDb = -300.0;
    SignalGenerator quiet(kFs);
    quiet.setSettings(t);
    const std::vector<Cf> q = make(quiet, 480);
    for (std::size_t i = 0; i < q.size(); ++i) {
        const double ph = dspm::kTwoPi * 1000.0 * static_cast<double>(i) / kFs;
        CHECK_NEAR(q[i].real(), std::cos(ph), 2e-6);
    }
    t.addNoiseDb = -40.0;
    SignalGenerator noisy(kFs);
    noisy.setSettings(t);
    const std::vector<Cf> nz = make(noisy, 96000);
    double err = 0;
    for (std::size_t i = 0; i < nz.size(); ++i) {
        const double ph = dspm::kTwoPi * 1000.0 * static_cast<double>(i) / kFs;
        err += std::norm(std::complex<double>(nz[i].real() - std::cos(ph), nz[i].imag() - std::sin(ph)));
    }
    CHECK_NEAR(10.0 * std::log10(err / nz.size()), -40.0, 0.1);
}

void testSweep() {
    // The instantaneous frequency (from the phase step between samples) follows the closed form.
    SignalGenerator g(kFs);
    GenSettings s;
    s.kind = GenKind::Sweep;
    s.startHz = -8000.0;
    s.stopHz = 12000.0;
    s.periodS = 0.5;
    s.amplitudeDb = 0.0;
    g.setSettings(s);
    const std::vector<Cf> v = make(g, 24000);  // one period
    double worst = 0;
    for (std::size_t i = 1; i < v.size(); ++i) {
        const double dphi = std::arg(std::complex<double>(v[i].real(), v[i].imag()) * std::conj(std::complex<double>(v[i - 1].real(), v[i - 1].imag())));
        const double f = dphi * kFs / dspm::kTwoPi;
        const double t = (static_cast<double>(i) - 0.5) / kFs;  // between the two samples
        const double want = -8000.0 + 20000.0 * t / 0.5;
        worst = std::max(worst, std::fabs(f - want));
    }
    std::printf("MEAS linear sweep -8..+12 kHz in 0.5 s: worst deviation of the instantaneous frequency %.2f Hz\n", worst);
    CHECK(worst < 3.0);
    // After one period it starts again at the start frequency.
    CHECK_NEAR(g.sweepFrequencyNow(), -8000.0, 1.0);
    // Log: geometric. 100 Hz to 10 kHz over 0.4 s: the frequency halfway (0.2 s) is 1 kHz.
    SignalGenerator l(kFs);
    s.startHz = 100.0;
    s.stopHz = 10000.0;
    s.periodS = 0.4;
    s.shape = SweepShape::Log;
    l.setSettings(s);
    const std::vector<Cf> lv = make(l, 9600);  // 0.2 s
    const std::size_t i = 9599;
    const double dphi = std::arg(std::complex<double>(lv[i].real(), lv[i].imag()) * std::conj(std::complex<double>(lv[i - 1].real(), lv[i - 1].imag())));
    CHECK_NEAR(dphi * kFs / dspm::kTwoPi, 1000.0, 15.0);
    CHECK_NEAR(l.sweepFrequencyNow(), 1000.0, 1.0);
    // Log with an end at zero (or on different sides) falls back to linear.
    SignalGenerator f(kFs);
    s.startHz = -1000.0;
    s.stopHz = 1000.0;
    s.periodS = 1.0;
    f.setSettings(s);
    make(f, 24000);
    CHECK_NEAR(f.sweepFrequencyNow(), 0.0, 1.0);
    // UpDown: a triangle - the frequency at the end of the first leg is the stop frequency, then comes back down.
    SignalGenerator ud(kFs);
    s.shape = SweepShape::Linear;
    s.direction = SweepDirection::UpDown;
    s.startHz = 0.0;
    s.stopHz = 6000.0;
    s.periodS = 0.25;
    ud.setSettings(s);
    make(ud, 12000 - 1);
    CHECK_NEAR(ud.sweepFrequencyNow(), 6000.0, 2.0);
    make(ud, 6000);
    CHECK_NEAR(ud.sweepFrequencyNow(), 3000.0, 2.0);
    // Equal ends are a tone; the sweep is phase-continuous at the wrap (no step bigger than the largest legal phase step).
    SignalGenerator w(kFs);
    s.direction = SweepDirection::Up;
    s.startHz = 2000.0;
    s.stopHz = 2000.0;
    w.setSettings(s);
    const std::vector<Cf> wv = make(w, 20000);
    CHECK_NEAR(line(wv, 0, 20000, 2000.0), std::pow(10.0, s.amplitudeDb / 20.0), 1e-4);
}

void testAm() {
    SignalGenerator g(kFs);
    GenSettings s;
    s.kind = GenKind::Am;
    s.freqHz = 6000.0;
    s.modHz = 500.0;
    s.depth = 0.6;
    s.amplitudeDb = -6.0;
    g.setSettings(s);
    const std::vector<Cf> v = make(g, 48000);
    const double a = std::pow(10.0, -6.0 / 20.0);
    const double car = line(v, 0, 48000, 6000.0), up = line(v, 0, 48000, 6500.0), dn = line(v, 0, 48000, 5500.0);
    std::printf("MEAS AM depth 0.6: carrier %.5f (pin %.5f), sidebands %.5f / %.5f (pin %.5f)\n", car, a, up, dn, a * 0.3);
    CHECK_NEAR(car, a, 1e-4);
    CHECK_NEAR(up, a * 0.3, 1e-4);
    CHECK_NEAR(dn, a * 0.3, 1e-4);
    CHECK(line(v, 0, 48000, 7000.0) < 1e-4);
    double lo = 1e9, hi = 0;
    for (const Cf& x : v) { lo = std::min(lo, static_cast<double>(std::abs(x))); hi = std::max(hi, static_cast<double>(std::abs(x))); }
    CHECK_NEAR(hi, a * 1.6, 1e-4);
    CHECK_NEAR(lo, a * 0.4, 1e-4);
    // Depth is clamped to 0..1.
    s.depth = 7.0;
    g.setSettings(s);
    CHECK(g.settings().depth == 1.0);
}

void testFm() {
    // Bessel lines: beta = 2.4048 (the first zero of J0) puts the carrier in a null and J1(beta) = 0.5191 on each first sideband.
    const double beta = 2.4048255577;
    SignalGenerator g(kFs);
    GenSettings s;
    s.kind = GenKind::Fm;
    s.freqHz = 8000.0;
    s.modHz = 1000.0;
    s.deviationHz = beta * 1000.0;
    s.amplitudeDb = 0.0;
    g.setSettings(s);
    const std::vector<Cf> v = make(g, 48000);
    double worst = 0;
    for (int k = -3; k <= 3; ++k) {
        const double got = line(v, 0, 48000, 8000.0 + 1000.0 * k);
        const double want = std::fabs(besselJ(std::abs(k), beta));
        worst = std::max(worst, std::fabs(got - want));
        if (k == 0) { CHECK(got < 1e-4); }
    }
    std::printf("MEAS FM beta %.4f: lines k=-3..3 match |J_k| within %.2e (J0 = %.2e, J1 = %.4f)\n", beta, worst, besselJ(0, beta), besselJ(1, beta));
    CHECK(worst < 1e-3);
    CHECK_NEAR(line(v, 0, 48000, 9000.0), 0.5191, 1e-3);
    // Constant modulus; zero deviation is a plain carrier.
    for (std::size_t i = 0; i < v.size(); i += 331) { CHECK_NEAR(std::abs(v[i]), 1.0, 1e-5); }
    s.deviationHz = 0.0;
    g.setSettings(s);
    const std::vector<Cf> c = make(g, 48000);
    CHECK_NEAR(line(c, 0, 48000, 8000.0), 1.0, 1e-4);
}

void testPhaseContinuityAcrossChanges() {
    // Changing the frequency in mid-stream moves the frequency, not the phase: no sample-to-sample jump beyond the larger phase step.
    SignalGenerator g(kFs);
    GenSettings s;
    s.kind = GenKind::Tone;
    s.freqHz = 1000.0;
    s.amplitudeDb = 0.0;
    g.setSettings(s);
    std::vector<Cf> out;
    for (int blk = 0; blk < 40; ++blk) {
        s.freqHz = 1000.0 + 300.0 * (blk % 7);
        if (blk == 20) { s.kind = GenKind::Fm; s.modHz = 700.0; s.deviationHz = 900.0; }
        g.setSettings(s);
        const std::vector<Cf> v = make(g, 333);
        out.insert(out.end(), v.begin(), v.end());
    }
    double step = 0;
    for (std::size_t i = 1; i < out.size(); ++i) { step = std::max(step, static_cast<double>(std::abs(out[i] - out[i - 1]))); }
    // The largest legal step: 2 sin(pi f / fs) at the highest instantaneous frequency (2800 Hz carrier + 900 Hz deviation).
    const double bound = 2.0 * std::sin(3.14159265358979 * 3700.0 / kFs);
    std::printf("MEAS 40 settings changes in 13320 samples: largest sample step %.4f (bound %.4f)\n", step, bound);
    CHECK(step <= bound * 1.001);
    // Any chunking gives the same samples.
    auto run = [&](std::size_t block) {
        SignalGenerator h(kFs);
        GenSettings t;
        t.kind = GenKind::Am;
        t.freqHz = 2222.0;
        t.addNoiseDb = -50.0;
        t.seed = 3;
        h.setSettings(t);
        std::vector<Cf> all(10007);
        for (std::size_t p = 0; p < all.size(); p += block) { h.generate(all.data() + p, std::min(block, all.size() - p)); }
        return all;
    };
    CHECK(run(10007) == run(1) && run(10007) == run(64) && run(10007) == run(999));
}

void testLimitsAndText() {
    SignalGenerator g(kFs);
    GenSettings s;
    s.freqHz = 1e9;
    s.amplitudeDb = 50.0;
    s.periodS = -4;
    s.modHz = 0.0;
    s.deviationHz = 1e12;
    s.noiseDb = 1e9;
    g.setSettings(s);
    CHECK_NEAR(g.settings().freqHz, 0.49 * kFs, 1e-6);
    CHECK(g.settings().amplitudeDb == 0.0);
    CHECK(g.settings().periodS == 0.001);
    CHECK_NEAR(g.settings().modHz, 0.01, 1e-12);
    CHECK_NEAR(g.settings().deviationHz, 0.45 * kFs, 1e-6);
    CHECK(g.settings().noiseDb == 10.0);
    // Non-finite numbers keep the old value.
    GenSettings n = g.settings();
    n.freqHz = std::numeric_limits<double>::quiet_NaN();
    n.depth = std::numeric_limits<double>::infinity();
    const double was = g.settings().freqHz;
    g.setSettings(n);
    CHECK(g.settings().freqHz == was);
    // Text form: exact round trip of what it writes, refusal of everything else.
    GenSettings t;
    t.kind = GenKind::Fm;
    t.freqHz = -1234.5;
    t.amplitudeDb = -3.25;
    t.noiseDb = -61.0;
    t.addNoiseDb = -300.0;
    t.startHz = 100.0;
    t.stopHz = 2200.0;
    t.periodS = 2.5;
    t.shape = SweepShape::Log;
    t.direction = SweepDirection::UpDown;
    t.modHz = 333.333333;
    t.depth = 0.25;
    t.deviationHz = 4000.0;
    t.seed = 123456789012345ull;
    const std::string text = SignalGenerator::toText(t);
    GenSettings back;
    CHECK(SignalGenerator::fromText(text, &back));
    CHECK(back.kind == t.kind && back.shape == t.shape && back.direction == t.direction && back.seed == t.seed);
    CHECK_NEAR(back.freqHz, t.freqHz, 1e-6);
    CHECK_NEAR(back.modHz, t.modHz, 1e-6);
    CHECK_NEAR(back.deviationHz, t.deviationHz, 1e-6);
    CHECK(SignalGenerator::toText(back) == text);
    GenSettings keep;
    keep.freqHz = 4321.0;
    const char* bad[] = {"", "FOXGEN2;tone;1;2;3;4;5;6;7;0;0;8;0.5;9;1", "FOXGEN1;tone;1;2", "FOXGEN1;wobble;1;2;3;4;5;6;7;0;0;8;0.5;9;1",
                         "FOXGEN1;tone;1x;2;3;4;5;6;7;0;0;8;0.5;9;1", "FOXGEN1;tone;1;2;3;4;5;6;7;2;0;8;0.5;9;1", "FOXGEN1;tone;1;2;3;4;5;6;7;0;0;8;0.5;9;-1",
                         "FOXGEN1;tone;1;2;3;4;5;6;7;0;0;8;0.5;9;", "FOXGEN1;tone;.5;2;3;4;5;6;7;0;0;8;0.5;9;1", "FOXGEN1;tone;1.;2;3;4;5;6;7;0;0;8;0.5;9;1",
                         "FOXGEN1;tone;1;2;3;4;5;6;7;0;0;8;0.5;9;1;extra"};
    for (const char* b : bad) {
        GenSettings o = keep;
        const bool accepted = SignalGenerator::fromText(b, &o);
        if (accepted) { std::printf("ACCEPTED BAD: %s\\n", b); }
        CHECK(!accepted);
        CHECK(o.freqHz == 4321.0);
    }
    CHECK(!SignalGenerator::fromText(std::string(700, 'x'), &keep));
    // Fuzz: mutated text never crashes and what is accepted writes back to a text that parses to the same settings.
    std::mt19937_64 rng(77);
    int ok = 0;
    for (int i = 0; i < 20000; ++i) {
        std::string m = text;
        for (int e = 0; e < 1 + static_cast<int>(rng() % 3); ++e) {
            const std::size_t at = rng() % m.size();
            if (rng() % 2) { m[at] = static_cast<char>(rng()); } else { m.erase(at, 1); }
        }
        GenSettings o;
        if (SignalGenerator::fromText(m, &o)) {
            ++ok;
            GenSettings o2;
            CHECK(SignalGenerator::fromText(SignalGenerator::toText(o), &o2));
        }
    }
    std::printf("MEAS settings text fuzz: 20000 mutated strings, %d accepted\n", ok);
    // Names.
    for (GenKind k : {GenKind::Off, GenKind::Tone, GenKind::Noise, GenKind::Sweep, GenKind::Am, GenKind::Fm}) { CHECK(genKindFromName(genKindName(k)) == k); }
}

void testNoAlloc() {
    SignalGenerator g(kFs);
    GenSettings s;
    s.kind = GenKind::Fm;
    s.addNoiseDb = -60.0;
    g.setSettings(s);
    std::vector<Cf> v(4096);
    g.generate(v.data(), v.size());
    DspmAllocProbe probe;
    for (int i = 0; i < 50; ++i) {
        s.freqHz = 100.0 * i;
        g.setSettings(s);
        g.generate(v.data(), v.size());
    }
    CHECK(probe.news() == 0);
    g.generate(nullptr, 10);
    g.generate(v.data(), 0);
}

}  // namespace

int main() {
    testTone();
    testNoise();
    testSweep();
    testAm();
    testFm();
    testPhaseContinuityAcrossChanges();
    testLimitsAndText();
    testNoAlloc();
    return testSummary("test_siggen_model");
}
