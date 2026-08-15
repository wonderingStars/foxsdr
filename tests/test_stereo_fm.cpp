// Tests for dsp/stereo_fm.hpp — the broadcast-FM stereo (MPX) decoder.
//
// Every composite signal here is synthesized in-test from the published
// multiplex standard: 0.45*(L+R) baseband, a 19 kHz pilot, and 0.45*(L-R) on a
// DSB-SC subcarrier at exactly twice the pilot frequency AND twice its phase.
// Measurements are made with a direct DFT written here, against analytic
// expectations derived from those synthesis constants — never against numbers
// read back from the implementation.
//
// The pilot is deliberately given a non-zero absolute phase (and, in one test,
// a frequency offset): a real transmitter's pilot phase is arbitrary at the
// receiver, and only a decoder that genuinely locks to it can separate the
// channels. A decoder whose 38 kHz came from a free-running oscillator would
// pass none of the separation tests below.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "dsp/stereo_fm.hpp"

#include "test_check.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace {

constexpr double kTwoPi = 6.283185307179586476925286766559;

// 200 kHz is a typical WFM channel rate and comfortably above the 106 kHz the
// decoder needs for the full 53 kHz composite.
constexpr double kFs = 200000.0;
// Analysis window: 20 Hz bins, so 1 kHz, 2.5 kHz and 10 kHz all land on
// integer bins and the DFT is coherent (a tone contributes to its own bin
// only, so measured crosstalk is real crosstalk and not spectral leakage).
constexpr std::size_t kN = 10000;
// 200 ms of warm-up: PLL acquisition (~11 ms time constant), the level
// estimators (10 ms) and the stereo gate ramp (20 ms) all settle inside it.
constexpr std::size_t kWarm = 40000;

constexpr double kPilotHz = 19000.0;
// Standard composite weighting: audio at 45% each, pilot at 10%.
constexpr double kAudioScale = 0.45;
constexpr double kPilotAmp = 0.1;

using cascade::dsp::StereoFm;

struct Tone {
    double hz;
    double amp;
};

double toneSum(const std::vector<Tone>& tones, double t) {
    double v = 0.0;
    for (const Tone& x : tones) { v += x.amp * std::cos(kTwoPi * x.hz * t); }
    return v;
}

// Builds a broadcast-FM composite from explicit L and R programme content.
//
//   mpx = 0.45*(L+R) + pilotAmp*cos(p) + 0.45*(L-R)*cos(2p),  p = 2*pi*fp*t+ph
//
// The subcarrier is the pilot's exact second harmonic in both frequency and
// phase — that is the relationship the standard defines and the only reason a
// receiver can regenerate a suppressed carrier at all.
std::vector<float> makeComposite(std::size_t n, const std::vector<Tone>& left,
                                 const std::vector<Tone>& right,
                                 double pilotAmp, double pilotHz,
                                 double pilotPhase) {
    std::vector<float> mpx(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / kFs;
        const double l = toneSum(left, t);
        const double r = toneSum(right, t);
        const double p = kTwoPi * pilotHz * t + pilotPhase;
        const double v = kAudioScale * (l + r) + pilotAmp * std::cos(p) +
                         kAudioScale * (l - r) * std::cos(2.0 * p);
        mpx[i] = static_cast<float>(v);
    }
    return mpx;
}

struct Stereo {
    std::vector<float> left;
    std::vector<float> right;
};

Stereo run(StereoFm& d, const std::vector<float>& mpx) {
    Stereo o;
    o.left.resize(mpx.size());
    o.right.resize(mpx.size());
    CHECK(d.process(mpx.data(), mpx.size(), o.left.data(), o.right.data()) ==
          mpx.size());
    return o;
}

// Direct-DFT magnitude at bin k — the independent spectral reference.
double dftMag(const float* x, std::size_t n, std::size_t k) {
    double re = 0.0;
    double im = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double ph = kTwoPi * static_cast<double>(k) *
                          static_cast<double>(i) / static_cast<double>(n);
        re += static_cast<double>(x[i]) * std::cos(ph);
        im -= static_cast<double>(x[i]) * std::sin(ph);
    }
    return std::sqrt(re * re + im * im);
}

// Amplitude of a coherently sampled real sinusoid: |X[k]| = A*n/2.
double binAmp(const float* x, std::size_t n, std::size_t k) {
    return 2.0 * dftMag(x, n, k) / static_cast<double>(n);
}

