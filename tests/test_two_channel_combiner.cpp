// Tests for dsp/two_channel_combiner.hpp.
//
// Scene: a wanted signal w, an interferer i and thermal noise n0, n1, all
// independent complex Gaussian sequences (fixed seeds). Channel 0 carries
// w + i + n0, channel 1 carries rw*w + ri*i + n1 with DIFFERENT complex
// coupling (amplitude and phase) for the wanted signal and the interferer,
// which is the only thing that lets a two-element combiner separate them.
// To measure what a set of weights does to each component, the same weights
// are applied to wanted-only, interferer-only and noise-only streams generated
// with the same couplings (the weights are frozen first), so every figure is a
// per-component power gain with nothing else mixed in.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "dsp/two_channel_combiner.hpp"

#include "test_check.hpp"

#include <cmath>
#include <complex>
#include <chrono>
#include <cstdio>
#include <random>
#include <vector>

namespace {

using cascade::dsp::Cov2;
using cascade::dsp::Gains2;
using cascade::dsp::TwoChannelCombiner;
using cf = std::complex<float>;
using cd = std::complex<double>;
constexpr double kTwoPi = 6.283185307179586476925286766559;

struct Scene {
    cd rw = std::polar(0.8, 0.5);    // wanted: ch1/ch0
    cd ri = std::polar(0.9, -1.2);   // interferer: ch1/ch0
    double pw = 1.0, pi = 1.0, pn = 1e-3;
    double wDriftCyclesPerSample = 0.0;  // phase drift of ch1 relative to ch0 (all sources)
    double iDriftCyclesPerSample = 0.0;  // extra drift on the interferer path only
};

struct Streams { std::vector<cf> x0, x1; };

cd gauss(std::mt19937& g, double p) {
    std::normal_distribution<double> nd(0.0, std::sqrt(p / 2.0));
    return {nd(g), nd(g)};
}

// Component selectors: which of w / i / n are present.
Streams make(const Scene& s, std::size_t n, std::size_t offset, unsigned seed, bool w, bool i, bool noise) {
    std::mt19937 gw(seed), gi(seed + 1000), gn(seed + 2000);
    Streams o;
    o.x0.resize(n);
    o.x1.resize(n);
    for (std::size_t k = 0; k < n + offset; ++k) {
        const cd sw = gauss(gw, s.pw), si = gauss(gi, s.pi), n0 = gauss(gn, s.pn), n1 = gauss(gn, s.pn);
        if (k < offset) { continue; }
        const double t = static_cast<double>(k);
        const cd drift = std::polar(1.0, kTwoPi * s.wDriftCyclesPerSample * t);
        const cd idrift = std::polar(1.0, kTwoPi * s.iDriftCyclesPerSample * t);
        cd a = 0.0, b = 0.0;
        if (w) { a += sw; b += s.rw * sw * drift; }
        if (i) { a += si; b += s.ri * si * drift * idrift; }
        if (noise) { a += n0; b += n1; }
        o.x0[k - offset] = cf(static_cast<float>(a.real()), static_cast<float>(a.imag()));
        o.x1[k - offset] = cf(static_cast<float>(b.real()), static_cast<float>(b.imag()));
    }
    return o;
}

double meanPower(const std::vector<cf>& v) {
    double p = 0.0;
    for (const cf& c : v) { p += std::norm(c); }
    return p / static_cast<double>(v.size());
}

double db(double x) { return 10.0 * std::log10(x); }

// Power gain of the combiner (weights as they stand, frozen) on a component,
// referred to channel 0's power of the same component.
double gainDb(TwoChannelCombiner& c, const Scene& s, bool w, bool i, bool noise, std::size_t offset = 0) {
    const Streams st = make(s, 40000, offset, 777, w, i, noise);
    std::vector<cf> y(st.x0.size());
    c.freeze();
    c.process(st.x0.data(), st.x1.data(), st.x0.size(), y.data());
    return db(meanPower(y) / meanPower(st.x0) + 1e-30);  // floor: an exact null is -300 dB, not -inf
}

}  // namespace

