// I/Q correction (src/dsp/iq_correct.hpp): DC removal, blind gain/phase balance, swap flag. A quadrature imbalance is GENERATED
// (gain g, phase phi, DC offset) on a known signal and the image rejection is measured by DFT before and after.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <complex>
#include <cstdio>
#include <limits>
#include <random>

#include "alloc_probe.hpp"
#include "blk4_util.hpp"
#include "dsp/iq_correct.hpp"
#include "test_check.hpp"

using cascade::dsp::IqCorrector;
using cd = std::complex<double>;
using cf = std::complex<float>;

namespace {
constexpr double kTwoPi = 6.283185307179586476925286766559;
constexpr std::size_t kN = 8192;

// Clean signal: tone A at fs/8 (bin 1024 of 8192), a tone B at -0.21 fs and noise; then the imbalance and a DC offset.
struct Gen {
    double fs, g, phi;
    cd dc;
    std::mt19937 rng{5};
    std::normal_distribution<double> nd{0.0, 1.0};
    double sigma = 0.0001, ampA = 0.5, ampB = 0.3, fB;
    std::size_t n = 0;
    Gen(double f, double gain, double phaseRad, cd d) : fs(f), g(gain), phi(phaseRad), dc(d), fB(-0.21 * f) {}
    cf next() {
        const double t = static_cast<double>(n++);
        const cd x = std::polar(ampA, kTwoPi * t / 8.0) + std::polar(ampB, kTwoPi * fB * t / fs) + cd(sigma * nd(rng), sigma * nd(rng));
        const double I = x.real(), Q = x.imag();
        const cd y(I, g * (Q * std::cos(phi) + I * std::sin(phi)));
        const cd o = y + dc;
        return cf(static_cast<float>(o.real()), static_cast<float>(o.imag()));
    }
    void fill(std::vector<cf>& v) { for (auto& s : v) { s = next(); } }
};

cd dftBin(const std::vector<cf>& v, std::size_t from, long k) {
    cd s = 0;
    for (std::size_t i = 0; i < kN; ++i) {
        const double w = 0.5 - 0.5 * std::cos(kTwoPi * static_cast<double>(i) / static_cast<double>(kN));
        s += cd(v[from + i].real(), v[from + i].imag()) * w * std::polar(1.0, -kTwoPi * static_cast<double>(k) * static_cast<double>(i) / static_cast<double>(kN));
    }
    return s;
}

// Image rejection of the main tone (bin 1024 and its mirror), dB, over the last kN samples of v.
double irrDb(const std::vector<cf>& v) {
    const std::size_t from = v.size() - kN;
    const double sig = std::norm(dftBin(v, from, 1024)), img = std::norm(dftBin(v, from, -1024));
    return 10.0 * std::log10(sig / std::max(img, 1e-30));
}

std::vector<cf> runGen(IqCorrector* c, double fs, double g, double phiDeg, cd dc, double seconds) {
    Gen gen(fs, g, phiDeg * kTwoPi / 360.0, dc);
    const std::size_t n = static_cast<std::size_t>(seconds * fs);
    std::vector<cf> v(n);
    gen.fill(v);
    if (c) {
        for (std::size_t p = 0; p < n; p += 4096) { c->process(v.data() + p, std::min<std::size_t>(4096, n - p)); }
    }
    return v;
}

void testImageRejection() {
    struct Row { double g, phi; };
    for (double fs : {250000.0, 2400000.0}) {
        for (Row r : {Row{0.9, 3.0}, Row{0.8, 10.0}, Row{1.0, 2.0}, Row{1.1, -6.0}, Row{0.97, 0.0}}) {
            const std::vector<cf> raw = runGen(nullptr, fs, r.g, r.phi, 0.0, 0.2);
            IqCorrector c(fs);
            c.setBalance(true);
            const std::vector<cf> fixed = runGen(&c, fs, r.g, r.phi, 0.0, fs > 1e6 ? 4.0 : 6.0);
            const double before = irrDb(raw), after = irrDb(fixed);
            std::printf("MEAS fs %.0f, gain error %.2f dB (g %.2f), phase %.1f deg: image rejection before %.1f dB, after %.1f dB (estimator says input was %.1f dB)\n",
                        fs, 20.0 * std::log10(r.g), r.g, r.phi, before, after, c.imageRejectionDb());
            CHECK(after > before + 20.0);
            CHECK(after > 80.0);
            // The block keeps the total POWER it was given (it cannot know the true gain): the tone ends at the level of the
            // imbalanced input, sqrt((1 + g^2) / 2) of the clean one.
            const double a = std::abs(dftBin(fixed, fixed.size() - kN, 1024)) / (0.5 * static_cast<double>(kN)) ;  // Hann coherent gain 0.5
            CHECK_NEAR(20.0 * std::log10(a / 0.5), 20.0 * std::log10(std::sqrt((1.0 + r.g * r.g) / 2.0)), 0.25);
        }
    }
}

void testDc() {
    const double fs = 250000.0;
    IqCorrector c(fs);
    c.setDcRemoval(true);
    const std::vector<cf> out = runGen(&c, fs, 1.0, 0.0, cd(0.05, -0.03), 6.0);
    cd m = 0;
    for (std::size_t i = out.size() - 65536; i < out.size(); ++i) { m += cd(out[i].real(), out[i].imag()); }
    m /= 65536.0;
    // Mean of the clean signal over the window is nonzero by noise only.
    std::printf("MEAS DC offset (0.05, -0.03) = %.1f dBFS: residual mean %.2e (%.1f dB down), estimator holds (%.4f, %.4f)\n", 20 * std::log10(std::abs(cd(0.05, -0.03))),
                std::abs(m), 20 * std::log10(std::abs(cd(0.05, -0.03)) / std::abs(m)), c.dcEstimate().real(), c.dcEstimate().imag());
    CHECK(std::abs(m) < 5e-4);
    // A low-frequency tone survives: 20 Hz at fs 250 kHz is passed within 1 dB (the cut-off is 0.3 Hz).
    IqCorrector c2(fs);
    c2.setDcRemoval(true);
    std::vector<cf> t(static_cast<std::size_t>(6 * fs));
    for (std::size_t i = 0; i < t.size(); ++i) { t[i] = std::polar(0.2f, static_cast<float>(kTwoPi * 20.0 * static_cast<double>(i) / fs)); }
    c2.process(t.data(), t.size());
    double mx = 0;
    for (std::size_t i = t.size() - 20000; i < t.size(); ++i) { mx = std::max(mx, static_cast<double>(std::abs(t[i]))); }
    std::printf("MEAS 20 Hz tone (0.2) after DC removal: peak %.4f\n", mx);
    CHECK_NEAR(mx, 0.2, 0.01);
}

void testSwap() {
    const double fs = 250000.0;
    IqCorrector c(fs);
    c.setSwap(true);
    std::vector<cf> v(static_cast<std::size_t>(0.5 * fs));
    for (std::size_t i = 0; i < v.size(); ++i) { v[i] = std::polar(0.5f, static_cast<float>(kTwoPi * static_cast<double>(i) / 8.0)); }  // +fs/8
    c.process(v.data(), v.size());
    const double pos = std::abs(dftBin(v, v.size() - kN, 1024)), neg = std::abs(dftBin(v, v.size() - kN, -1024));
    std::printf("MEAS swap: tone +fs/8 afterwards: positive bin %.4f, negative bin %.1f (mirrored)\n", pos, neg);
    CHECK(neg > 1000.0 * std::max(pos, 1e-9));
    // Swap twice = identity (after both crossfades).
    IqCorrector c2(fs);
    std::vector<cf> a(20000);
    for (std::size_t i = 0; i < a.size(); ++i) { a[i] = cf(static_cast<float>(0.1 * std::sin(i * 0.3)), static_cast<float>(0.2 * std::cos(i * 0.17))); }
    std::vector<cf> b = a;
    c2.setSwap(true);
    c2.process(b.data(), 10000);
    c2.setSwap(false);
    c2.process(b.data() + 10000, 10000);
    bool same = true;
    for (std::size_t i = 14000; i < 20000; ++i) { same = same && std::abs(b[i] - a[i]) < 1e-6f; }
    CHECK(same);
    // The crossfade: a tone is swapped on mid-stream; the output never jumps by more than the tone's own largest step (instant swap would).
    IqCorrector c3(fs);
    std::vector<cf> t(40000);
    for (std::size_t i = 0; i < t.size(); ++i) { t[i] = std::polar(0.5f, static_cast<float>(kTwoPi * 0.01 * static_cast<double>(i))); }  // slow tone, 0.01 cycles/sample
    c3.process(t.data(), 20000);
    c3.setSwap(true);
    c3.process(t.data() + 20000, 20000);
    double mxStep = 0;
    for (std::size_t i = 1; i < t.size(); ++i) { mxStep = std::max(mxStep, static_cast<double>(std::abs(t[i] - t[i - 1]))); }
    const double own = 0.5 * kTwoPi * 0.01;
    std::printf("MEAS swap on under a slow tone: largest sample step %.4f, tone's own %.4f (ratio %.2f)\n", mxStep, own, mxStep / own);
    CHECK(mxStep < 3.0 * own);  // a crossfade passes through the mirror image; an instant swap would be a step up to the full amplitude (1.0)
}

void testSwitching() {
    const double fs = 250000.0;
    // DC removal switched on under a small tone with a big DC: the offset leaves over 20 ms, no jump.
    IqCorrector c(fs);
    std::vector<cf> v(static_cast<std::size_t>(3 * fs));
    for (std::size_t i = 0; i < v.size(); ++i) { v[i] = cf(0.3f, 0.0f) + std::polar(0.01f, static_cast<float>(kTwoPi * 5000.0 * static_cast<double>(i) / fs)); }
    c.process(v.data(), static_cast<std::size_t>(fs));  // off: untouched
    c.setDcRemoval(true);
    c.process(v.data() + static_cast<std::size_t>(fs), static_cast<std::size_t>(2 * fs));
    double mxStep = 0;
    for (std::size_t i = static_cast<std::size_t>(fs) - 5; i < v.size(); ++i) { mxStep = std::max(mxStep, static_cast<double>(std::abs(v[i] - v[i - 1]))); }
    const double own = 0.01 * kTwoPi * 5000.0 / fs;
    std::printf("MEAS DC removal on under a 0.3 DC with a 0.01 tone: largest step %.5f, the tone's own %.5f (ratio %.2f)\n", mxStep, own, mxStep / own);
    CHECK(mxStep < 1.5 * own);
    // Off again: the correction ramps out, the output returns to the raw samples.
    c.setDcRemoval(false);
    std::vector<cf> w(static_cast<std::size_t>(0.5 * fs), cf(0.3f, 0.0f));
    c.process(w.data(), w.size());
    CHECK(c.isBypassed());
    CHECK(std::abs(w.back() - cf(0.3f, 0.0f)) < 1e-6f);
}

void testBasics() {
    // All off: bit-exact, no work.
    IqCorrector c(250000.0);
    std::vector<cf> a(5000), b;
    for (std::size_t i = 0; i < a.size(); ++i) { a[i] = cf(static_cast<float>(std::sin(i * 0.1)), static_cast<float>(std::cos(i * 0.07))); }
    b = a;
    c.process(b.data(), b.size());
    CHECK(a == b);
    CHECK(c.isBypassed());
    // Non-finite input reads as 0.
    c.setDcRemoval(true);
    c.setBalance(true);
    std::vector<cf> bad(2000, cf(std::numeric_limits<float>::quiet_NaN(), 0.0f));
    c.process(bad.data(), bad.size());
    bool fin = true;
    for (auto s : bad) { fin = fin && std::isfinite(s.real()) && std::isfinite(s.imag()); }
    CHECK(fin);
    // A circular signal with no imbalance is left alone: image rejection estimate stays high, tone level unchanged.
    IqCorrector bal(250000.0);
    bal.setBalance(true);
    const std::vector<cf> clean = runGen(&bal, 250000.0, 1.0, 0.0, 0.0, 6.0);
    std::printf("MEAS balanced input: image rejection after %.1f dB, estimator says %.1f dB\n", irrDb(clean), bal.imageRejectionDb());
    CHECK(irrDb(clean) > 55.0);
    // Silence: no division by zero, no output.
    IqCorrector z(250000.0);
    z.setBalance(true);
    z.setDcRemoval(true);
    std::vector<cf> zero(5000, cf(0.0f, 0.0f));
    z.process(zero.data(), zero.size());
    CHECK(zero[4999] == cf(0.0f, 0.0f));
    // A real-valued (improper) signal is held at the correction limit, not turned into garbage: output stays finite and bounded.
    IqCorrector rl(250000.0);
    rl.setBalance(true);
    std::vector<cf> real(100000);
    for (std::size_t i = 0; i < real.size(); ++i) { real[i] = cf(static_cast<float>(std::sin(i * 0.2)), 0.0f); }
    rl.process(real.data(), real.size());
    double mx = 0;
    for (auto s : real) { mx = std::max(mx, static_cast<double>(std::abs(s))); }
    CHECK(mx < 3.0);
    CHECK(std::abs(rl.gammaEstimate()) <= 0.3 + 1e-9);
    // Chunking.
    std::vector<cf> in = runGen(nullptr, 250000.0, 0.9, 3.0, cd(0.02, 0.01), 1.0);
    IqCorrector w(250000.0);
    w.setBalance(true); w.setDcRemoval(true); w.setSwap(true);
    std::vector<cf> whole = in;
    w.process(whole.data(), whole.size());
    for (std::size_t blk : {1u, 99u, 4096u}) {
        IqCorrector v(250000.0);
        v.setBalance(true); v.setDcRemoval(true); v.setSwap(true);
        std::vector<cf> o = in;
        for (std::size_t p = 0; p < o.size(); p += blk) { v.process(o.data() + p, std::min<std::size_t>(blk, o.size() - p)); }
        CHECK(o == whole);
    }
    // Allocation, and cost per sample (all on / all off) at the source rate.
    IqCorrector al(250000.0);
    std::vector<cf> blk(4096, cf(0.1f, 0.05f));
    long news = 0;
    for (int i = 0; i < 40; ++i) {
        AllocProbe p;
        al.setDcRemoval(i % 2 == 0);
        al.setBalance(i % 3 != 0);
        al.setSwap(i % 5 == 0);
        al.process(blk.data(), blk.size());
        news += p.news();
    }
    std::printf("MEAS operator new in 40 process calls with switching: %ld\n", news);
    CHECK(news == 0);
    std::vector<cf> big(1 << 20);
    for (std::size_t i = 0; i < big.size(); ++i) { big[i] = cf(static_cast<float>(std::sin(i * 0.1)), static_cast<float>(std::cos(i * 0.07))); }
    IqCorrector on(2400000.0), off(2400000.0);
    on.setDcRemoval(true);
    on.setBalance(true);
    auto t0 = std::chrono::steady_clock::now();
    off.process(big.data(), big.size());
    auto t1 = std::chrono::steady_clock::now();
    on.process(big.data(), big.size());
    auto t2 = std::chrono::steady_clock::now();
    const double nsOff = std::chrono::duration<double, std::nano>(t1 - t0).count() / static_cast<double>(big.size());
    const double nsOn = std::chrono::duration<double, std::nano>(t2 - t1).count() / static_cast<double>(big.size());
    std::printf("MEAS cost per sample: all off %.3f ns, DC + balance on %.1f ns (2.4 MS/s needs under 400 ns)\n", nsOff, nsOn);
    CHECK(nsOff * 20.0 < nsOn);
    CHECK(nsOn < 400.0);
}
}  // namespace

int main() {
    testImageRejection();
    testDc();
    testSwap();
    testSwitching();
    testBasics();
    return testSummary("test_iq_correct");
}
