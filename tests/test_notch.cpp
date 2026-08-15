// Tests for dsp/notch.hpp.
//
// Everything is measured, never asserted against a constant lifted from the
// implementation: tone amplitudes come from an in-test coherent detector (a
// single-bin DFT at the exact tone frequency, over an integer number of
// cycles, so the two test tones are exactly orthogonal), and the -3 dB notch
// width is found by bisecting the filter's own measured response.
//
// SPDX-License-Identifier: MIT
#include "dsp/notch.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "test_check.hpp"

using cascade::dsp::AutoNotch;
using cascade::dsp::Notch;

namespace {

constexpr double kTwoPi = 6.283185307179586476925286766559;
constexpr double kFs = 48000.0;

// Fixed-seed LCG (Numerical Recipes constants) — no <random>, no device.
struct Lcg {
    std::uint32_t s;
    explicit Lcg(std::uint32_t seed) : s(seed) {}
    float next() {  // uniform in [-1, 1)
        s = s * 1664525u + 1013904223u;
        return static_cast<float>(s) * (2.0f / 4294967296.0f) - 1.0f;
    }
};

// Coherent single-bin DFT: returns the amplitude of the freqHz component.
// Exact when the window spans a whole number of cycles of every tone present.
double toneAmp(const float* x, std::size_t n, double freqHz) {
    double re = 0.0;
    double im = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double ang = kTwoPi * freqHz * static_cast<double>(i) / kFs;
        re += static_cast<double>(x[i]) * std::cos(ang);
        im -= static_cast<double>(x[i]) * std::sin(ang);
    }
    return 2.0 * std::sqrt(re * re + im * im) / static_cast<double>(n);
}

double db(double ratio) {
    return 20.0 * std::log10(ratio > 1e-300 ? ratio : 1e-300);
}

// Steady-state |H(f)| of a notch, measured by running a unit tone through it.
double measuredGain(double f0, double q, double f) {
    constexpr std::size_t kWarm = 16384;  // >> the Q=40 ring-down (~3k samples)
    constexpr std::size_t kMeas = 8192;
    Notch nf(kFs, f0, q);
    std::vector<float> x(kWarm + kMeas);
    for (std::size_t i = 0; i < x.size(); ++i) {
        x[i] = static_cast<float>(std::cos(kTwoPi * f * static_cast<double>(i) / kFs));
    }
    nf.process(x.data(), x.size());
    return toneAmp(x.data() + kWarm, kMeas, f);
}

// Distance from f0 at which the measured response reaches -3 dB, found by
// bisection on the real filter (the response is monotone in |f - f0| across
// the search span). sign = +1 searches above f0, -1 below.
double edge3dB(double f0, double q, double sign) {
    const double target = 0.7071067811865476;
    double lo = 0.0;                 // at f0 the gain is 0
    double hi = 6.0 * f0 / q;        // comfortably outside the null
    CHECK(measuredGain(f0, q, f0 + sign * hi) > target);
    for (int it = 0; it < 22; ++it) {
        const double mid = 0.5 * (lo + hi);
        if (measuredGain(f0, q, f0 + sign * mid) < target) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return 0.5 * (lo + hi);
}

std::vector<float> makeTwoTone(std::size_t n) {
    std::vector<float> x(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / kFs;
        x[i] = static_cast<float>(0.5 * std::cos(kTwoPi * 1000.0 * t) +
                                  0.5 * std::cos(kTwoPi * 3000.0 * t));
    }
    return x;
}

// One 512-sample analysis block of the AutoNotch test signal.
void fillBlock(std::vector<float>& v, std::size_t start, std::size_t n,
               double interfererAmp, double interfererHz, double wantedAmp,
               double noiseAmp, Lcg& rng) {
    v.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(start + i) / kFs;
        double s = interfererAmp * std::cos(kTwoPi * interfererHz * t) +
                   wantedAmp * std::cos(kTwoPi * 1031.25 * t);
        s += noiseAmp * static_cast<double>(rng.next());
        v[i] = static_cast<float>(s);
    }
}

}  // namespace