std::size_t binOf(double hz) {
    return static_cast<std::size_t>(hz * static_cast<double>(kN) / kFs + 0.5);
}

double meanSquare(const float* x, std::size_t n) {
    double s = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        s += static_cast<double>(x[i]) * static_cast<double>(x[i]);
    }
    return s / static_cast<double>(n);
}

double dB(double ratio) { return 20.0 * std::log10(ratio); }

// Largest sample-to-sample step over [lo, hi) — the click detector.
double maxStep(const std::vector<float>& y, std::size_t lo, std::size_t hi) {
    double m = 0.0;
    for (std::size_t i = lo + 1; i < hi; ++i) {
        m = std::max(m, std::fabs(static_cast<double>(y[i]) -
                                  static_cast<double>(y[i - 1])));
    }
    return m;
}

// Fixed-seed LCG (Numerical Recipes constants); the only randomness anywhere
// in this file, used to dirty a decoder's state before reset().
float lcgUnit(std::uint32_t& s) {
    s = s * 1664525u + 1013904223u;
    return static_cast<float>(s >> 8) * (2.0f / 16777216.0f) - 1.0f;
}

// Analytic one-pole de-emphasis magnitude from the SPEC time constant.
double deemphMag(double tauUs, double fHz) {
    if (tauUs <= 0.0) { return 1.0; }
    const double p = std::exp(-1.0 / (kFs * tauUs * 1.0e-6));
    const double w = kTwoPi * fHz / kFs;
    return (1.0 - p) / std::sqrt(1.0 - 2.0 * p * std::cos(w) + p * p);
}

}  // namespace

