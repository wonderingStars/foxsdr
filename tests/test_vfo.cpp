// Tests for dsp/vfo.hpp: NCO mix -> anti-alias low-pass -> decimate.
//
// Test geometry throughout: inputRate 2 MS/s, decimation 10 -> 200 kHz
// channel. With bandwidth 100 kHz the implementation's harris-rule design
// yields 169 taps, so the filter settling length is 169 input samples
// (~17 channel samples). Every spectral/equivalence check below discards a
// generous 512 channel samples (5120 input samples) of settling first.
//
// SPDX-License-Identifier: MIT
#include "dsp/vfo.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "test_check.hpp"

using cascade::dsp::Vfo;

namespace {

constexpr double kTwoPi = 6.283185307179586476925286766559;
constexpr double kFs = 2.0e6;       // input sample rate
constexpr unsigned kDecim = 10;     // -> 200 kHz channel
constexpr double kChanRate = 200.0e3;
constexpr std::size_t kK = 1024;    // DFT length for channel-domain checks
constexpr std::size_t kSettleOut = 512;  // channel samples discarded >> taps/decim

// Complex tone e^{j 2 pi f n}, f normalized to the generating rate. Phase is
// accumulated in double and wrapped each sample so long signals don't lose
// precision to a growing trig argument.
std::vector<std::complex<float>> tone(std::size_t n, double fNorm) {
    std::vector<std::complex<float>> x(n);
    double ph = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double a = kTwoPi * ph;
        x[i] = std::complex<float>(static_cast<float>(std::cos(a)),
                                   static_cast<float>(std::sin(a)));
        ph += fNorm;
        ph -= std::floor(ph + 0.5);  // wrap to [-0.5, 0.5)
    }
    return x;
}

// Fixed-seed LCG (Numerical Recipes constants) — deterministic test input.
struct Lcg {
    std::uint32_t state;
    explicit Lcg(std::uint32_t seed) : state(seed) {}
    float next() {
        state = state * 1664525u + 1013904223u;
        return (static_cast<float>(state >> 8) / 8388608.0f) - 1.0f;
    }
};

std::vector<std::complex<float>> lcgSignal(std::size_t n, std::uint32_t seed) {
    Lcg rng(seed);
    std::vector<std::complex<float>> x(n);
    for (std::size_t i = 0; i < n; ++i) {
        const float re = rng.next();
        const float im = rng.next();
        x[i] = {re, im};
    }
    return x;
}

// Direct DFT magnitude of all K bins — the definition, independent of the
// implementation under test (and of the project's FFT).
std::vector<double> dftMag(const std::complex<float>* x, std::size_t K) {
    std::vector<double> mag(K);
    for (std::size_t k = 0; k < K; ++k) {
        double re = 0.0;
        double im = 0.0;
        for (std::size_t n = 0; n < K; ++n) {
            const double phi = -kTwoPi * static_cast<double>(k) *
                               static_cast<double>(n) / static_cast<double>(K);
            const double c = std::cos(phi);
            const double s = std::sin(phi);
            const double xr = static_cast<double>(x[n].real());
            const double xi = static_cast<double>(x[n].imag());
            re += xr * c - xi * s;
            im += xr * s + xi * c;
        }
        mag[k] = std::sqrt(re * re + im * im);
    }
    return mag;
}

std::size_t argmax(const std::vector<double>& v) {
    std::size_t best = 0;
    for (std::size_t i = 1; i < v.size(); ++i) {
        if (v[i] > v[best]) { best = i; }
    }
    return best;
}

// Runs the Vfo over x in blocks of `blockSize` with ample per-call capacity
// and returns all channel samples.
std::vector<std::complex<float>> runChunked(Vfo& v,
                                            const std::vector<std::complex<float>>& x,
                                            std::size_t blockSize) {
    std::vector<std::complex<float>> y;
    std::vector<std::complex<float>> buf;
    for (std::size_t pos = 0; pos < x.size(); pos += blockSize) {
        const std::size_t n = std::min(blockSize, x.size() - pos);
        buf.resize(n / kDecim + 1);
        const std::size_t got = v.process(x.data() + pos, n, buf.data(), buf.size());
        CHECK(got <= buf.size());
        y.insert(y.end(), buf.begin(),
                 buf.begin() + static_cast<std::ptrdiff_t>(got));
    }
    return y;
}

void checkSameSignal(const std::vector<std::complex<float>>& a,
                     const std::vector<std::complex<float>>& b, double tol) {
    CHECK(a.size() == b.size());
    if (a.size() != b.size()) { return; }
    double maxErr = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        maxErr = std::max(maxErr, static_cast<double>(std::abs(a[i] - b[i])));
    }
    CHECK(maxErr <= tol);
    if (maxErr > tol) {
        std::printf("  max sample error %.3g exceeds tol %.3g\n", maxErr, tol);
    }
}

}  // namespace