int main() {
    // ---------------------------------------------------------------------
    // Depth: a 3 kHz interferer alongside a 1 kHz wanted tone. 9600 samples
    // is 200 whole cycles of 1 kHz and 600 of 3 kHz, so the detector sees no
    // cross-leakage and the two numbers below are independent.
    // ---------------------------------------------------------------------
    {
        constexpr std::size_t kN = 24000;
        constexpr std::size_t kMeas = 9600;
        auto in = makeTwoTone(kN);
        auto out = in;
        Notch nf(kFs, 3000.0, 30.0);
        nf.process(out.data(), out.size());

        const std::size_t off = kN - kMeas;
        const double in1k = toneAmp(in.data() + off, kMeas, 1000.0);
        const double in3k = toneAmp(in.data() + off, kMeas, 3000.0);
        const double out1k = toneAmp(out.data() + off, kMeas, 1000.0);
        const double out3k = toneAmp(out.data() + off, kMeas, 3000.0);
        const double drop3k = db(out3k / in3k);
        const double delta1k = db(out1k / in1k);
        std::printf("notch f0=3000 Q=30: 3 kHz %+.2f dB, 1 kHz %+.4f dB\n", drop3k,
                    delta1k);
        CHECK(drop3k < -30.0);
        CHECK(std::fabs(delta1k) < 1.0);
    }

    // ---------------------------------------------------------------------
    // Endpoints: a notch requested at DC or at Nyquist must stay bounded.
    // Both are clamped away from the degenerate points where the poles would
    // land exactly on the unit circle (see notch.hpp).
    // ---------------------------------------------------------------------
    for (double request : {0.0, -1000.0, kFs * 0.5, kFs}) {
        constexpr std::size_t kN = 100000;
        Lcg rng(0xABCDEFu);
        Notch nf(kFs, 1000.0, 30.0);
        nf.setFrequencyHz(request);
        CHECK(nf.frequencyHz() > 0.0);
        CHECK(nf.frequencyHz() < kFs * 0.5);
        std::vector<float> x(kN);
        for (std::size_t i = 0; i < kN; ++i) { x[i] = rng.next(); }
        nf.process(x.data(), x.size());
        bool ok = true;
        for (std::size_t i = 0; i < kN; ++i) {
            if (!std::isfinite(x[i]) || std::fabs(x[i]) > 10.0f) { ok = false; }
        }
        CHECK(ok);
    }
    // A silly Q must not blow up either.
    for (double q : {0.0, -5.0, 1e9}) {
        Notch nf(kFs, 3000.0, q);
        std::vector<float> x(20000);
        Lcg rng(0x13579u);
        for (auto& v : x) { v = rng.next(); }
        nf.process(x.data(), x.size());
        bool ok = true;
        for (float v : x) {
            if (!std::isfinite(v) || std::fabs(v) > 10.0f) { ok = false; }
        }
        CHECK(ok);
    }

    // ---------------------------------------------------------------------
    // Q widens/narrows the null: measured -3 dB width against the textbook
    // f0/Q, and strictly ordered between the two Q values.
    // ---------------------------------------------------------------------
    {
        const double w10 = edge3dB(3000.0, 10.0, +1.0) + edge3dB(3000.0, 10.0, -1.0);
        const double w40 = edge3dB(3000.0, 40.0, +1.0) + edge3dB(3000.0, 40.0, -1.0);
        std::printf("notch -3 dB width: Q=10 -> %.1f Hz (f0/Q=300), "
                    "Q=40 -> %.1f Hz (f0/Q=75)\n",
                    w10, w40);
        CHECK(w40 < w10);
        CHECK(std::fabs(w10 - 300.0) < 0.20 * 300.0);
        CHECK(std::fabs(w40 - 75.0) < 0.20 * 75.0);
    }

    // ---------------------------------------------------------------------
    // Disabled is a bit-exact passthrough.
    // ---------------------------------------------------------------------
    {
        auto in = makeTwoTone(4096);
        auto out = in;
        Notch nf(kFs, 3000.0, 30.0);
        nf.setEnabled(false);
        nf.process(out.data(), out.size());
        std::size_t bad = 0;
        for (std::size_t i = 0; i < in.size(); ++i) {
            if (out[i] != in[i]) { ++bad; }
        }
        CHECK(bad == 0);
    }

    // ---------------------------------------------------------------------
    // Chunked == one-shot, over odd block sizes.
    // ---------------------------------------------------------------------
    {
        auto in = makeTwoTone(5000);
        auto oneShot = in;
        Notch a(kFs, 3000.0, 30.0);
        a.process(oneShot.data(), oneShot.size());

        auto chunked = in;
        Notch b(kFs, 3000.0, 30.0);
        const std::size_t sizes[] = {1, 3, 7, 13, 31, 193};
        std::size_t i = 0;
        std::size_t si = 0;
        while (i < chunked.size()) {
            std::size_t take = sizes[si % 6];
            ++si;
            if (i + take > chunked.size()) { take = chunked.size() - i; }
            b.process(chunked.data() + i, take);
            i += take;
        }
        std::size_t bad = 0;
        for (std::size_t k = 0; k < in.size(); ++k) {
            if (chunked[k] != oneShot[k]) { ++bad; }
        }
        CHECK(bad == 0);
    }

    // ---------------------------------------------------------------------
    // reset() == a freshly constructed filter.
    // ---------------------------------------------------------------------
    {
        auto in = makeTwoTone(3000);
        Notch a(kFs, 3000.0, 30.0);
        auto fresh = in;
        a.process(fresh.data(), fresh.size());

        Notch b(kFs, 3000.0, 30.0);
        auto scratch = in;
        b.process(scratch.data(), scratch.size());
        b.reset();
        auto afterReset = in;
        b.process(afterReset.data(), afterReset.size());

        std::size_t bad = 0;
        for (std::size_t k = 0; k < in.size(); ++k) {
            if (afterReset[k] != fresh[k]) { ++bad; }
        }
        CHECK(bad == 0);
    }

    // ---------------------------------------------------------------------
    // AutoNotch: parks on the strongest tone, kills it, leaves the wanted
    // tone alone.
    // ---------------------------------------------------------------------
    {
        AutoNotch an(kFs, 512, 30.0);
        Lcg rng(0x5EED01u);
        std::vector<float> blk;
        std::size_t t = 0;

        // 8 analysis blocks of 3 kHz interferer + 1031.25 Hz wanted + noise.
        for (int b = 0; b < 8; ++b) {
            fillBlock(blk, t, 512, 0.8, 3000.0, 0.2, 0.02, rng);
            an.process(blk.data(), blk.size());
            t += 512;
        }
        std::printf("autonotch: engaged=%d f=%.2f Hz prominence=%.1f dB\n",
                    an.isEngaged() ? 1 : 0, an.frequencyHz(),
                    static_cast<double>(an.prominenceDb()));
        CHECK(an.isEngaged());
        CHECK(std::fabs(an.frequencyHz() - 3000.0) < 20.0);

        // Measure over the next 9728 samples = 19 * 512, which is a whole
        // number of cycles of BOTH tones (608 of 3 kHz, 209 of 1031.25 Hz), so
        // the coherent detector sees them as exactly orthogonal.
        constexpr std::size_t kMeas = 9728;
        std::vector<float> sig;
        fillBlock(sig, t, kMeas, 0.8, 3000.0, 0.2, 0.02, rng);
        auto got = sig;
        an.process(got.data(), got.size());

        const double in3k = toneAmp(sig.data(), kMeas, 3000.0);
        const double out3k = toneAmp(got.data(), kMeas, 3000.0);
        const double in1k = toneAmp(sig.data(), kMeas, 1031.25);
        const double out1k = toneAmp(got.data(), kMeas, 1031.25);
        const double drop3k = db(out3k / in3k);
        const double delta1k = db(out1k / in1k);
        std::printf("autonotch suppression: 3 kHz %+.2f dB, 1031.25 Hz %+.3f dB\n",
                    drop3k, delta1k);
        CHECK(drop3k < -20.0);
        CHECK(std::fabs(delta1k) < 1.5);

        // Hysteresis: ONE anomalous block at a different frequency must not
        // move a parked notch; the second consecutive one may.
        const double parked = an.frequencyHz();
        std::vector<float> odd(512);
        for (std::size_t i = 0; i < odd.size(); ++i) {
            odd[i] = static_cast<float>(
                0.8 * std::cos(kTwoPi * 6000.0 * static_cast<double>(i) / kFs));
        }
        auto odd1 = odd;
        an.process(odd1.data(), odd1.size());
        CHECK(an.frequencyHz() == parked);  // confirmation count not yet met
        auto odd2 = odd;
        an.process(odd2.data(), odd2.size());
        std::printf("autonotch after 2 confirming blocks: f=%.2f Hz\n",
                    an.frequencyHz());
        CHECK(std::fabs(an.frequencyHz() - 6000.0) < 20.0);
    }

    // ---------------------------------------------------------------------
    // AutoNotch does not hunt once the tone is removed: it releases, and the
    // parked frequency never moves while only noise is present.
    // ---------------------------------------------------------------------
    {
        AutoNotch an(kFs, 512, 30.0);
        Lcg rng(0x0BADF00Du);
        std::vector<float> blk;
        std::size_t t = 0;
        for (int b = 0; b < 8; ++b) {
            fillBlock(blk, t, 512, 0.8, 3000.0, 0.2, 0.02, rng);
            an.process(blk.data(), blk.size());
            t += 512;
        }
        CHECK(an.isEngaged());
        const double parked = an.frequencyHz();

        double maxProm = -1e9;
        std::size_t moves = 0;
        for (int b = 0; b < 40; ++b) {
            std::vector<float> noise(512);
            for (auto& v : noise) { v = 0.3f * rng.next(); }
            an.process(noise.data(), noise.size());
            if (an.frequencyHz() != parked) { ++moves; }
            if (static_cast<double>(an.prominenceDb()) > maxProm) {
                maxProm = static_cast<double>(an.prominenceDb());
            }
        }
        std::printf("autonotch on noise only: moves=%zu engaged=%d "
                    "max prominence=%.1f dB (release threshold 16 dB)\n",
                    moves, an.isEngaged() ? 1 : 0, maxProm);
        CHECK(moves == 0);
        CHECK(!an.isEngaged());
        CHECK(an.frequencyHz() == parked);  // released, not forgotten
    }

    // ---------------------------------------------------------------------
    // AutoNotch: disabled is bit-exact, chunked == one-shot, reset == fresh.
    // ---------------------------------------------------------------------
    {
        Lcg rng(0x777u);
        std::vector<float> in(4096);
        for (std::size_t i = 0; i < in.size(); ++i) {
            const double t = static_cast<double>(i) / kFs;
            in[i] = static_cast<float>(0.8 * std::cos(kTwoPi * 3000.0 * t) +
                                       0.02 * static_cast<double>(rng.next()));
        }

        auto off = in;
        AutoNotch a(kFs, 512, 30.0);
        a.setEnabled(false);
        a.process(off.data(), off.size());
        std::size_t bad = 0;
        for (std::size_t i = 0; i < in.size(); ++i) {
            if (off[i] != in[i]) { ++bad; }
        }
        CHECK(bad == 0);

        auto oneShot = in;
        AutoNotch b(kFs, 512, 30.0);
        b.process(oneShot.data(), oneShot.size());

        auto chunked = in;
        AutoNotch c(kFs, 512, 30.0);
        const std::size_t sizes[] = {1, 3, 7, 13, 31, 193, 511};
        std::size_t i = 0;
        std::size_t si = 0;
        while (i < chunked.size()) {
            std::size_t take = sizes[si % 7];
            ++si;
            if (i + take > chunked.size()) { take = chunked.size() - i; }
            c.process(chunked.data() + i, take);
            i += take;
        }
        bad = 0;
        for (std::size_t k = 0; k < in.size(); ++k) {
            if (chunked[k] != oneShot[k]) { ++bad; }
        }
        CHECK(bad == 0);
        CHECK(c.isEngaged() == b.isEngaged());
        CHECK(c.frequencyHz() == b.frequencyHz());

        AutoNotch d(kFs, 512, 30.0);
        auto scratch = in;
        d.process(scratch.data(), scratch.size());
        d.reset();
        CHECK(!d.isEngaged());
        CHECK(d.frequencyHz() == 0.0);
        auto afterReset = in;
        d.process(afterReset.data(), afterReset.size());
        bad = 0;
        for (std::size_t k = 0; k < in.size(); ++k) {
            if (afterReset[k] != oneShot[k]) { ++bad; }
        }
        CHECK(bad == 0);
    }

    return testSummary("test_notch");
}
