// Tests for dsp/resampler.hpp: streaming polyphase rational resampler.
// SPDX-License-Identifier: MIT
#include "dsp/resampler.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <vector>

#include "test_check.hpp"

using cascade::dsp::RationalResampler;

namespace {

constexpr double kTwoPi = 6.283185307179586476925286766559;

// Direct evaluation of |X(f)| = |sum_k x[start+k] exp(-j 2 pi f k)| over an
// n-sample window — the DTFT definition, independent of the implementation
// under test.
double dtftMag(const std::vector<float>& x, std::size_t start, std::size_t n,
               double f) {
    double re = 0.0;
    double im = 0.0;
    for (std::size_t k = 0; k < n; ++k) {
        const double phi = kTwoPi * f * static_cast<double>(k);
        re += static_cast<double>(x[start + k]) * std::cos(phi);
        im -= static_cast<double>(x[start + k]) * std::sin(phi);
    }
    return std::sqrt(re * re + im * im);
}

// Fixed-seed LCG (Numerical Recipes constants) — deterministic test input.
struct Lcg {
    std::uint32_t state;
    explicit Lcg(std::uint32_t seed) : state(seed) {}
    std::uint32_t nextU32() {
        state = state * 1664525u + 1013904223u;
        return state;
    }
    float next() {
        // Top 24 bits -> [-1, 1).
        return (static_cast<float>(nextU32() >> 8) / 8388608.0f) - 1.0f;
    }
};

std::vector<float> lcgSignal(std::size_t n, std::uint32_t seed) {
    Lcg rng(seed);
    std::vector<float> x(n);
    for (std::size_t i = 0; i < n; ++i) { x[i] = rng.next(); }
    return x;
}

std::vector<float> toneSignal(std::size_t n, double fNorm) {
    std::vector<float> x(n);
    for (std::size_t i = 0; i < n; ++i) {
        x[i] = static_cast<float>(
            std::cos(kTwoPi * fNorm * static_cast<double>(i)));
    }
    return x;
}

std::vector<float> runOneShot(RationalResampler& rs,
                              const std::vector<float>& x) {
    std::vector<float> y(rs.maxOut(x.size()));
    y.resize(rs.process(x.data(), x.size(), y.data(), y.size()));
    return y;
}

// Runs the resampler over `x` in blocks of `blockSize` and concatenates all
// outputs, checking the maxOut bound on every call.
std::vector<float> runChunked(RationalResampler& rs,
                              const std::vector<float>& x,
                              std::size_t blockSize) {
    std::vector<float> y;
    std::vector<float> buf;
    for (std::size_t pos = 0; pos < x.size(); pos += blockSize) {
        const std::size_t n = std::min(blockSize, x.size() - pos);
        buf.resize(rs.maxOut(n));
        const std::size_t got =
            rs.process(x.data() + pos, n, buf.data(), buf.size());
        CHECK(got <= rs.maxOut(n));
        y.insert(y.end(), buf.begin(),
                 buf.begin() + static_cast<std::ptrdiff_t>(got));
    }
    return y;
}

void checkSameSignal(const std::vector<float>& a, const std::vector<float>& b,
                     double tol) {
    CHECK(a.size() == b.size());
    if (a.size() != b.size()) { return; }
    double maxErr = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        maxErr = std::max(
            maxErr, std::fabs(static_cast<double>(a[i]) -
                              static_cast<double>(b[i])));
    }
    CHECK(maxErr <= tol);
    if (maxErr > tol) {
        std::printf("  max sample error %.3g exceeds tol %.3g\n", maxErr, tol);
    }
}

// Scans DFT bins k = 1..n/2 of an n-sample window of y and returns the bin
// with the largest magnitude (real signal: one side of the spectrum carries
// everything).
std::size_t peakBin(const std::vector<float>& y, std::size_t start,
                    std::size_t n) {
    std::size_t best = 1;
    double bestMag = -1.0;
    for (std::size_t k = 1; k <= n / 2; ++k) {
        const double mag =
            dtftMag(y, start, n, static_cast<double>(k) / static_cast<double>(n));
        if (mag > bestMag) {
            bestMag = mag;
            best = k;
        }
    }
    return best;
}

