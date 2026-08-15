// Tests for dsp/noise_reduction.hpp.
//
// The load-bearing test here is the COLA reconstruction check: it runs the
// FULL overlap-add path (strength just above the bypass threshold, so every
// per-bin gain is within 1e-6 of unity) and demands the output equal the input
// delayed by latencySamples(). Any error in the window pair, the hop, the
// accumulator shift, or the transform normalisation shows up there. The
// bit-exact passthrough checks cannot catch those on their own, because
// bypass is deliberately a separate delay line (see noise_reduction.hpp).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "dsp/noise_reduction.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <vector>

#include "test_check.hpp"

using cascade::dsp::NoiseReduction;

namespace {

constexpr double kTwoPi = 6.283185307179586476925286766559;
constexpr double kFs = 48000.0;
// Bin 11 of a 512-point transform at 48 kHz: exactly on a bin, so the tone's
// energy does not smear across neighbours in either the module's analysis or
// the measurement below.
constexpr double kToneHz = 1031.25;
constexpr std::size_t kToneBin = 11;

struct Lcg {
    std::uint32_t s;
    explicit Lcg(std::uint32_t seed) : s(seed) {}
    float next() {  // uniform in [-1, 1)
        s = s * 1664525u + 1013904223u;
        return static_cast<float>(s) * (2.0f / 4294967296.0f) - 1.0f;
    }
};

std::vector<float> makeNoisyTone(std::size_t n, double toneAmp, double noiseAmp,
                                 std::uint32_t seed) {
    Lcg rng(seed);
    std::vector<float> x(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / kFs;
        x[i] = static_cast<float>(toneAmp * std::cos(kTwoPi * kToneHz * t) +
                                  noiseAmp * static_cast<double>(rng.next()));
    }
    return x;
}

// Index of the first sample where out[j] does not bit-match the expected
// delayed input, or SIZE_MAX. Reported as one CHECK instead of 48000 of them.
std::size_t firstBitMismatch(const std::vector<float>& in, const std::vector<float>& out,
                             std::size_t latency) {
    for (std::size_t j = 0; j < out.size(); ++j) {
        const float want = (j < latency) ? 0.0f : in[j - latency];
        if (out[j] != want) { return j; }
    }
    return std::numeric_limits<std::size_t>::max();
}

// Hann-windowed power spectrum by direct DFT — an in-test reference, not the
// module's own transform.
std::vector<double> powerSpectrum(const float* x, std::size_t m) {
    std::vector<double> w(m);
    for (std::size_t i = 0; i < m; ++i) {
        w[i] = 0.5 - 0.5 * std::cos(kTwoPi * static_cast<double>(i) /
                                    static_cast<double>(m));
    }
    std::vector<double> out(m / 2 + 1, 0.0);
    for (std::size_t k = 0; k < out.size(); ++k) {
        double re = 0.0;
        double im = 0.0;
        for (std::size_t i = 0; i < m; ++i) {
            const double ang =
                kTwoPi * static_cast<double>(k) * static_cast<double>(i) /
                static_cast<double>(m);
            const double v = w[i] * static_cast<double>(x[i]);
            re += v * std::cos(ang);
            im -= v * std::sin(ang);
        }
        out[k] = re * re + im * im;
    }
    return out;
}

// Tone energy (the tone's three bins) and mean per-bin noise energy (the rest
// of the band), averaged over consecutive 512-sample windows.
struct ToneNoise {
    double tone;
    double noisePerBin;
    double tnrDb() const {
        return 10.0 * std::log10(tone / (noisePerBin > 1e-300 ? noisePerBin : 1e-300));
    }
};

ToneNoise measure(const float* x, std::size_t n) {
    constexpr std::size_t kM = 512;
    std::vector<double> avg(kM / 2 + 1, 0.0);
    const std::size_t windows = n / kM;
    for (std::size_t w = 0; w < windows; ++w) {
        const auto p = powerSpectrum(x + w * kM, kM);
        for (std::size_t k = 0; k < avg.size(); ++k) { avg[k] += p[k]; }
    }
    ToneNoise r{0.0, 0.0};
    for (std::size_t k = kToneBin - 1; k <= kToneBin + 1; ++k) { r.tone += avg[k]; }
    std::size_t count = 0;
    for (std::size_t k = 4; k <= 250; ++k) {
        if (k >= kToneBin - 3 && k <= kToneBin + 3) { continue; }
        r.noisePerBin += avg[k];
        ++count;
    }
    r.noisePerBin /= static_cast<double>(count);
    return r;
}

}  // namespace