int main() {
    // --- Getters ------------------------------------------------------------
    {
        Vfo v(kFs, kDecim, 100.0e3);
        CHECK_NEAR(v.channelRateHz(), kChanRate, 1e-9);
        v.setOffsetHz(50.0e3);
        CHECK_NEAR(v.offsetHz(), 50.0e3, 1e-9);
    }

    // --- Offset-to-DC: tone at center+50 kHz, offset 50 kHz -> DC -----------
    // 50 kHz at 2 MS/s is fNorm = 0.025; after mixing by -offset it must land
    // at channel bin 0. All bin indices below are exact (no leakage): the
    // channel bin spacing is 200 kHz / 1024 and every test frequency is an
    // integer multiple of it.
    {
        Vfo v(kFs, kDecim, 100.0e3);
        v.setOffsetHz(50.0e3);
        const std::size_t nIn = (kSettleOut + kK) * kDecim;
        const auto x = tone(nIn, 50.0e3 / kFs);
        std::vector<std::complex<float>> y(nIn / kDecim + 1);
        const std::size_t got = v.process(x.data(), nIn, y.data(), y.size());
        CHECK(got == nIn / kDecim);  // fresh stream: one output per full stride
        const auto mag = dftMag(y.data() + kSettleOut, kK);
        const std::size_t peak = argmax(mag);
        CHECK(peak == 0 || peak == 1 || peak == kK - 1);  // DC within 1 bin
        // In-passband tone at DC passes with the filter's unity DC gain.
        CHECK_NEAR(mag[0] / static_cast<double>(kK), 1.0, 0.02);
    }

    // --- Same tone, offset 0 -> +50 kHz in-channel --------------------------
    {
        Vfo v(kFs, kDecim, 100.0e3);
        v.setOffsetHz(0.0);
        const std::size_t nIn = (kSettleOut + kK) * kDecim;
        const auto x = tone(nIn, 50.0e3 / kFs);
        std::vector<std::complex<float>> y(nIn / kDecim + 1);
        const std::size_t got = v.process(x.data(), nIn, y.data(), y.size());
        CHECK(got == nIn / kDecim);
        const auto mag = dftMag(y.data() + kSettleOut, kK);
        const std::size_t peak = argmax(mag);
        // 50 kHz / (200 kHz / 1024) = bin 256 exactly; allow +/- 1 bin.
        CHECK(peak >= 255 && peak <= 257);
        // 50 kHz is exactly the cutoff (bw/2) — the -6 dB point of a
        // windowed-sinc design, i.e. amplitude 0.5.
        CHECK_NEAR(mag[peak] / static_cast<double>(kK), 0.5, 0.1);
    }

    // --- Out-of-band rejection: tone at +300 kHz, offset 0 ------------------
    // 300 kHz is far outside the 100 kHz bandwidth and beyond the channel
    // Nyquist; whatever aliases into the channel (at -50 kHz after folding by
    // 200 kHz... actually 300-200=100 -> folds onto the Nyquist edge; either
    // way it must be in the filter's deep stopband). Contract: > 50 dB down
    // relative to the unit-amplitude input tone, checked across ALL bins.
    {
        Vfo v(kFs, kDecim, 100.0e3);
        v.setOffsetHz(0.0);
        const std::size_t nIn = (kSettleOut + kK) * kDecim;
        const auto x = tone(nIn, 300.0e3 / kFs);
        std::vector<std::complex<float>> y(nIn / kDecim + 1);
        (void)v.process(x.data(), nIn, y.data(), y.size());
        const auto mag = dftMag(y.data() + kSettleOut, kK);
        const double worst = mag[argmax(mag)] / static_cast<double>(kK);
        // A unit tone in a bin would score 1.0; > 50 dB down means < 10^-2.5.
        CHECK(worst < 3.162e-3);
    }

    // --- Chunked == one-shot streaming equivalence --------------------------
    // Nco advances per sample and FirDecimator is bit-exact under any block
    // split, so the composition must be too. 9001 also exercises the internal
    // 8192-sample chunking against an external split that straddles it.
    {
        const auto x = lcgSignal(15360, 0xC0FFEEu);
        Vfo whole(kFs, kDecim, 100.0e3);
        whole.setOffsetHz(-37.0e3);
        const auto yWhole = runChunked(whole, x, x.size());
        for (const std::size_t bs : {std::size_t{7}, std::size_t{64},
                                     std::size_t{997}, std::size_t{9001}}) {
            Vfo chunked(kFs, kDecim, 100.0e3);
            chunked.setOffsetHz(-37.0e3);
            const auto y = runChunked(chunked, x, bs);
            checkSameSignal(y, yWhole, 1e-7);
        }
    }

    // --- Retune mid-stream vs fresh Vfo on the post-retune input ------------
    // A runs at -30 kHz for 20010 samples (a multiple of the decimation, so
    // its output grid stays aligned with a fresh stream), then retunes to
    // +50 kHz. After the filter settling (169-tap history plus margin: we
    // discard 512 channel samples), A's state depends only on post-retune
    // input, so it must match a fresh Vfo B fed the same samples — up to one
    // CONSTANT phase rotation, because A's NCO is phase-continuous while B
    // starts at phase 0. First-principles bookkeeping of that rotation:
    // A's NCO ran at -offsetPre/fs = +0.015 cycles/sample for 20010 samples,
    // so at the retune instant its phase is frac(0.015 * 20010) = 0.15 cycles
    // ahead of B's — and that exact rotation must be what we estimate, which
    // is also what proves setOffsetHz kept the phase (a phase-resetting
    // implementation would show rotation 1).
    {
        const double offPre = -30.0e3;
        const double offPost = 50.0e3;
        const std::size_t preLen = 20010;
        const std::size_t postLen = 15360;
        const auto pre = lcgSignal(preLen, 0xBEEF01u);
        const auto post = lcgSignal(postLen, 0xD00D02u);

        Vfo a(kFs, kDecim, 100.0e3);
        a.setOffsetHz(offPre);
        std::vector<std::complex<float>> drop(preLen / kDecim + 1);
        (void)a.process(pre.data(), preLen, drop.data(), drop.size());
        a.setOffsetHz(offPost);
        std::vector<std::complex<float>> ya(postLen / kDecim + 1);
        ya.resize(a.process(post.data(), postLen, ya.data(), ya.size()));

        Vfo b(kFs, kDecim, 100.0e3);
        b.setOffsetHz(offPost);
        std::vector<std::complex<float>> yb(postLen / kDecim + 1);
        yb.resize(b.process(post.data(), postLen, yb.data(), yb.size()));

        CHECK(ya.size() == yb.size());
        CHECK(ya.size() == postLen / kDecim);

        // Estimate the rotation from the settled region: sum a*conj(b) has
        // argument equal to the constant phase difference.
        double sre = 0.0, sim = 0.0, power = 0.0;
        for (std::size_t i = kSettleOut; i < ya.size(); ++i) {
            const double ar = ya[i].real(), ai = ya[i].imag();
            const double br = yb[i].real(), bi = yb[i].imag();
            sre += ar * br + ai * bi;
            sim += ai * br - ar * bi;
            power += br * br + bi * bi;
        }
        CHECK(power > 1e-3);  // sanity: we are not comparing silence
        const double smag = std::sqrt(sre * sre + sim * sim);
        CHECK(smag > 1e-6);
        const double rotRe = sre / smag;
        const double rotIm = sim / smag;

        // The rotation must equal the predicted phase-continuity value.
        const double expCyc =
            std::fmod((-offPre / kFs) * static_cast<double>(preLen), 1.0);
        CHECK_NEAR(rotRe, std::cos(kTwoPi * expCyc), 1e-2);
        CHECK_NEAR(rotIm, std::sin(kTwoPi * expCyc), 1e-2);

        // And with it applied, the settled samples must agree.
        double maxErr = 0.0;
        for (std::size_t i = kSettleOut; i < ya.size(); ++i) {
            const double br = static_cast<double>(yb[i].real());
            const double bi = static_cast<double>(yb[i].imag());
            const double er = static_cast<double>(ya[i].real()) -
                              (rotRe * br - rotIm * bi);
            const double ei = static_cast<double>(ya[i].imag()) -
                              (rotIm * br + rotRe * bi);
            maxErr = std::max(maxErr, std::sqrt(er * er + ei * ei));
        }
        CHECK(maxErr <= 1e-3);
        if (maxErr > 1e-3) {
            std::printf("  retune max error %.3g\n", maxErr);
        }
    }

    // --- outCap: surplus outputs drop, but stream state still advances ------
    {
        const auto x = lcgSignal(2000, 0xFACE03u);
        Vfo ref(kFs, kDecim, 100.0e3);
        ref.setOffsetHz(12.0e3);
        const auto yRef = runChunked(ref, x, x.size());
        CHECK(yRef.size() == 200);

        Vfo v(kFs, kDecim, 100.0e3);
        v.setOffsetHz(12.0e3);
        std::vector<std::complex<float>> buf(256);
        // First 1000 inputs would yield 100 outputs; cap at 50 -> the first
        // 50 must come through, the rest of this call's outputs drop.
        std::size_t got = v.process(x.data(), 1000, buf.data(), 50);
        CHECK(got == 50);
        std::vector<std::complex<float>> head(buf.begin(), buf.begin() + 50);
        std::vector<std::complex<float>> refHead(yRef.begin(), yRef.begin() + 50);
        checkSameSignal(head, refHead, 1e-7);
        // Second call: if the first call really consumed all 1000 inputs, its
        // outputs are exactly yRef[100..199].
        got = v.process(x.data() + 1000, 1000, buf.data(), buf.size());
        CHECK(got == 100);
        std::vector<std::complex<float>> tail(buf.begin(), buf.begin() + 100);
        std::vector<std::complex<float>> refTail(yRef.begin() + 100, yRef.end());
        checkSameSignal(tail, refTail, 1e-7);

        // outCap 0: nothing written, state still advances.
        Vfo z(kFs, kDecim, 100.0e3);
        z.setOffsetHz(12.0e3);
        got = z.process(x.data(), 1000, buf.data(), 0);
        CHECK(got == 0);
        got = z.process(x.data() + 1000, 1000, buf.data(), buf.size());
        CHECK(got == 100);
        std::vector<std::complex<float>> tail2(buf.begin(), buf.begin() + 100);
        checkSameSignal(tail2, refTail, 1e-7);
    }

    // --- reset(): fresh-construction behavior, offset retained --------------
    {
        const auto x = lcgSignal(5000, 0xAB1E04u);
        Vfo v(kFs, kDecim, 100.0e3);
        v.setOffsetHz(50.0e3);
        const auto y1 = runChunked(v, x, x.size());
        (void)runChunked(v, x, 313);  // dirty the state (NCO phase, history)
        v.reset();
        CHECK_NEAR(v.offsetHz(), 50.0e3, 1e-9);
        const auto y2 = runChunked(v, x, x.size());
        checkSameSignal(y2, y1, 1e-7);
    }

    // --- setBandwidthHz: redesign + history clear ---------------------------
    // Offset never set (0), so the NCO holds phase 0 throughout and the
    // post-change Vfo must match a FRESH Vfo built at the new bandwidth
    // exactly — that single comparison proves the redesign, the history
    // clear, and the grid restart at once. A third Vfo at the OLD bandwidth
    // must disagree, proving the filter actually changed.
    {
        const auto preNoise = lcgSignal(5000, 0x5EED05u);
        const auto x2 = lcgSignal(15360, 0x5EED06u);

        Vfo a(kFs, kDecim, 150.0e3);
        (void)runChunked(a, preNoise, preNoise.size());
        a.setBandwidthHz(60.0e3);
        const auto ya = runChunked(a, x2, x2.size());

        Vfo b(kFs, kDecim, 60.0e3);
        const auto yb = runChunked(b, x2, x2.size());
        checkSameSignal(ya, yb, 1e-7);

        Vfo c(kFs, kDecim, 150.0e3);
        const auto yc = runChunked(c, x2, x2.size());
        double maxDiff = 0.0;
        CHECK(ya.size() == yc.size());
        for (std::size_t i = 0; i < std::min(ya.size(), yc.size()); ++i) {
            maxDiff = std::max(maxDiff,
                               static_cast<double>(std::abs(ya[i] - yc[i])));
        }
        CHECK(maxDiff > 1e-3);  // different bandwidth => different output
    }

    // --- Bandwidth clamp: absurd request behaves like 0.9 * channelRate -----
    // bw 5 MHz on a 200 kHz channel must clamp to 180 kHz; a tone at 150 kHz
    // (normalized 0.075, beyond the 100 kHz channel-Nyquist stopband edge at
    // 0.05) must therefore still be deeply attenuated instead of aliasing in.
    {
        Vfo v(kFs, kDecim, 5.0e6);
        v.setOffsetHz(0.0);
        const std::size_t nIn = (kSettleOut + kK) * kDecim;
        const auto x = tone(nIn, 150.0e3 / kFs);
        std::vector<std::complex<float>> y(nIn / kDecim + 1);
        (void)v.process(x.data(), nIn, y.data(), y.size());
        const auto mag = dftMag(y.data() + kSettleOut, kK);
        const double worst = mag[argmax(mag)] / static_cast<double>(kK);
        CHECK(worst < 3.162e-3);
    }

    return testSummary("test_vfo");
}
