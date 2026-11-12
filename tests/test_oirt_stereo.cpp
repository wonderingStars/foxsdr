// Tests for dsp/oirt_stereo.hpp, the OIRT polar-modulation stereo decoder.
//
// Every multiplex is synthesised here from Rec. ITU-R BS.450-3 section 2.1.2:
// M = 0.8 (A+B)/2 and S = 0.8 (A-B)/2 pre-emphasised (50 us), S amplitude-
// modulating an unsuppressed 31.25 kHz carrier of amplitude 1 whose spectrum is
// shaped by K(f) = (1 + j6.4f)/(5 + j6.4f) (f in kHz), so the carrier ends at
// 20 %. K is applied ANALYTICALLY per tone (magnitude and phase from the
// formula), not with the decoder's own bilinear filter, so the decoder and the
// generator do not share an implementation of K. Measurements use a direct DFT
// on integer bins; nothing is read back from the implementation.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "dsp/oirt_stereo.hpp"

#include "test_check.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <chrono>
#include <vector>

namespace {

using cascade::dsp::OirtStereo;
constexpr double kTwoPi = 6.283185307179586476925286766559;
constexpr double kFs = 200000.0;
constexpr int kDec = 4;
constexpr double kOutFs = kFs / kDec;
constexpr std::size_t kWarm = 100000;  // 0.5 s of composite
constexpr std::size_t kMeas = 20000;   // 0.4 s of output at 50 kHz -> 2.5 Hz bins

struct Tone { double hz; double amp; };

std::complex<double> kOf(double hz) {
    const std::complex<double> jw(0.0, 6.4 * hz / 1000.0);
    return (1.0 + jw) / (5.0 + jw);
}

// Pre-emphasis 1 + j w tau, tau = 50 us.
std::complex<double> preOf(double hz) { return {1.0, kTwoPi * hz * 50e-6}; }

struct Gen {
    std::vector<Tone> a, b;       // left (A) and right (B) programme
    double carrierHz = 31250.0;
    double carrierPhase = 0.7;
    bool subcarrier = true;       // false: a mono transmission
    double monoNoise = 0.0;       // adds low-passed noise to M when > 0
    std::uint32_t seed = 1;
    double lp = 0.0;

    double next(std::size_t n) {
        const double t = static_cast<double>(n) / kFs;
        double m = 0.0, env = 0.2;
        auto add = [&](const std::vector<Tone>& v, double sgn) {
            for (const Tone& x : v) {
                const std::complex<double> p = preOf(x.hz);
                const double ph = std::arg(p);
                const double g = std::abs(p) * x.amp * 0.8 * 0.5;
                m += g * std::cos(kTwoPi * x.hz * t + ph);
                const std::complex<double> k = kOf(x.hz);
                env += sgn * g * std::abs(k) * std::cos(kTwoPi * x.hz * t + ph + std::arg(k));
            }
        };
        add(a, +1.0);
        add(b, -1.0);
        if (monoNoise > 0.0) {
            seed = seed * 1664525u + 1013904223u;
            const double w = (static_cast<double>(seed >> 8) / 8388608.0) - 1.0;
            lp += 0.35 * (w - lp);
            m += monoNoise * lp;
        }
        if (!subcarrier) { return m; }
        return m + env * std::cos(kTwoPi * carrierHz * t + carrierPhase);
    }
};

std::vector<float> render(Gen& g, std::size_t from, std::size_t n) {
    std::vector<float> v(n);
    for (std::size_t i = 0; i < n; ++i) { v[i] = static_cast<float>(g.next(from + i)); }
    return v;
}

double amp(const std::vector<float>& y, std::size_t start, double hz) {
    std::complex<double> acc(0, 0);
    for (std::size_t i = 0; i < kMeas; ++i) {
        const double ph = -kTwoPi * hz * static_cast<double>(i) / kOutFs;
        acc += static_cast<double>(y[start + i]) * std::complex<double>(std::cos(ph), std::sin(ph));
    }
    return 2.0 * std::abs(acc) / static_cast<double>(kMeas);
}

struct Result { std::vector<float> l, r; bool lamp = false; };

Result run(OirtStereo& dec, Gen& g, std::size_t total) {
    const std::vector<float> in = render(g, 0, total);
    Result res;
    res.l.resize(dec.outputsFor(total));
    res.r.resize(dec.outputsFor(total));
    const std::size_t n = dec.process(in.data(), in.size(), res.l.data(), res.r.data());
    res.l.resize(n);
    res.r.resize(n);
    res.lamp = dec.stereoLamp();
    return res;
}

double db(double x) { return 20.0 * std::log10(x); }

// Separation in dB for a tone present on one channel only.
double separation(double hz, bool leftOnly, bool restore = true, double carrierHz = 31250.0) {
    OirtStereo dec(kFs, kDec);
    dec.setRestoreLevelForTest(restore);
    Gen g;
    g.carrierHz = carrierHz;
    (leftOnly ? g.a : g.b).push_back({hz, 0.4});
    const Result r = run(dec, g, kWarm + kMeas * kDec);
    const std::size_t s = r.l.size() - kMeas;
    const double wanted = amp(leftOnly ? r.l : r.r, s, hz);
    const double leak = amp(leftOnly ? r.r : r.l, s, hz);
    return db(wanted / leak);
}

}  // namespace