// Largest bin magnitude outside a +-guard window around bin kTone: the
// alias/image floor of the resampled tone.
double worstOffPeak(const std::vector<float>& y, std::size_t start,
                    std::size_t n, std::size_t kTone, std::size_t guard) {
    double worst = 0.0;
    for (std::size_t k = 1; k <= n / 2; ++k) {
        const std::size_t d = (k > kTone) ? k - kTone : kTone - k;
        if (d <= guard) { continue; }
        worst = std::max(worst, dtftMag(y, start, n,
                                        static_cast<double>(k) /
                                            static_cast<double>(n)));
    }
    return worst;
}

}  // namespace

int main() {
    // --- Tone through 6/25 (200 kHz -> 48 kHz) keeps its absolute frequency -
    // From first principles: a 10 kHz tone at 200 kHz sample rate is
    // fIn = 10k/200k = 0.05 cycles/sample. After 6/25 resampling the rate is
    // 200k*6/25 = 48 kHz and the SAME 10 kHz tone must sit at
    // fOut = 10k/48k = 5/24 cycles/sample (= fIn*25/6). A 480-sample DFT puts
    // that exactly on bin fOut*480 = 100, so leakage cannot blur the peak.
    {
        const double inRate = 200000.0;
        const double outRate = inRate * 6.0 / 25.0;
        CHECK_NEAR(outRate, 48000.0, 1e-9);
        const double toneHz = 10000.0;
        const double fIn = toneHz / inRate;
        const double fOut = toneHz / outRate;
        const std::size_t n = 480;
        CHECK_NEAR(fOut * static_cast<double>(n), 100.0, 1e-9);

        RationalResampler rs(6, 25);
        const auto x = toneSignal(8000, fIn);
        const auto y = runOneShot(rs, x);
        // ceil(8000*6/25) = 1920 outputs from a fresh stream.
        CHECK(y.size() == 1920);

        const std::size_t start = 200;  // comfortably past the startup ramp
        const std::size_t kTone = 100;
        CHECK(peakBin(y, start, n) == kTone);

        // Amplitude: with the default 16 taps/phase the Blackman-Harris
        // transition band is wide relative to the narrow 0.5/25 cutoff, so a
        // tone at 10/24 of the output Nyquist already sees some rolloff.
        // Measured 0.9088 gain, alias floor 8.6e-7 of the peak.
        const double peak = dtftMag(y, start, n, fOut);
        const double amp = peak / (static_cast<double>(n) / 2.0);
        const double floor = worstOffPeak(y, start, n, kTone, 3);
        CHECK_NEAR(amp, 0.909, 0.05);
        CHECK(floor < 1e-3 * peak);

        // Sharper filter: same peak bin, amplitude near unity.
        // Measured 0.999995 gain, alias floor 2.1e-7 of the peak.
        RationalResampler sharp(6, 25, 64);
        const auto ys = runOneShot(sharp, x);
        CHECK(peakBin(ys, start, n) == kTone);
        const double peakS = dtftMag(ys, start, n, fOut);
        const double ampS = peakS / (static_cast<double>(n) / 2.0);
        const double floorS = worstOffPeak(ys, start, n, kTone, 3);
        CHECK_NEAR(ampS, 1.0, 0.01);
        CHECK(floorS < 1e-3 * peakS);
    }

    // --- Upsample 3/2 (e.g. 32 kHz -> 48 kHz): same absolute frequency -----
    // fIn = 0.05 -> fOut = fIn*2/3 = 1/30; a 480-sample DFT puts the tone
    // exactly on bin 16.
    {
        const double fIn = 0.05;
        const double fOut = fIn * 2.0 / 3.0;
        const std::size_t n = 480;
        CHECK_NEAR(fOut * static_cast<double>(n), 16.0, 1e-9);

        RationalResampler rs(3, 2);
        const auto x = toneSignal(4000, fIn);
        const auto y = runOneShot(rs, x);
        CHECK(y.size() == 6000);  // ceil(4000*3/2)

        const std::size_t start = 200;
        const std::size_t kTone = 16;
        CHECK(peakBin(y, start, n) == kTone);
        // Measured 0.999992 gain, alias floor 4.6e-6 of the peak (here the
        // 0.5/3 cutoff is wide, so the tone sits deep in the flat passband).
        const double peak = dtftMag(y, start, n, fOut);
        const double amp = peak / (static_cast<double>(n) / 2.0);
        const double floor = worstOffPeak(y, start, n, kTone, 3);
        CHECK_NEAR(amp, 1.0, 0.01);
        CHECK(floor < 1e-3 * peak);
    }

    // --- DC in -> DC out at gain 1 within 0.1% ------------------------------
    {
        const struct { unsigned L, M; } ratios[] = {{6, 25}, {3, 2}};
        for (const auto& r : ratios) {
            RationalResampler rs(r.L, r.M);
            const std::vector<float> x(2000, 1.0f);
            const auto y = runOneShot(rs, x);
            // Outputs are fully warmed once their newest input index reaches
            // the branch length; skipping 50 covers every ratio here.
            CHECK(y.size() > 100);
            // Contract is 0.1%; per-branch tap normalization makes it exact
            // in practice (measured worst deviation: 0.0 for both ratios).
            double worst = 0.0;
            for (std::size_t k = 50; k < y.size(); ++k) {
                worst = std::max(
                    worst, std::fabs(static_cast<double>(y[k]) - 1.0));
            }
            CHECK(worst <= 1e-3);
        }
    }

    // --- Chunked processing == one-shot, bit-identical ----------------------
    // History (hist_), branch phase and skip carry across calls, so the same
    // double dot products run on the same values regardless of block split.
    {
        const auto x = lcgSignal(4096, 0xC0FFEEu);
        const struct { unsigned L, M; } ratios[] = {{6, 25}, {3, 2}, {5, 3}};
        const std::size_t blockSizes[] = {1, 3, 5, 7, 16, 37, 129, 1000};
        for (const auto& r : ratios) {
            RationalResampler one(r.L, r.M);
            const auto ref = runOneShot(one, x);
            for (const std::size_t bs : blockSizes) {
                RationalResampler rs(r.L, r.M);
                const auto y = runChunked(rs, x, bs);
                checkSameSignal(y, ref, 0.0);
            }
        }
    }

    // --- gcd reduction: (12, 50) is bit-identical to (6, 25) ---------------
    {
        const auto x = lcgSignal(2048, 0xBEEF01u);
        RationalResampler a(12, 50);
        RationalResampler b(6, 25);
        checkSameSignal(runOneShot(a, x), runOneShot(b, x), 0.0);
        CHECK(a.maxOut(1000) == b.maxOut(1000));
    }

    // --- 1/1 (and 7/7, reduced) passes samples through untouched -----------
    {
        const auto x = lcgSignal(512, 0xA5A5A5u);
        RationalResampler rs(7, 7);
        const auto y = runOneShot(rs, x);
        checkSameSignal(y, x, 0.0);
    }

    // --- reset() returns the stream to freshly-constructed state -----------
    // Prefix length 511 parks the state mid-stride for 6/25 (after 511 inputs
    // the next output needs 1 more input and branch phase 3), so a reset()
    // that missed skip_, phase_ or hist_ would each show up here.
    {
        const auto x = lcgSignal(1024, 0xD00DFEu);
        RationalResampler fresh(6, 25);
        const auto ref = runOneShot(fresh, x);

        RationalResampler rs(6, 25);
        const auto prefix = lcgSignal(511, 0x1234u);
        (void)runOneShot(rs, prefix);
        rs.reset();
        const auto y = runOneShot(rs, x);
        checkSameSignal(y, ref, 0.0);
    }

    // --- maxOut is never exceeded over a long fixed-LCG run ----------------
    // Random block sizes 1..64, three ratios; also checks the exact total:
    // a fresh stream over nTotal inputs must produce ceil(nTotal*L/M) outputs
    // (reduced L/M) no matter how the input was chunked.
    {
        const struct { unsigned L, M; } ratios[] = {{6, 25}, {3, 2}, {5, 3}};
        for (const auto& r : ratios) {
            RationalResampler rs(r.L, r.M);
            Lcg rng(0xFEED5EEDu);
            Lcg sig(0x0BADF00Du);
            std::size_t totalIn = 0;
            std::size_t totalOut = 0;
            std::vector<float> block;
            std::vector<float> buf;
            for (int call = 0; call < 2000; ++call) {
                const std::size_t bs = 1 + (rng.nextU32() % 64u);
                block.resize(bs);
                for (auto& s : block) { s = sig.next(); }
                buf.resize(rs.maxOut(bs));
                const std::size_t got =
                    rs.process(block.data(), bs, buf.data(), buf.size());
                CHECK(got <= rs.maxOut(bs));
                totalIn += bs;
                totalOut += got;
            }
            const unsigned g = std::gcd(r.L, r.M);
            const std::size_t Lr = r.L / g;
            const std::size_t Mr = r.M / g;
            CHECK(totalOut == (totalIn * Lr + Mr - 1) / Mr);
        }
    }

    return testSummary("test_resampler");
}
