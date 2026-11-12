// Tests for the two noise-reduction methods (dsp/nr_methods.hpp).
//
// Figures are measured against a KNOWN clean reference: SNR(out) = sum s^2 / sum (out - s)^2 with the clean signal s delayed by the
// method's latency, so signal distortion counts against the method exactly like residual noise does. The tone and the speech-like
// signal are generated here (sbgen::speechLike is NOT speech; the record says so).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "dsp/nr_methods.hpp"
#include "dsp/noise_reduction.hpp"

#include "alloc_probe.hpp"
#include "signal_block_gen.hpp"
#include "test_check.hpp"

#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

namespace {

using cascade::dsp::LineEnhancer;
using cascade::dsp::MinStatSubtractor;
using cascade::dsp::NoiseReducer;

constexpr double kFs = 48000.0;

double power(const std::vector<float>& v, std::size_t from, std::size_t to) {
    double p = 0;
    for (std::size_t i = from; i < to; ++i) { p += static_cast<double>(v[i]) * v[i]; }
    return p / static_cast<double>(to - from);
}

// SNR of `out` against `clean` over clean[from, to), with out delayed by `lag`.
double snrDb(const std::vector<float>& clean, const std::vector<float>& out, std::size_t lag, std::size_t from, std::size_t to) {
    double num = 0, den = 0;
    for (std::size_t i = from; i < to; ++i) {
        const double d = static_cast<double>(out[i + lag]) - clean[i];
        num += static_cast<double>(clean[i]) * clean[i];
        den += d * d;
    }
    return 10.0 * std::log10(num / std::max(den, 1e-30));
}

std::vector<float> run(NoiseReducer& r, const std::vector<float>& x, std::size_t chunk = 4800) {
    std::vector<float> y(x.size());
    for (std::size_t i = 0; i < x.size(); i += chunk) { r.process(x.data() + i, std::min(chunk, x.size() - i), y.data() + i); }
    return y;
}

std::vector<float> mix(const std::vector<float>& s, const std::vector<float>& n) {
    std::vector<float> x(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) { x[i] = s[i] + n[i]; }
    return x;
}

// Scales white noise so the input SNR (full band) is `snrIn` dB against `clean` (measured over the whole of it).
std::vector<float> noiseFor(const std::vector<float>& clean, double snrIn, std::uint32_t seed) {
    auto n = sbgen::whiteNoise(clean.size(), 1.0, seed);
    const double sp = power(clean, 0, clean.size());
    const double np = power(n, 0, n.size());
    const float g = static_cast<float>(std::sqrt(sp / (np * std::pow(10.0, snrIn / 10.0))));
    for (auto& v : n) { v *= g; }
    return n;
}

struct Method {
    const char* name;
    std::unique_ptr<NoiseReducer> (*make)();
};
std::unique_ptr<NoiseReducer> makeSub() { return std::make_unique<MinStatSubtractor>(); }
std::unique_ptr<NoiseReducer> makeAle() { return std::make_unique<LineEnhancer>(); }
const Method kMethods[] = {{"MinStatSubtractor", makeSub}, {"LineEnhancer", makeAle}};

std::vector<float> tone(std::size_t n, double hz, double amp) {
    std::vector<float> v(n, 0.0f);
    sbgen::addTone(v, kFs, hz, amp, 0, n);
    return v;
}

// ---- SNR improvement ------------------------------------------------------------------------------------------------------

struct Gains {
    double toneImp[2] = {0, 0};
    double speechImp[2] = {0, 0};
};
Gains g_gain;

void testSnrImprovement() {
    const std::size_t n = static_cast<std::size_t>(20 * kFs);
    const auto cleanTone = tone(n, 800.0, 0.1);
    const auto cleanSpeech = sbgen::speechLike(n, kFs, 0.5, 21);
    for (int m = 0; m < 2; ++m) {
        for (int kind = 0; kind < 2; ++kind) {
            const auto& clean = kind == 0 ? cleanTone : cleanSpeech;
            const double snrIn = kind == 0 ? 0.0 : 5.0;
            const auto noisy = mix(clean, noiseFor(clean, snrIn, 77 + static_cast<std::uint32_t>(kind)));
            auto r = kMethods[m].make();
            r->setStrength(1.0f);
            const auto y = run(*r, noisy);
            const std::size_t from = static_cast<std::size_t>(10 * kFs), to = n - 2000;
            const double in = snrDb(clean, noisy, 0, from, to - r->latencySamples());
            const double out = snrDb(clean, y, r->latencySamples(), from, to - r->latencySamples());
            std::printf("  %-18s %-12s input SNR %+5.1f dB -> output %+6.1f dB: improvement %+5.1f dB\n", kMethods[m].name,
                        kind == 0 ? "800 Hz tone" : "speech-like", in, out, out - in);
            (kind == 0 ? g_gain.toneImp : g_gain.speechImp)[m] = out - in;
        }
    }
    // pinned from the measured numbers (below) with a margin of about 1.5 dB
    CHECK(g_gain.toneImp[0] > 12.0);      // subtractor on a tone in white noise
    CHECK(g_gain.speechImp[0] > 4.0);     // subtractor on speech-like in white noise
    CHECK(g_gain.toneImp[1] > 14.5);     // line enhancer on a tone in white noise
    CHECK(g_gain.speechImp[1] > -5.0);    // measured -3.7: the enhancer is not the tool for speech (it keeps only the predictable part); pinned so it does not get worse
}

// ---- strength -------------------------------------------------------------------------------------------------------------

void testStrength() {
    const std::size_t n = static_cast<std::size_t>(12 * kFs);
    const auto clean = tone(n, 800.0, 0.1);
    const auto noisy = mix(clean, noiseFor(clean, 0.0, 5));
    const auto x = sbgen::speechLike(n, kFs, 0.5, 3);
    for (int m = 0; m < 2; ++m) {
        double prev = -100;
        for (float s : {0.0f, 0.25f, 0.5f, 1.0f}) {
            auto r = kMethods[m].make();
            r->setStrength(s);
            const auto y = run(*r, noisy);
            const double out = snrDb(clean, y, r->latencySamples(), static_cast<std::size_t>(6 * kFs), n - 2000);
            std::printf("  %-18s strength %.2f: tone-in-noise output SNR %+6.2f dB\n", kMethods[m].name, s, out);
            CHECK(out > prev - 0.05);       // more strength never gives a worse SNR on this signal
            prev = out;
            if (s == 0.0f) {
                // bit-exact bypass, delayed by the latency
                auto r2 = kMethods[m].make();
                r2->setStrength(0.0f);
                const auto z = run(*r2, x);
                bool exact = true;
                const std::size_t lag = r2->latencySamples();
                for (std::size_t i = lag; i < x.size(); ++i) { exact = exact && (z[i] == x[i - lag]); }
                CHECK(exact);
            }
        }
        // out-of-range and non-finite settings are clamped
        auto r = kMethods[m].make();
        r->setStrength(7.0f);
        CHECK_NEAR(r->strength(), 1.0, 0);
        r->setStrength(-3.0f);
        CHECK_NEAR(r->strength(), 0.0, 0);
        r->setStrength(std::nanf(""));
        CHECK_NEAR(r->strength(), 0.0, 0);
    }
}

// ---- distortion on a clean signal ---------------------------------------------------------------------------------------------

void testCleanSignalDistortion() {
    const std::size_t n = static_cast<std::size_t>(12 * kFs);
    const auto cleanTone = tone(n, 800.0, 0.1);
    const auto cleanSpeech = sbgen::speechLike(n, kFs, 0.5, 9);
    for (int m = 0; m < 2; ++m) {
        for (int kind = 0; kind < 2; ++kind) {
            const auto& clean = kind == 0 ? cleanTone : cleanSpeech;
            auto r = kMethods[m].make();
            r->setStrength(1.0f);
            const auto y = run(*r, clean);
            const double snr = snrDb(clean, y, r->latencySamples(), static_cast<std::size_t>(6 * kFs), n - 2000);
            std::printf("  %-18s clean %-12s at strength 1: output vs input SNR %+6.1f dB (distortion %.2f%%)\n", kMethods[m].name,
                        kind == 0 ? "800 Hz tone" : "speech-like", snr, 100.0 * std::pow(10.0, -snr / 20.0));
            if (m == 0 && kind == 0) { CHECK(snr > 60.0); }   // a steady tone is left alone (tilt cap): measured 112 dB
            if (m == 0 && kind == 1) { CHECK(snr > 35.0); }   // measured 43.7 dB (0.65 % distortion)
            if (m == 1 && kind == 0) { CHECK(snr > 100.0); }
            if (m == 1 && kind == 1) { CHECK(snr > 0.0); }       // measured +1.1 dB: it rewrites speech heavily at strength 1
        }
    }
}

// ---- noise estimate bias and the floor ------------------------------------------------------------------------------------------

// Coefficient of variation of the short-time bin powers (512-sample Hann periodograms, hop 512, bins 4..249) of y[from, to).
double binPowerCv(const std::vector<float>& y, std::size_t from) {
    cascade::dsp::ComplexFFT fft(512);
    std::vector<std::complex<float>> a(512), b(512);
    double s1 = 0, s2 = 0;
    long cnt = 0;
    for (std::size_t st = from; st + 512 <= y.size(); st += 512) {
        for (std::size_t i = 0; i < 512; ++i) {
            const double w = 0.5 - 0.5 * std::cos(6.283185307179586 * static_cast<double>(i) / 512.0);
            a[i] = std::complex<float>(static_cast<float>(y[st + i] * w), 0.0f);
        }
        fft.forward(a.data(), b.data());
        for (std::size_t k = 4; k < 250; ++k) {
            const double p = std::norm(b[k]);
            s1 += p;
            s2 += p * p;
            ++cnt;
        }
    }
    const double mean = s1 / static_cast<double>(cnt);
    return std::sqrt(s2 / static_cast<double>(cnt) - mean * mean) / mean;
}

void testNoiseEstimateAndFloor() {
    // white Gaussian noise of sigma 0.1: E|X_k|^2 = sigma^2 * sum(window^2) = sigma^2 * N / 2 (sqrt-Hann squared is Hann)
    const std::size_t n = static_cast<std::size_t>(30 * kFs);
    const auto nz = sbgen::whiteNoise(n, 0.1, 1234);
    MinStatSubtractor r;
    r.setStrength(1.0f);
    std::vector<float> y(n);
    double ratioSum = 0;
    int count = 0;
    for (std::size_t i = 0; i < n; i += 4800) {
        r.process(nz.data() + i, 4800, y.data() + i);
        if (i > static_cast<std::size_t>(5 * kFs)) {
            const double expect = 0.1 * 0.1 * 512.0 / 2.0;
            double s = 0;
            for (std::size_t k = 2; k < 250; ++k) { s += r.noisePower(k); }
            ratioSum += s / 248.0 / expect;
            ++count;
        }
    }
    const double ratio = ratioSum / count;
    std::printf("  noise estimate / true noise power on white Gaussian noise: %.3f (%.2f dB)\n", ratio, 10 * std::log10(ratio));
    CHECK(std::fabs(10 * std::log10(ratio)) < 1.0);       // within 1 dB of the truth after the bias factor
    // noise-only output: reduced but never muted (the floor), and not 'musical': the coefficient of variation of the output's
    // short-time bin powers stays near 1, the value for Gaussian noise (a shower of tonal blips pushes it well above)
    const double pin = power(nz, static_cast<std::size_t>(10 * kFs), n);
    const double pout = power(y, static_cast<std::size_t>(10 * kFs), n);
    const double redDb = 10 * std::log10(pin / pout);
    std::printf("  noise-only at strength 1: level reduced by %.1f dB (floor -20 dB)\n", redDb);
    CHECK(redDb > 6.0 && redDb < 21.0);
    const double cv = binPowerCv(y, static_cast<std::size_t>(10 * kFs));
    cascade::dsp::NoiseReduction existing(kFs);
    existing.setStrength(1.0f);
    std::vector<float> ye(n);
    existing.process(nz.data(), n, ye.data());
    const double cvExisting = binPowerCv(ye, static_cast<std::size_t>(10 * kFs));
    std::printf("  output bin-power coefficient of variation: %.2f (Gaussian noise 1.00; the existing NoiseReduction on the same noise: %.2f)\n", cv, cvExisting);
    CHECK(cv < 1.75 && cv < cvExisting + 0.1);   // measured 1.62 vs the existing 1.67
}

// ---- latency, chunking, robustness, allocation --------------------------------------------------------------------------------------

void testLatencyAndRobustness() {
    const std::size_t n = static_cast<std::size_t>(8 * kFs);
    // latency measured: a burst of a 1 kHz tone starting at a known sample; the output envelope starts that many samples later
    for (int m = 0; m < 2; ++m) {
        std::vector<float> x(n, 0.0f);
        const std::size_t on = static_cast<std::size_t>(5 * kFs);
        sbgen::addTone(x, kFs, 1000.0, 0.3, on, on + 4800);
        auto nz = sbgen::whiteNoise(n, 0.002, 4);
        for (std::size_t i = 0; i < n; ++i) { x[i] += nz[i]; }
        auto r = kMethods[m].make();
        r->setStrength(0.5f);
        const auto y = run(*r, x);
        // find by cross-correlation the lag (0..1200) at which output best matches the input burst
        double best = -1;
        std::size_t bestLag = 0;
        for (std::size_t lag = 0; lag <= 1200; ++lag) {
            double c = 0;
            for (std::size_t i = on; i < on + 4800; ++i) { c += static_cast<double>(x[i]) * y[i + lag]; }
            if (c > best) {
                best = c;
                bestLag = lag;
            }
        }
        std::printf("  %-18s measured latency %zu samples (%.2f ms), stated %zu\n", kMethods[m].name, bestLag, 1000.0 * static_cast<double>(bestLag) / kFs,
                    r->latencySamples());
        CHECK(bestLag == r->latencySamples());
    }
    // chunking: identical output for any split
    const auto x = mix(tone(40000, 700.0, 0.1), sbgen::whiteNoise(40000, 0.05, 8));
    for (int m = 0; m < 2; ++m) {
        auto a = kMethods[m].make();
        auto b = kMethods[m].make();
        a->setStrength(0.8f);
        b->setStrength(0.8f);
        std::vector<float> ya(x.size()), yb(x.size());
        a->process(x.data(), x.size(), ya.data());
        for (std::size_t i = 0; i < x.size();) {
            const std::size_t k = std::min<std::size_t>(1 + (i * 7919) % 991, x.size() - i);
            b->process(x.data() + i, k, yb.data() + i);
            i += k;
        }
        CHECK(ya == yb);
        // non-finite input
        auto c = kMethods[m].make();
        c->setStrength(1.0f);
        std::vector<float> bad = x;
        bad[3000] = std::nanf("");
        bad[3001] = INFINITY;
        std::vector<float> yc(bad.size());
        c->process(bad.data(), bad.size(), yc.data());
        bool fin = true;
        for (float v : yc) { fin = fin && std::isfinite(v); }
        CHECK(fin);
        // in place
        auto d = kMethods[m].make();
        d->setStrength(0.8f);
        std::vector<float> inplace = x;
        d->process(inplace.data(), inplace.size(), inplace.data());
        CHECK(inplace == ya);
        // reset: a second pass after reset equals the first
        a->reset();
        std::vector<float> yr(x.size());
        a->process(x.data(), x.size(), yr.data());
        CHECK(yr == ya);
        // no allocation in process after set-up
        {
            std::vector<float> yo(4800);
            AllocProbe probe;
            for (int k = 0; k < 20; ++k) {
                a->setStrength(k % 3 == 0 ? 0.0f : 0.9f);
                a->process(x.data(), 4800, yo.data());
            }
            CHECK(probe.news() == 0);
        }
    }
}

}  // namespace

int main() {
    std::printf("SNR improvement:\n");
    testSnrImprovement();
    std::printf("strength:\n");
    testStrength();
    std::printf("clean signal:\n");
    testCleanSignalDistortion();
    std::printf("noise estimate and floor:\n");
    testNoiseEstimateAndFloor();
    std::printf("latency and robustness:\n");
    testLatencyAndRobustness();
    return testSummary("test_nr_methods");
}