int main() {
    // ---------------------------------------------------------------------
    // Contract basics.
    // ---------------------------------------------------------------------
    {
        NoiseReduction nr(kFs, 512);
        CHECK(nr.fftSize() == 512);
        CHECK(nr.hopSize() == 256);
        CHECK(nr.latencySamples() == 512);
        bool threw = false;
        try {
            NoiseReduction bad(kFs, 100);  // not 2^a*3^b*5^c with a >= 5
            (void)bad;
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        CHECK(threw);
        nr.setStrength(-1.0f);
        CHECK(nr.strength() == 0.0f);
        nr.setStrength(5.0f);
        CHECK(nr.strength() == 1.0f);
    }

    const auto in = makeNoisyTone(48000, 0.4, 0.3, 0xC0FFEEu);

    // ---------------------------------------------------------------------
    // Disabled: bit-exact passthrough after the documented latency.
    // ---------------------------------------------------------------------
    {
        NoiseReduction nr(kFs, 512);
        nr.setStrength(1.0f);
        nr.setEnabled(false);
        std::vector<float> out(in.size());
        const std::size_t wrote = nr.process(in.data(), in.size(), out.data());
        CHECK(wrote == in.size());
        const std::size_t bad = firstBitMismatch(in, out, nr.latencySamples());
        if (bad != std::numeric_limits<std::size_t>::max()) {
            std::printf("disabled: first mismatch at %zu (%.9g vs %.9g)\n", bad,
                        static_cast<double>(out[bad]),
                        static_cast<double>(in[bad - nr.latencySamples()]));
        }
        CHECK(bad == std::numeric_limits<std::size_t>::max());
    }

    // ---------------------------------------------------------------------
    // Strength 0 while enabled: also bit-exact (the STFT keeps running to hold
    // the noise estimate warm, but the output comes from the delay line).
    // ---------------------------------------------------------------------
    {
        NoiseReduction nr(kFs, 512);
        nr.setStrength(0.0f);
        std::vector<float> out(in.size());
        nr.process(in.data(), in.size(), out.data());
        const std::size_t bad = firstBitMismatch(in, out, nr.latencySamples());
        CHECK(bad == std::numeric_limits<std::size_t>::max());
    }

    // Aliased in == out must behave identically.
    {
        NoiseReduction nr(kFs, 512);
        nr.setStrength(0.0f);
        std::vector<float> buf = in;
        nr.process(buf.data(), buf.size(), buf.data());
        const std::size_t bad = firstBitMismatch(in, buf, nr.latencySamples());
        CHECK(bad == std::numeric_limits<std::size_t>::max());
    }

    // ---------------------------------------------------------------------
    // COLA reconstruction through the real overlap-add path.
    // ---------------------------------------------------------------------
    {
        NoiseReduction nr(kFs, 512);
        nr.setStrength(1e-6f);  // above bypass, gains within 1e-6 of unity
        std::vector<float> out(in.size());
        nr.process(in.data(), in.size(), out.data());
        const std::size_t L = nr.latencySamples();
        double maxErr = 0.0;
        for (std::size_t j = L; j < out.size(); ++j) {
            const double e = std::fabs(static_cast<double>(out[j]) -
                                       static_cast<double>(in[j - L]));
            if (e > maxErr) { maxErr = e; }
        }
        std::printf("COLA overlap-add: max |out[j] - in[j-%zu]| = %.3g\n", L, maxErr);
        CHECK(maxErr < 1e-5);
    }

    // ---------------------------------------------------------------------
    // Suppression: tone-to-noise ratio at strength 1 versus the input.
    // ---------------------------------------------------------------------
    {
        NoiseReduction nr(kFs, 512);
        nr.setStrength(1.0f);
        std::vector<float> out(in.size());
        nr.process(in.data(), in.size(), out.data());
        const std::size_t L = nr.latencySamples();
        const std::size_t warm = 16384;  // let the floor estimate settle
        const std::size_t meas = 8192;
        const ToneNoise before = measure(in.data() + warm, meas);
        const ToneNoise after = measure(out.data() + warm + L, meas);
        std::printf("noise reduction: TNR %.2f dB -> %.2f dB (%+.2f dB); "
                    "tone %+.2f dB, noise floor %+.2f dB\n",
                    before.tnrDb(), after.tnrDb(), after.tnrDb() - before.tnrDb(),
                    10.0 * std::log10(after.tone / before.tone),
                    10.0 * std::log10(after.noisePerBin / before.noisePerBin));
        CHECK(after.tnrDb() - before.tnrDb() > 8.0);
        // The wanted tone must survive: this is what stops a "better TNR"
        // being achieved by simply turning everything down.
        CHECK(10.0 * std::log10(after.tone / before.tone) > -3.0);
    }

    // ---------------------------------------------------------------------
    // Silence stays finite: an all-zero noise estimate must not divide by
    // zero, and a hard cut to silence after loud input must not either.
    // ---------------------------------------------------------------------
    {
        NoiseReduction nr(kFs, 512);
        nr.setStrength(1.0f);
        std::vector<float> zeros(8192, 0.0f);
        std::vector<float> out(zeros.size());
        nr.process(zeros.data(), zeros.size(), out.data());
        bool ok = true;
        for (float v : out) {
            if (!std::isfinite(v) || v != 0.0f) { ok = false; }
        }
        CHECK(ok);

        NoiseReduction nr2(kFs, 512);
        nr2.setStrength(1.0f);
        std::vector<float> loud(8192);
        for (std::size_t i = 0; i < loud.size(); ++i) { loud[i] = in[i]; }
        std::vector<float> o1(loud.size());
        nr2.process(loud.data(), loud.size(), o1.data());
        std::vector<float> o2(zeros.size());
        nr2.process(zeros.data(), zeros.size(), o2.data());
        bool ok2 = true;
        for (float v : o1) {
            if (!std::isfinite(v)) { ok2 = false; }
        }
        for (float v : o2) {
            if (!std::isfinite(v)) { ok2 = false; }
        }
        CHECK(ok2);
        // Past the latency the tail must actually be silent, not a residue.
        double tail = 0.0;
        for (std::size_t j = 1024; j < o2.size(); ++j) {
            tail = std::max(tail, std::fabs(static_cast<double>(o2[j])));
        }
        CHECK(tail < 1e-6);
    }

    // ---------------------------------------------------------------------
    // Chunked == one-shot over odd block sizes (bit-exact: the per-sample
    // path does not depend on where a call boundary falls).
    // ---------------------------------------------------------------------
    {
        NoiseReduction a(kFs, 512);
        a.setStrength(0.7f);
        std::vector<float> oneShot(in.size());
        a.process(in.data(), in.size(), oneShot.data());

        NoiseReduction b(kFs, 512);
        b.setStrength(0.7f);
        std::vector<float> chunked(in.size());
        const std::size_t sizes[] = {1, 3, 7, 13, 31, 193, 511};
        std::size_t i = 0;
        std::size_t si = 0;
        while (i < in.size()) {
            std::size_t take = sizes[si % 7];
            ++si;
            if (i + take > in.size()) { take = in.size() - i; }
            b.process(in.data() + i, take, chunked.data() + i);
            i += take;
        }
        std::size_t bad = 0;
        for (std::size_t k = 0; k < in.size(); ++k) {
            if (chunked[k] != oneShot[k]) { ++bad; }
        }
        CHECK(bad == 0);
    }

    // ---------------------------------------------------------------------
    // reset() == a freshly constructed reducer.
    // ---------------------------------------------------------------------
    {
        NoiseReduction fresh(kFs, 512);
        fresh.setStrength(0.7f);
        std::vector<float> want(in.size());
        fresh.process(in.data(), in.size(), want.data());

        NoiseReduction used(kFs, 512);
        used.setStrength(0.7f);
        std::vector<float> scratch(in.size());
        used.process(in.data(), in.size(), scratch.data());
        used.reset();
        std::vector<float> got(in.size());
        used.process(in.data(), in.size(), got.data());

        std::size_t bad = 0;
        for (std::size_t k = 0; k < in.size(); ++k) {
            if (got[k] != want[k]) { ++bad; }
        }
        CHECK(bad == 0);
    }

    return testSummary("test_noise_reduction");
}