int main() {
    const std::size_t k1k = binOf(1000.0);
    const std::size_t k2k5 = binOf(2500.0);
    const std::size_t k10k = binOf(10000.0);

    // --- Channel separation: L = 1 kHz, R = 2.5 kHz -------------------------
    // The pilot carries an arbitrary absolute phase (0.9 rad), so recovering
    // either channel at all requires a phase-coherent regenerated subcarrier.
    //
    // MEASURED on this build (printed by the run below): 127.3 dB left,
    // 128.5 dB right. That is not a physical separation figure — it is the
    // float32 output noise floor plus the DFT's 40 dB processing gain, i.e.
    // the residual carrier phase error (~0.08 degrees) is small enough that
    // arithmetic precision, not the decoder, sets the number. The contract
    // asks for 25 dB; the second assertion at 60 dB is the useful one, a
    // regression floor with ~67 dB of margin below what is measured.
    {
        StereoFm d(kFs);
        CHECK(d.stereoCapable());
        d.setDeemphasisUs(0.0);  // flat: separation is the only variable here

        auto mpx = makeComposite(kWarm + kN, {{1000.0, 1.0}}, {{2500.0, 0.8}},
                                 kPilotAmp, kPilotHz, 0.9);
        auto o = run(d, mpx);
        CHECK(d.pilotLocked());
        CHECK(d.pilotLevel() > 0.9f);

        const float* L = o.left.data() + kWarm;
        const float* R = o.right.data() + kWarm;
        const double lWant = binAmp(L, kN, k1k);
        const double lLeak = binAmp(L, kN, k2k5);
        const double rWant = binAmp(R, kN, k2k5);
        const double rLeak = binAmp(R, kN, k1k);

        // The decoder is a matrix, not a normalizer: a hard-left tone of
        // amplitude A arrives as 0.45*A, the composite's own weighting.
        CHECK_NEAR(lWant, kAudioScale * 1.0, 0.02 * kAudioScale);
        CHECK_NEAR(rWant, kAudioScale * 0.8, 0.02 * kAudioScale * 0.8);

        const double sepL = dB(lWant / lLeak);
        const double sepR = dB(rWant / rLeak);
        std::printf("  separation: left %.1f dB, right %.1f dB\n", sepL, sepR);
        CHECK(sepL > 25.0);
        CHECK(sepR > 25.0);
        CHECK(sepL > 60.0);
        CHECK(sepR > 60.0);

        // Nothing else got into the right channel either: everything beyond
        // its own 2.5 kHz tone (leaked 1 kHz, pilot residue, subcarrier
        // residue) together stays 34 dB down.
        const double resid =
            std::sqrt(std::max(0.0, meanSquare(R, kN) - 0.5 * rWant * rWant));
        CHECK(resid < 0.02 * rWant);
    }

    // --- Mono composite: no pilot, no difference ----------------------------
    {
        StereoFm d(kFs);
        d.setDeemphasisUs(0.0);
        auto mpx = makeComposite(kWarm + kN, {{1000.0, 1.0}}, {{1000.0, 1.0}},
                                 0.0, kPilotHz, 0.0);
        auto o = run(d, mpx);

        CHECK(!d.pilotLocked());
        CHECK(d.pilotLevel() < 0.2f);
        // With the stereo gate shut the difference term is exactly zero, so
        // the two channels are bit-identical, not merely close.
        bool identical = true;
        for (std::size_t i = 0; i < o.left.size(); ++i) {
            if (o.left[i] != o.right[i]) { identical = false; }
        }
        CHECK(identical);

        // L = R = (L+R)/2 = 0.45 * the programme tone.
        const double a = binAmp(o.left.data() + kWarm, kN, k1k);
        CHECK_NEAR(a, kAudioScale * 1.0, 0.02 * kAudioScale);
    }

    // --- Pilot present, zero difference signal (L == R) ---------------------
    {
        StereoFm d(kFs);
        d.setDeemphasisUs(0.0);
        auto mpx = makeComposite(kWarm + kN, {{1000.0, 1.0}}, {{1000.0, 1.0}},
                                 kPilotAmp, kPilotHz, -0.4);
        auto o = run(d, mpx);

        CHECK(d.pilotLocked());
        CHECK(d.pilotLevel() > 0.9f);

        const float* L = o.left.data() + kWarm;
        const float* R = o.right.data() + kWarm;
        const double a = binAmp(L, kN, k1k);
        CHECK_NEAR(a, kAudioScale * 1.0, 0.02 * kAudioScale);

        // The gate is fully open, so this is the real test: with a locked
        // carrier and no (L-R) content the difference path must contribute
        // nothing measurable to either channel.
        std::vector<float> diff(kN);
        for (std::size_t i = 0; i < kN; ++i) { diff[i] = L[i] - R[i]; }
        CHECK(binAmp(diff.data(), kN, k1k) < 1.0e-3 * a);
        CHECK(std::sqrt(meanSquare(diff.data(), kN)) < 1.0e-2 * a);
    }

    // --- setForceMono(true) over a full stereo composite --------------------
    {
        StereoFm d(kFs);
        d.setDeemphasisUs(0.0);
        d.setForceMono(true);
        CHECK(d.forceMono());
        auto mpx = makeComposite(kWarm + kN, {{1000.0, 1.0}}, {{2500.0, 0.8}},
                                 kPilotAmp, kPilotHz, 0.9);
        auto o = run(d, mpx);

        // The pilot is still detected and reported — force-mono is a listening
        // choice, not a blindfold — but the outputs are exactly equal.
        CHECK(d.pilotLocked());
        bool identical = true;
        for (std::size_t i = 0; i < o.left.size(); ++i) {
            if (o.left[i] != o.right[i]) { identical = false; }
        }
        CHECK(identical);

        // And what is heard is the true mono sum: 0.45*(L+R)/2 per tone.
        const float* L = o.left.data() + kWarm;
        CHECK_NEAR(binAmp(L, kN, k1k), 0.5 * kAudioScale * 1.0,
                   0.02 * kAudioScale);
        CHECK_NEAR(binAmp(L, kN, k2k5), 0.5 * kAudioScale * 0.8,
                   0.02 * kAudioScale);
    }

    // --- Pilot 2 Hz high: the loop must TRACK, not merely sit at 19 kHz -----
    // A fixed 38 kHz oscillator (or a type-1 loop with a static phase error)
    // cannot pass this: 2 Hz of pilot offset is 4 Hz of subcarrier offset, so
    // the carrier phase would walk a full turn every 250 ms and separation
    // would average out to nothing over the analysis window.
    // MEASURED: 128.3 dB left, 128.8 dB right — indistinguishable from the
    // on-frequency case, which is the type-2 loop's zero steady-state phase
    // error showing up as zero separation penalty.
    {
        StereoFm d(kFs);
        d.setDeemphasisUs(0.0);
        auto mpx = makeComposite(kWarm + kN, {{1000.0, 1.0}}, {{2500.0, 0.8}},
                                 kPilotAmp, kPilotHz + 2.0, -1.7);
        auto o = run(d, mpx);
        CHECK(d.pilotLocked());

        const float* L = o.left.data() + kWarm;
        const float* R = o.right.data() + kWarm;
        const double lWant = binAmp(L, kN, k1k);
        const double rWant = binAmp(R, kN, k2k5);
        const double sepL = dB(lWant / binAmp(L, kN, k2k5));
        const double sepR = dB(rWant / binAmp(R, kN, k1k));
        std::printf("  separation (pilot +2 Hz): left %.1f dB, right %.1f dB\n",
                    sepL, sepR);
        CHECK_NEAR(lWant, kAudioScale * 1.0, 0.02 * kAudioScale);
        CHECK(sepL > 25.0);
        CHECK(sepR > 25.0);
        CHECK(sepL > 60.0);
        CHECK(sepR > 60.0);
    }

    // --- Chunked processing equals one-shot, bit for bit --------------------
    {
        auto mpx = makeComposite(30000, {{1000.0, 1.0}}, {{2500.0, 0.8}},
                                 kPilotAmp, kPilotHz, 0.9);
        StereoFm one(kFs);
        StereoFm chunked(kFs);

        auto a = run(one, mpx);

        std::vector<float> bl(mpx.size());
        std::vector<float> br(mpx.size());
        // Ragged splits including single-sample blocks, so every piece of
        // state — NCO phase, both FIR histories, the PLL, the level
        // estimators, the gate ramp — is walked across a seam.
        const std::size_t splits[] = {1, 4097, 3, 8191, 2, 111, 9999, 7596};
        std::size_t pos = 0;
        for (std::size_t len : splits) {
            CHECK(chunked.process(mpx.data() + pos, len, bl.data() + pos,
                                  br.data() + pos) == len);
            pos += len;
        }
        CHECK(pos == mpx.size());

        bool same = true;
        for (std::size_t i = 0; i < mpx.size(); ++i) {
            if (bl[i] != a.left[i] || br[i] != a.right[i]) { same = false; }
        }
        CHECK(same);
        CHECK(chunked.pilotLocked() == one.pilotLocked());
    }

    // --- reset() equals a fresh object --------------------------------------
    {
        auto block = makeComposite(4096, {{1000.0, 1.0}}, {{2500.0, 0.8}},
                                   kPilotAmp, kPilotHz, 0.9);

        // Dirty every state: a locked stereo composite (charges the PLL, the
        // gate and both FIRs) followed by noise (drives the loop off).
        auto dirtA = makeComposite(20000, {{700.0, 0.9}}, {{3100.0, 0.6}},
                                   kPilotAmp, kPilotHz + 40.0, 2.2);
        std::vector<float> dirtB(5000);
        std::uint32_t seed = 0xC0FFEEu;
        for (float& v : dirtB) { v = 0.5f * lcgUnit(seed); }

        StereoFm dirty(kFs);
        run(dirty, dirtA);
        run(dirty, dirtB);
        dirty.reset();

        StereoFm fresh(kFs);
        CHECK(dirty.pilotLocked() == fresh.pilotLocked());
        CHECK(dirty.pilotLevel() == fresh.pilotLevel());

        auto a = run(dirty, block);
        auto b = run(fresh, block);
        bool same = true;
        for (std::size_t i = 0; i < block.size(); ++i) {
            if (a.left[i] != b.left[i] || a.right[i] != b.right[i]) {
                same = false;
            }
        }
        CHECK(same);
    }

    // --- De-emphasis: 0 / 50 / 75 us measurably differ on BOTH channels -----
    {
        // Both channels carry 1 kHz and 10 kHz, at different levels, so the
        // measurement is per-channel and cannot be satisfied by a filter
        // sitting in a shared mono path.
        const std::vector<Tone> left = {{1000.0, 1.0}, {10000.0, 0.8}};
        const std::vector<Tone> right = {{1000.0, 0.6}, {10000.0, 0.5}};
        auto mpx = makeComposite(kWarm + kN, left, right, kPilotAmp, kPilotHz,
                                 0.9);

        double tiltL[3] = {0.0, 0.0, 0.0};
        double tiltR[3] = {0.0, 0.0, 0.0};
        const double taus[3] = {0.0, 50.0, 75.0};
        for (int t = 0; t < 3; ++t) {
            StereoFm d(kFs);
            d.setDeemphasisUs(taus[t]);
            CHECK_NEAR(d.deemphasisUs(), taus[t], 1e-9);
            auto o = run(d, mpx);
            CHECK(d.pilotLocked());

            const float* L = o.left.data() + kWarm;
            const float* R = o.right.data() + kWarm;
            // Divide out the programme's own 10 kHz : 1 kHz ratio, leaving the
            // de-emphasis network's response ratio alone.
            tiltL[t] = (binAmp(L, kN, k10k) / binAmp(L, kN, k1k)) / (0.8 / 1.0);
            tiltR[t] = (binAmp(R, kN, k10k) / binAmp(R, kN, k1k)) / (0.5 / 0.6);

            const double expected =
                deemphMag(taus[t], 10000.0) / deemphMag(taus[t], 1000.0);
            std::printf("  deemph %4.0f us: tilt L %.4f R %.4f (analytic %.4f)\n",
                        taus[t], tiltL[t], tiltR[t], expected);
            CHECK(std::fabs(dB(tiltL[t] / expected)) < 1.0);
            CHECK(std::fabs(dB(tiltR[t] / expected)) < 1.0);
        }

        // Off is flat; 50 us rolls off less than 75 us. A decoder that ignored
        // the setting, or applied one curve for both, fails here even though
        // each analytic check above could still pass.
        CHECK_NEAR(tiltL[0], 1.0, 0.03);
        CHECK_NEAR(tiltR[0], 1.0, 0.03);
        CHECK(tiltL[0] > tiltL[1]);
        CHECK(tiltL[1] > tiltL[2]);
        CHECK(tiltR[0] > tiltR[1]);
        CHECK(tiltR[1] > tiltR[2]);
    }

    // --- The stereo -> mono transition does not click -----------------------
    // A hard switch would step both channels by +/-(L-R)/2 at the instant of
    // the change: here that is up to 0.45*1.8/2 = 0.4, roughly 20x the largest
    // step the programme itself ever takes between samples. The ramped gate
    // must keep every step inside what the two steady states already produce.
    {
        const std::size_t kSwitch = 40000;
        const std::size_t kTotal = 60000;
        auto mpx = makeComposite(kTotal, {{1000.0, 1.0}}, {{2500.0, 0.8}},
                                 kPilotAmp, kPilotHz, 0.9);

        StereoFm d(kFs);
        d.setDeemphasisUs(0.0);
        std::vector<float> l(kTotal);
        std::vector<float> r(kTotal);
        CHECK(d.process(mpx.data(), kSwitch, l.data(), r.data()) == kSwitch);
        CHECK(d.pilotLocked());
        d.setForceMono(true);
        CHECK(d.process(mpx.data() + kSwitch, kTotal - kSwitch,
                        l.data() + kSwitch, r.data() + kSwitch) ==
              kTotal - kSwitch);

        // The switch really took effect: the tail is exactly mono.
        bool tailMono = true;
        for (std::size_t i = kTotal - 1000; i < kTotal; ++i) {
            if (l[i] != r[i]) { tailMono = false; }
        }
        CHECK(tailMono);

        const double pre = maxStep(l, 30000, 39000);   // steady stereo
        const double post = maxStep(l, 50000, 59000);  // steady mono
        const double trans = maxStep(l, kSwitch - 10, 46000);  // over the ramp
        std::printf("  gate ramp max step: pre %.5f post %.5f trans %.5f\n", pre,
                    post, trans);
        CHECK(trans < 1.05 * std::max(pre, post));
    }

    // --- Rates too low for a composite degrade to mono, not to garbage ------
    {
        StereoFm d(48000.0);
        CHECK(!d.stereoCapable());
        std::vector<float> mpx(4096);
        for (std::size_t i = 0; i < mpx.size(); ++i) {
            const double t = static_cast<double>(i) / 48000.0;
            mpx[i] = static_cast<float>(0.9 * std::cos(kTwoPi * 1000.0 * t));
        }
        std::vector<float> l(mpx.size());
        std::vector<float> r(mpx.size());
        CHECK(d.process(mpx.data(), mpx.size(), l.data(), r.data()) ==
              mpx.size());
        CHECK(!d.pilotLocked());
        bool identical = true;
        bool finite = true;
        for (std::size_t i = 0; i < mpx.size(); ++i) {
            if (l[i] != r[i]) { identical = false; }
            if (!std::isfinite(l[i])) { finite = false; }
        }
        CHECK(identical);
        CHECK(finite);

        // Zero-length calls are legal and produce nothing.
        CHECK(d.process(mpx.data(), 0, l.data(), r.data()) == 0);
    }

    return testSummary("test_stereo_fm");
}