int main() {
    // ---- Manual weight: nulls the interferer exactly, wanted gain is analytic.
    {
        Scene s;
        TwoChannelCombiner c;
        c.setManual(1.0 / std::abs(s.ri), kTwoPi / 2.0 - std::arg(s.ri));  // g1 = -1/ri
        c.setMode(TwoChannelCombiner::Mode::Manual);
        const double iRej = -gainDb(c, s, false, true, false);
        const double wGain = gainDb(c, s, true, false, false);
        const double expect = db(std::norm(1.0 - s.rw / s.ri));
        std::printf("combiner manual: interferer rejection %.1f dB, wanted gain %.2f dB (expect %.2f)\n", iRej, wGain, expect);
        CHECK(iRej > 90.0);
        CHECK(std::fabs(wGain - expect) < 0.05);
        // g0 stays 1: the manual weight is a pure ch1 weight.
        CHECK(std::abs(c.gains().g0 - cd(1.0, 0.0)) < 1e-12);
    }

    // ---- Manual weight when wanted and interferer arrive with the SAME coupling:
    // the null removes the wanted signal too. Stated limit, measured.
    {
        Scene s;
        s.rw = s.ri;
        TwoChannelCombiner c;
        c.setManual(1.0 / std::abs(s.ri), kTwoPi / 2.0 - std::arg(s.ri));
        const double wGain = gainDb(c, s, true, false, false);
        std::printf("combiner manual, wanted from the same direction as the interferer: wanted gain %.1f dB\n", wGain);
        CHECK(wGain < -90.0);
    }

    // ---- Maximum-ratio combining: SNR gain 10 log10(1 + |rho|^2).
    {
        const double mags[] = {1.0, 0.5};
        for (double m : mags) {
            Scene s;
            s.rw = std::polar(m, 0.9);
            s.pw = 1.0;
            s.pn = 0.1;
            // Auto steering: adapt on wanted + noise.
            TwoChannelCombiner c;
            c.setMode(TwoChannelCombiner::Mode::MaxRatio);
            c.setForgetting(8192.0);
            const Streams adapt = make(s, 60000, 0, 11, true, false, true);
            std::vector<cf> y(adapt.x0.size());
            c.process(adapt.x0.data(), adapt.x1.data(), adapt.x0.size(), y.data());
            const double wDb = gainDb(c, s, true, false, false);
            const double nDb = gainDb(c, s, false, false, true);
            const double snrGain = wDb - nDb;
            const double expect = db(1.0 + m * m);
            std::printf("combiner MRC |rho|=%.1f: SNR gain %.2f dB (theory %.2f), wanted gain %.2f dB\n", m, snrGain, expect, wDb);
            CHECK(std::fabs(snrGain - expect) < 0.25);
            CHECK(std::fabs(wDb) < 0.1);  // wanted referenced to channel 0 comes out unchanged
        }
    }

    // ---- Capon with exact steering, interferer 20 dB above noise, wanted 10 dB above noise.
    {
        Scene s;
        s.pw = 1.0;
        s.pn = 0.1;
        s.pi = 10.0;
        TwoChannelCombiner c;
        c.setMode(TwoChannelCombiner::Mode::Capon);
        c.setSteering(s.rw);
        c.setForgetting(8192.0);
        const Streams adapt = make(s, 80000, 0, 21, true, true, true);
        std::vector<cf> y(adapt.x0.size());
        c.process(adapt.x0.data(), adapt.x1.data(), adapt.x0.size(), y.data());
        const double iRej = -gainDb(c, s, false, true, false);
        const double wGain = gainDb(c, s, true, false, false);
        const double nGain = gainDb(c, s, false, false, true);
        const double sinrIn = db(s.pw / (s.pi + s.pn));
        const double sinrOut = db(s.pw * std::pow(10.0, wGain / 10.0) /
                                  (s.pi * std::pow(10.0, -iRej / 10.0) + s.pn * std::pow(10.0, nGain / 10.0)));
        std::printf("combiner Capon: interferer rejection %.1f dB, wanted gain %.3f dB, noise gain %.2f dB, SINR %.1f -> %.1f dB\n",
                    iRej, wGain, nGain, sinrIn, sinrOut);
        CHECK(iRej > 18.0);
        CHECK(std::fabs(wGain) < 0.05);       // distortionless on the wanted signal
        CHECK(nGain < 3.5);                    // wanted loss bounded: noise not amplified much
        CHECK(sinrOut > 9.0);  // noise-limited: the 10 dB wanted SNR minus about 0.4 dB of noise gain
    }

    // ---- Capon with NO steering given: uses the dominant eigenvector, which is
    // the interferer when it dominates. That nulls the WRONG signal; stated.
    {
        Scene s;
        s.pw = 1.0; s.pn = 0.1; s.pi = 10.0;
        TwoChannelCombiner c;
        c.setMode(TwoChannelCombiner::Mode::Capon);
        c.setForgetting(8192.0);
        const Streams adapt = make(s, 80000, 0, 22, true, true, true);
        std::vector<cf> y(adapt.x0.size());
        c.process(adapt.x0.data(), adapt.x1.data(), adapt.x0.size(), y.data());
        const double wGain = gainDb(c, s, true, false, false);
        std::printf("combiner Capon with auto steering on an interferer-dominated scene: wanted gain %.1f dB (it steers on the interferer)\n", wGain);
        CHECK(std::isfinite(wGain));  // printed only: steering on the dominant source nulls the wrong one
    }

    // ---- Blind reference null (no wanted-signal knowledge), interferer 30 dB above wanted.
    {
        Scene s;
        s.pw = 0.01; s.pi = 10.0; s.pn = 1e-4;
        TwoChannelCombiner c;
        c.setMode(TwoChannelCombiner::Mode::ReferenceNull);
        c.setForgetting(8192.0);
        const Streams adapt = make(s, 80000, 0, 31, true, true, true);
        std::vector<cf> y(adapt.x0.size());
        c.process(adapt.x0.data(), adapt.x1.data(), adapt.x0.size(), y.data());
        const double iRej = -gainDb(c, s, false, true, false);
        const double wGain = gainDb(c, s, true, false, false);
        std::printf("combiner reference-null, interferer 30 dB over wanted: rejection %.1f dB, wanted gain %.2f dB\n", iRej, wGain);
        CHECK(iRej > 25.0);
        CHECK(wGain > -6.0);   // wanted loss bounded by the coupling geometry: |1 - rw/ri| ~ 0.2..1.8
        // Equal-power wanted and interferer: it cannot tell them apart, the
        // rejection is much smaller. Measured and printed; the limit is the
        // reason the Capon mode with a steering vector exists.
        Scene e = s;
        e.pw = 10.0;
        TwoChannelCombiner c2;
        c2.setMode(TwoChannelCombiner::Mode::ReferenceNull);
        c2.setForgetting(8192.0);
        const Streams a2 = make(e, 80000, 0, 32, true, true, true);
        std::vector<cf> y2(a2.x0.size());
        c2.process(a2.x0.data(), a2.x1.data(), a2.x0.size(), y2.data());
        const double iRej2 = -gainDb(c2, e, false, true, false);
        std::printf("combiner reference-null, equal power wanted/interferer: interferer rejection %.1f dB\n", iRej2);
        CHECK(iRej2 < iRej - 10.0);
    }

    // ---- Coherence: a slow phase drift between the two channels.
    // Frozen weights (solved at t=0) against drift; and the adaptive tracker
    // (forgetting 2048 samples) against drift rate. Units: cycles per sample.
    {
        std::printf("combiner drift (interferer-only scene; rejection = ch0 power / output power)\n");
        const double rates[] = {0.0, 1e-5, 1e-4, 1e-3, 1e-2};
        double adaptiveAt1e4 = 0.0, adaptiveAt0 = 0.0, adaptiveAt1e5 = 0.0, frozenAt1e5 = 0.0, fastAt1e4 = 0.0;
        for (double f : rates) {
            Scene s;
            s.pw = 0.0; s.pi = 1.0; s.pn = 1e-5;
            // The channels' coupling drifts: ch1 = ri * i * e^{j 2 pi f k}.
            s.iDriftCyclesPerSample = f;
            // Frozen: weights solved on an UNDRIFTED snapshot, applied later.
            Scene s0 = s;
            s0.iDriftCyclesPerSample = 0.0;
            TwoChannelCombiner cf0;
            cf0.setMode(TwoChannelCombiner::Mode::ReferenceNull);
            cf0.setForgetting(2048.0);
            const Streams snap = make(s0, 20000, 0, 41, false, true, true);
            std::vector<cf> y(snap.x0.size());
            cf0.process(snap.x0.data(), snap.x1.data(), snap.x0.size(), y.data());
            const double frozenRej = -gainDb(cf0, s, false, true, true, 20000);
            // Adaptive, slow (tau 2048) and fast (tau 256) tracking.
            double adaptRejTau[2] = {0.0, 0.0};
            const double taus[2] = {2048.0, 256.0};
            for (int q = 0; q < 2; ++q) {
                TwoChannelCombiner ca;
                ca.setMode(TwoChannelCombiner::Mode::ReferenceNull);
                ca.setForgetting(taus[q]);
                ca.setUpdateInterval(16);
                const Streams run = make(s, 60000, 0, 42, false, true, true);
                std::vector<cf> y2(run.x0.size());
                ca.process(run.x0.data(), run.x1.data(), run.x0.size(), y2.data());
                std::vector<cf> tail(y2.begin() + 30000, y2.end());
                std::vector<cf> tail0(run.x0.begin() + 30000, run.x0.end());
                adaptRejTau[q] = -db(meanPower(tail) / meanPower(tail0));
            }
            const double adaptRej = adaptRejTau[0];
            std::printf("  drift %.0e cycles/sample: frozen weights %.1f dB, adaptive tau 2048 %.1f dB, tau 256 %.1f dB\n",
                        f, frozenRej, adaptRejTau[0], adaptRejTau[1]);
            if (f == 0.0) { adaptiveAt0 = adaptRej; }
            if (f == 1e-5) { adaptiveAt1e5 = adaptRej; frozenAt1e5 = frozenRej; }
            if (f == 1e-4) { adaptiveAt1e4 = adaptRej; fastAt1e4 = adaptRejTau[1]; }
        }
        CHECK(adaptiveAt0 > 40.0);
        CHECK(adaptiveAt1e5 > 12.0);            // slow drift: the tracker follows it
        CHECK(frozenAt1e5 < 0.0);               // frozen weights: worse than no combiner
        CHECK(adaptiveAt1e4 < 6.0);             // drift of 0.2 cycles per time constant: tracking lost
        CHECK(fastAt1e4 > adaptiveAt1e4 + 3.0); // a shorter time constant recovers part of it
    }

    // ---- CPU cost of the adaptive Capon path, printed for the record.
    {
        Scene s;
        s.pi = 10.0;
        TwoChannelCombiner c;
        c.setMode(TwoChannelCombiner::Mode::Capon);
        c.setSteering(s.rw);
        const Streams st = make(s, 250000, 0, 51, true, true, true);
        std::vector<cf> y(st.x0.size());
        const auto t0 = std::chrono::steady_clock::now();
        for (int k = 0; k < 8; ++k) { c.process(st.x0.data(), st.x1.data(), st.x0.size(), y.data()); }
        const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("combiner cpu: 2 MS of two-channel input in %.3f s = %.1f ns per sample pair\n", sec, sec / 2.0e6 * 1e9);
    }

    // ---- Closed-form helpers.
    {
        Cov2 r;
        const cd rho = std::polar(0.6, 1.1);
        r.r00 = 1.0;
        r.r11 = std::norm(rho);
        r.r01 = std::conj(rho);  // E[x0 conj(x1)] for x1 = rho x0
        const cd e = cascade::dsp::combiner::dominantSteering(r);
        CHECK(std::abs(e - rho) < 1e-12);
        const Gains2 g = cascade::dsp::combiner::maxRatio(rho);
        // g applied to [1, rho] gives exactly 1.
        CHECK(std::abs(g.g0 + g.g1 * rho - cd(1.0, 0.0)) < 1e-12);
        const Cov2 empty;
        CHECK(std::abs(cascade::dsp::combiner::dominantSteering(empty)) == 0.0);
        // Rank-1 covariance does not blow up the Capon solver (diagonal loading).
        const Gains2 cg = cascade::dsp::combiner::capon(r, rho * cd(0.5, 0.0), 1e-4);
        CHECK(std::isfinite(cg.g0.real()) && std::isfinite(cg.g1.real()));
    }

    return testSummary("test_two_channel_combiner");
}