int main() {
    // Left only: expected output level (programme x pre-emphasis x de-emphasis).
    {
        OirtStereo dec(kFs, kDec);
        Gen g;
        g.a.push_back({1000.0, 0.4});
        const Result r = run(dec, g, kWarm + kMeas * kDec);
        const std::size_t s = r.l.size() - kMeas;
        CHECK(r.lamp);
        CHECK(dec.carrierLevel() > 0.8f && dec.carrierLevel() < 1.2f);
        const double l = amp(r.l, s, 1000.0);
        const double expect = 0.4 * std::abs(preOf(1000.0)) / std::sqrt(1.0 + std::pow(kTwoPi * 1000 * 50e-6, 2));
        std::printf("oirt left-only 1 kHz: level %.4f expect %.4f (%.2f dB)\n", l, expect, db(l / expect));
        CHECK(std::fabs(db(l / expect)) < 0.5);
    }
    const double sepL1k = separation(1000.0, true);
    const double sepR1k = separation(1000.0, false);
    const double sepL100 = separation(100.0, true);
    const double sepL5k = separation(5000.0, true);
    const double sepL10k = separation(10000.0, true);
    const double sepOff = separation(1000.0, true, true, 31250.0 + 10.0);
    std::printf("oirt separation dB: L1k %.1f R1k %.1f L100 %.1f L5k %.1f L10k %.1f L1k(+10 Hz carrier) %.1f\n",
                sepL1k, sepR1k, sepL100, sepL5k, sepL10k, sepOff);
    CHECK(sepL1k > 55.0);
    CHECK(sepR1k > 55.0);
    CHECK(sepL100 > 35.0);  // limited by the 2 Hz offset high-pass
    CHECK(sepL5k > 55.0);
    CHECK(sepL10k > 55.0);
    CHECK(sepOff > 55.0);

    // Both channels at once with different tones: each lands in its own channel.
    {
        OirtStereo dec(kFs, kDec);
        Gen g;
        g.a.push_back({1000.0, 0.3});
        g.b.push_back({2500.0, 0.3});
        const Result r = run(dec, g, kWarm + kMeas * kDec);
        const std::size_t s = r.l.size() - kMeas;
        CHECK(db(amp(r.l, s, 1000.0) / amp(r.r, s, 1000.0)) > 40.0);
        CHECK(db(amp(r.r, s, 2500.0) / amp(r.l, s, 2500.0)) > 40.0);
    }

    // Break-it: without the sub-carrier level restoration the low-frequency
    // separation collapses (K = 1/5 near DC), so this MUST be far below the
    // restored figure; if it were not, the restoration proved nothing.
    {
        const double broken = separation(100.0, true, false);
        std::printf("oirt separation 100 Hz WITHOUT restoration: %.1f dB\n", broken);
        CHECK(broken < sepL100 - 15.0);
    }

    // Mono transmission (no sub-carrier): loud programme plus noise, stereo lamp
    // must never light and L == R.
    {
        OirtStereo dec(kFs, kDec);
        Gen g;
        g.subcarrier = false;
        g.a.push_back({1000.0, 0.9});
        g.a.push_back({7300.0, 0.5});
        g.b = g.a;
        g.monoNoise = 0.3;
        const std::size_t total = kWarm * 4;
        bool everOn = false;
        std::vector<float> l(dec.outputsFor(4096)), r(dec.outputsFor(4096));
        double maxDiff = 0.0, peak = 0.0;
        for (std::size_t pos = 0; pos < total; pos += 4096) {
            const std::vector<float> in = render(g, pos, 4096);
            const std::size_t n = dec.process(in.data(), in.size(), l.data(), r.data());
            everOn = everOn || dec.stereoLamp();
            for (std::size_t i = 0; i < n; ++i) {
                maxDiff = std::fmax(maxDiff, std::fabs(l[i] - r[i]));
                peak = std::fmax(peak, std::fabs(l[i]));
            }
        }
        CHECK(!everOn);
        CHECK(maxDiff == 0.0);
        CHECK(peak > 0.1);
        CHECK(dec.carrierLevel() < 0.2f);
    }

    // Stereo transmitter carrying mono content (A == B): lamp on, L == R closely.
    {
        OirtStereo dec(kFs, kDec);
        Gen g;
        g.a.push_back({1000.0, 0.4});
        g.b = g.a;
        const Result r = run(dec, g, kWarm + kMeas * kDec);
        CHECK(r.lamp);
        const std::size_t s = r.l.size() - kMeas;
        const double al = amp(r.l, s, 1000.0), ar = amp(r.r, s, 1000.0);
        CHECK(al > 0.3);
        CHECK(std::fabs(al - ar) < 0.01);
    }

    // Stereo then the sub-carrier disappears: the lamp goes out inside 150 ms.
    {
        OirtStereo dec(kFs, kDec);
        Gen g;
        g.a.push_back({1000.0, 0.4});
        const std::size_t first = kWarm;
        std::vector<float> l(dec.outputsFor(first)), r(dec.outputsFor(first));
        std::vector<float> in = render(g, 0, first);
        dec.process(in.data(), in.size(), l.data(), r.data());
        CHECK(dec.stereoLamp());
        g.subcarrier = false;
        in = render(g, first, static_cast<std::size_t>(0.15 * kFs));
        l.resize(dec.outputsFor(in.size()));
        r.resize(dec.outputsFor(in.size()));
        dec.process(in.data(), in.size(), l.data(), r.data());
        CHECK(!dec.stereoLamp());
    }

    // Block-split invariance: odd block sizes give bit-identical output.
    {
        Gen g;
        g.a.push_back({1000.0, 0.4});
        g.b.push_back({3100.0, 0.2});
        const std::size_t total = 30000;
        const std::vector<float> in = render(g, 0, total);
        OirtStereo one(kFs, kDec), two(kFs, kDec);
        std::vector<float> l1(one.outputsFor(total)), r1(l1.size());
        const std::size_t n1 = one.process(in.data(), total, l1.data(), r1.data());
        std::vector<float> l2, r2;
        std::vector<float> bl(one.outputsFor(997)), br(bl.size());
        for (std::size_t pos = 0; pos < total; pos += 997) {
            const std::size_t c = std::min<std::size_t>(997, total - pos);
            const std::size_t n = two.process(in.data() + pos, c, bl.data(), br.data());
            l2.insert(l2.end(), bl.begin(), bl.begin() + static_cast<std::ptrdiff_t>(n));
            r2.insert(r2.end(), br.begin(), br.begin() + static_cast<std::ptrdiff_t>(n));
        }
        CHECK(n1 == l2.size());
        bool same = n1 == l2.size();
        for (std::size_t i = 0; same && i < n1; ++i) { same = l1[i] == l2[i] && r1[i] == r2[i]; }
        CHECK(same);
    }

    // CPU cost: 10 s of composite at 200 kS/s, printed for the record.
    {
        OirtStereo dec(kFs, kDec);
        Gen g;
        g.a.push_back({1000.0, 0.4});
        const std::vector<float> in = render(g, 0, 200000);
        std::vector<float> l(dec.outputsFor(in.size())), r(l.size());
        const auto t0 = std::chrono::steady_clock::now();
        for (int k = 0; k < 10; ++k) { dec.process(in.data(), in.size(), l.data(), r.data()); }
        const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("oirt cpu: 10 s of 200 kS/s composite in %.3f s (%.1f %% of one core)\n", sec, sec * 10.0);
    }

    // Too-low composite rate: permanent mono decoder, no crash.
    {
        OirtStereo dec(76000.0, 1);
        CHECK(!dec.stereoCapable());
        std::vector<float> in(1000, 0.1f), l(dec.outputsFor(1000)), r(l.size());
        const std::size_t n = dec.process(in.data(), in.size(), l.data(), r.data());
        CHECK(n == 1000);
        CHECK(!dec.stereoLamp());
    }

    return testSummary("test_oirt_stereo");
}
