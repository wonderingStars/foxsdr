// Tests for the synchronous AM demodulator (dsp/sam.hpp).
//
// Every expectation is physical (a tone's distortion, an interferer's level, a time in seconds), measured on
// signals this file builds from the definition of AM: x(t) = (A(t) + m cos(2 pi fm t)) exp(j 2 pi f0 t) + noise.
// The reference "envelope detector on the same input" is the existing dsp::Demodulator in AM mode.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "dsp/sam.hpp"

#include "alloc_probe.hpp"
#include "dsp/demod.hpp"
#include "signal_block_gen.hpp"
#include "test_check.hpp"

#include <cmath>
#include <complex>
#include <cstdio>
#include <functional>
#include <random>
#include <vector>

namespace {

using cascade::dsp::DemodMode;
using cascade::dsp::Demodulator;
using cascade::dsp::SyncAm;
using cf = std::complex<float>;
constexpr double kPi2 = 6.283185307179586;

struct AmSpec {
    double fs = 48000.0;
    double seconds = 3.0;
    double fm = 1000.0;       // modulation tone
    double depth = 0.5;       // relative to the UNFADED carrier amplitude 1
    double offsetHz = 0.0;    // carrier offset from the channel centre
    double noise = 0.001;     // complex noise, sigma per component
    std::function<double(double)> carrier = [](double) { return 1.0; };   // A(t)
    double interfHz = 0.0;    // an extra tone at carrier + interfHz (negative = lower sideband)
    double interfAmp = 0.0;
    std::uint32_t seed = 7;
};

std::vector<cf> makeAm(const AmSpec& s) {
    const std::size_t n = static_cast<std::size_t>(s.seconds * s.fs);
    std::mt19937 g(s.seed);
    std::normal_distribution<double> nd(0.0, s.noise);
    std::vector<cf> x(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / s.fs;
        const double env = s.carrier(t) + s.depth * std::cos(kPi2 * s.fm * t);
        const double ph = kPi2 * s.offsetHz * t;
        std::complex<double> v = env * std::polar(1.0, ph);
        if (s.interfAmp != 0.0) { v += s.interfAmp * std::polar(1.0, kPi2 * (s.offsetHz + s.interfHz) * t); }
        x[i] = cf(static_cast<float>(v.real() + nd(g)), static_cast<float>(v.imag() + nd(g)));
    }
    return x;
}

// Amplitude of a tone at f0 (Hz) in x[from, to): single-bin DFT, the window should hold whole cycles.
double toneAmp(const std::vector<float>& x, double fs, double f0, std::size_t from, std::size_t to) {
    double a = 0, b = 0;
    for (std::size_t i = from; i < to; ++i) {
        const double ph = kPi2 * f0 * static_cast<double>(i) / fs;
        a += x[i] * std::cos(ph);
        b += x[i] * std::sin(ph);
    }
    const double n = static_cast<double>(to - from);
    return 2.0 * std::sqrt(a * a + b * b) / n;
}

// Distortion plus noise relative to the fundamental at f0 (mean removed): sqrt((P_total - P_f0) / P_f0).
double thdn(const std::vector<float>& x, double fs, double f0, std::size_t from, std::size_t to) {
    double mean = 0;
    for (std::size_t i = from; i < to; ++i) { mean += x[i]; }
    mean /= static_cast<double>(to - from);
    double p = 0;
    for (std::size_t i = from; i < to; ++i) { p += (x[i] - mean) * (x[i] - mean); }
    p /= static_cast<double>(to - from);
    const double a = toneAmp(x, fs, f0, from, to);
    const double pf = a * a / 2.0;
    return std::sqrt(std::max(p - pf, 0.0) / std::max(pf, 1e-30));
}

std::vector<float> runSam(SyncAm& d, const std::vector<cf>& x, std::size_t chunk = 4096) {
    std::vector<float> y(x.size());
    for (std::size_t i = 0; i < x.size(); i += chunk) {
        const std::size_t n = std::min(chunk, x.size() - i);
        d.process(x.data() + i, n, y.data() + i);
    }
    return y;
}

std::vector<float> runEnv(double fs, const std::vector<cf>& x) {
    Demodulator d(fs);
    d.setMode(DemodMode::AM);
    std::vector<float> y(x.size());
    d.process(x.data(), x.size(), y.data());
    return y;
}

std::size_t at(double fs, double s) { return static_cast<std::size_t>(s * fs); }

// ---- 1. clean carrier: linear, same level as the envelope detector, locks ------------------------------------------

void testCleanCarrier() {
    AmSpec s;
    const auto x = makeAm(s);
    SyncAm d(s.fs);
    const auto y = runSam(d, x);
    const auto e = runEnv(s.fs, x);
    const double aS = toneAmp(y, s.fs, 1000.0, at(s.fs, 2.0), at(s.fs, 2.5));
    const double aE = toneAmp(e, s.fs, 1000.0, at(s.fs, 2.0), at(s.fs, 2.5));
    const double tS = thdn(y, s.fs, 1000.0, at(s.fs, 2.0), at(s.fs, 2.5));
    const double tE = thdn(e, s.fs, 1000.0, at(s.fs, 2.0), at(s.fs, 2.5));
    std::printf("clean: SAM amp %.4f THD+N %.4f%%, envelope amp %.4f THD+N %.4f%%, latency %zu samples, taps %zu\n", aS,
                tS * 100, aE, tE * 100, d.latencySamples(), d.taps());
    CHECK_NEAR(aS, 0.5, 0.01);            // the tone is the modulation (depth 0.5) at the envelope detector's level
    CHECK_NEAR(aS / aE, 1.0, 0.02);
    CHECK(tS < 0.005);                    // < 0.5 % distortion + noise (noise floor sigma 0.001)
    CHECK(d.locked() && d.state() == SyncAm::State::Locked);
    CHECK(d.carrierPresence() > 0.8f);
    CHECK_NEAR(d.carrierOffsetHz(), 0.0, 1.0);
}

// ---- 2. carrier offset acquisition ---------------------------------------------------------------------------------

double lockTimeSeconds(double offsetHz, double fm, std::uint32_t seed, double* offsetErrHz, double* thdAfter) {
    AmSpec s;
    s.offsetHz = offsetHz;
    s.fm = fm;
    s.seed = seed;
    s.seconds = 3.0;
    const auto x = makeAm(s);
    SyncAm d(s.fs);
    std::vector<float> y(x.size());
    double lockAt = -1.0;
    const std::size_t chunk = 48;
    for (std::size_t i = 0; i + chunk <= x.size(); i += chunk) {
        d.process(x.data() + i, chunk, y.data() + i);
        if (lockAt < 0.0 && d.locked()) { lockAt = static_cast<double>(i + chunk) / s.fs; }
    }
    if (offsetErrHz != nullptr) { *offsetErrHz = d.carrierOffsetHz() - offsetHz; }
    if (thdAfter != nullptr) { *thdAfter = thdn(y, s.fs, fm, at(s.fs, 2.0), at(s.fs, 2.5)); }
    return lockAt;
}

void testOffsetAcquisition() {
    double worst = 0.0;
    for (double off : {-200.0, -150.0, -100.0, -37.0, 0.0, 25.0, 100.0, 150.0, 200.0}) {
        double err = 0, th = 0;
        const double t = lockTimeSeconds(off, 1000.0, 11, &err, &th);
        std::printf("offset %+6.1f Hz: lock at %.3f s, residual offset error %+.3f Hz, THD+N after %.3f%%\n", off, t, err, th * 100);
        CHECK(t > 0.0);
        worst = std::max(worst, t);
        CHECK(t < 1.0);
        CHECK(std::fabs(err) < 1.0);
        CHECK(th < 0.01);
    }
    std::printf("worst lock time over the offsets: %.3f s\n", worst);
    // a speech-like modulation (energy from 100 Hz up) instead of a tone: the lock still comes, at +-200 Hz too
    for (double off : {-200.0, 200.0}) {
        AmSpec s;
        s.offsetHz = off;
        std::vector<cf> x = makeAm(s);
        // replace the tone by speech: rebuild with the speech-like audio as modulation, depth 0.6 of peak
        const auto sp = sbgen::speechLike(x.size(), s.fs, 1.0, 5);
        std::mt19937 g(3);
        std::normal_distribution<double> nd(0.0, 0.001);
        for (std::size_t i = 0; i < x.size(); ++i) {
            const double t = static_cast<double>(i) / s.fs;
            const double env = 1.0 + 0.6 * sp[i];
            const std::complex<double> v = env * std::polar(1.0, kPi2 * off * t);
            x[i] = cf(static_cast<float>(v.real() + nd(g)), static_cast<float>(v.imag() + nd(g)));
        }
        SyncAm d(s.fs);
        double lockAt = -1.0;
        std::vector<float> y(x.size());
        for (std::size_t i = 0; i + 48 <= x.size(); i += 48) {
            d.process(x.data() + i, 48, y.data() + i);
            if (lockAt < 0.0 && d.locked()) { lockAt = static_cast<double>(i + 48) / s.fs; }
        }
        std::printf("speech-like, offset %+.0f Hz: lock at %.3f s, final offset error %+.2f Hz\n", off, lockAt, d.carrierOffsetHz() - off);
        CHECK(lockAt > 0.0 && lockAt < 1.5);
        CHECK(std::fabs(d.carrierOffsetHz() - off) < 2.0);
    }
}

// ---- 3. selective fade: SAM against the envelope detector on the same input --------------------------------------------

void testSelectiveFade() {
    for (double fadeDb : {20.0, 26.0, 30.0}) {
        AmSpec s;
        s.seconds = 4.0;
        const double a = std::pow(10.0, -fadeDb / 20.0);
        s.carrier = [a](double t) {
            // carrier down by fadeDb between 2.0 s and 3.0 s, 5 ms edges
            const double up = std::clamp((t - 2.0) / 0.005, 0.0, 1.0);
            const double dn = std::clamp((t - 3.0) / 0.005, 0.0, 1.0);
            return 1.0 - (1.0 - a) * (up - dn);
        };
        const auto x = makeAm(s);
        SyncAm d(s.fs);
        std::vector<float> y(x.size());
        std::vector<int> stateAt;
        for (std::size_t i = 0; i + 480 <= x.size(); i += 480) {
            d.process(x.data() + i, 480, y.data() + i);
            stateAt.push_back(static_cast<int>(d.state()));
        }
        const auto e = runEnv(s.fs, x);
        // measure late in the fade, after the 2 Hz DC blocker (80 ms) has settled: 2.6 .. 2.95 s (350 whole cycles at 1 kHz)
        const double tS = thdn(y, s.fs, 1000.0, at(s.fs, 2.6), at(s.fs, 2.95));
        const double tE = thdn(e, s.fs, 1000.0, at(s.fs, 2.6), at(s.fs, 2.95));
        const double aS = toneAmp(y, s.fs, 1000.0, at(s.fs, 2.6), at(s.fs, 2.95));
        const int stateMid = stateAt[static_cast<std::size_t>(2.5 * 100)];     // 100 chunks per second
        const int stateEnd = stateAt[static_cast<std::size_t>(3.9 * 100)];
        // peak excursion of the audio in the fade onset and the recovery, against the normal tone peak (0.5)
        double peakOn = 0, peakOff = 0;
        for (std::size_t i = at(s.fs, 1.95); i < at(s.fs, 2.3); ++i) { peakOn = std::max<double>(peakOn, std::fabs(y[i])); }
        for (std::size_t i = at(s.fs, 3.0); i < at(s.fs, 3.5); ++i) { peakOff = std::max<double>(peakOff, std::fabs(y[i])); }
        std::printf("fade %2.0f dB: SAM THD+N %.3f%% (amp %.3f), envelope THD+N %.1f%%, state mid-fade %d, at end %d, peaks %.2f / %.2f\n", fadeDb,
                    tS * 100, aS, tE * 100, stateMid, stateEnd, peakOn, peakOff);
        CHECK(tE > 0.3);                    // the envelope detector is badly distorted (>30 %): the case is real
        CHECK(tS < 0.02);                   // SAM stays under 2 %
        CHECK(tS < tE / 20.0);              // pinned margin: at least 20 times less distortion than the envelope detector
        CHECK_NEAR(aS, 0.5, 0.03);
        // at 20 dB the carrier (0.1) is still 27 % of the channel rms and the loop stays locked; deeper fades are held
        CHECK(stateMid == static_cast<int>(fadeDb >= 26.0 ? SyncAm::State::Held : SyncAm::State::Locked));
        CHECK(stateEnd == static_cast<int>(SyncAm::State::Locked));   // re-acquired after the carrier came back
        CHECK(peakOn < 1.6 && peakOff < 1.6);   // no burst: the audio never exceeds ~3x the tone peak around the fade edges
    }
}

// ---- 4. sideband selection ---------------------------------------------------------------------------------------------

void testSidebandSelection() {
    const double fs = 48000.0;
    for (int lowerInterferer = 0; lowerInterferer < 2; ++lowerInterferer) {
        AmSpec s;
        s.seconds = 3.0;
        s.interfHz = lowerInterferer ? -1500.0 : 1500.0;     // an unmodulated carrier 1.5 kHz off, on the unwanted side
        s.interfAmp = 0.2;
        const auto x = makeAm(s);
        SyncAm both(fs), keep(fs);
        both.setSideband(SyncAm::Sideband::Both);
        keep.setSideband(lowerInterferer ? SyncAm::Sideband::Upper : SyncAm::Sideband::Lower);
        const auto yb = runSam(both, x);
        const auto yk = runSam(keep, x);
        const double iB = toneAmp(yb, fs, 1500.0, at(fs, 2.0), at(fs, 2.5));
        const double iK = toneAmp(yk, fs, 1500.0, at(fs, 2.0), at(fs, 2.5));
        const double wB = toneAmp(yb, fs, 1000.0, at(fs, 2.0), at(fs, 2.5));
        const double wK = toneAmp(yk, fs, 1000.0, at(fs, 2.0), at(fs, 2.5));
        const double rejDb = 20.0 * std::log10(iB / std::max(iK, 1e-12));
        std::printf("interferer on the %s sideband: Both %.4f, single-sideband %.6f -> removed by %.1f dB; wanted tone %.3f -> %.3f; locked %d/%d\n",
                    lowerInterferer ? "lower" : "upper", iB, iK, rejDb, wB, wK, both.locked(), keep.locked());
        CHECK(both.locked() && keep.locked());       // a 0.2 interferer does not unlock the loop
        CHECK_NEAR(iB, 0.2, 0.02);                  // in Both mode the interferer is fully audible (beats with the carrier)
        CHECK(rejDb > 40.0);                        // pinned: >= 40 dB removed (measured 45; the PLL's own phase wobble from the interferer sets the floor)
        CHECK_NEAR(wK, 0.5, 0.02);                  // the wanted tone (in both sidebands) is intact at the same level
        CHECK_NEAR(wB, 0.5, 0.02);
    }
    // the near-carrier limit: an interferer 150 Hz off on the unwanted side is only partly rejected (documented)
    AmSpec s;
    s.interfHz = -150.0;
    s.interfAmp = 0.2;
    const auto x = makeAm(s);
    SyncAm keep(fs);
    keep.setSideband(SyncAm::Sideband::Upper);
    const auto yk = runSam(keep, x);
    const double i150 = toneAmp(yk, fs, 150.0, at(fs, 2.0), at(fs, 2.5));
    std::printf("interferer 150 Hz below the carrier, Upper: %.4f of 0.2 (%.1f dB down)\n", i150, 20 * std::log10(0.2 / std::max(i150, 1e-12)));
    CHECK(i150 < 0.2);
}

// ---- 5. lock indicator ------------------------------------------------------------------------------------------------

void testLockIndicator() {
    const double fs = 48000.0;
    // false: noise only, never locks
    {
        const auto nz = sbgen::whiteNoise(static_cast<std::size_t>(3 * fs) * 2, 0.3, 5);
        std::vector<cf> x(nz.size() / 2);
        for (std::size_t i = 0; i < x.size(); ++i) { x[i] = cf(nz[2 * i], nz[2 * i + 1]); }
        SyncAm d(fs);
        std::vector<float> y(x.size());
        bool everLocked = false;
        for (std::size_t i = 0; i + 480 <= x.size(); i += 480) {
            d.process(x.data() + i, 480, y.data() + i);
            everLocked = everLocked || d.locked();
        }
        std::printf("noise only: ever locked %d, presence %.3f, peak audio %.2f\n", everLocked, d.carrierPresence(),
                    *std::max_element(y.begin(), y.end()));
        CHECK(!everLocked);
        CHECK(d.carrierPresence() < 0.2f);
    }
    // true then false then true: carrier gone for 5 s (longer than the 2 s hold), then back
    {
        AmSpec s;
        s.seconds = 10.0;
        s.carrier = [](double t) { return (t >= 2.0 && t < 7.0) ? 0.0 : 1.0; };
        s.depth = 0.0;      // no sidebands either: the whole signal vanishes
        s.noise = 0.01;
        const auto x = makeAm(s);
        SyncAm d(fs);
        std::vector<float> y(x.size());
        std::vector<int> st;
        for (std::size_t i = 0; i + 480 <= x.size(); i += 480) {
            d.process(x.data() + i, 480, y.data() + i);
            st.push_back(static_cast<int>(d.state()));
        }
        const auto S = [&](double t) { return st[static_cast<std::size_t>(t * 100)]; };
        std::printf("loss test: states at 1.5 s %d, 2.5 s %d, 4.5 s %d, 6.5 s %d, 9.5 s %d\n", S(1.5), S(2.5), S(4.5), S(6.5), S(9.5));
        CHECK(S(1.5) == static_cast<int>(SyncAm::State::Locked));
        CHECK(S(2.5) == static_cast<int>(SyncAm::State::Held));            // within the 2 s hold
        CHECK(S(4.5) == static_cast<int>(SyncAm::State::Searching));       // hold expired
        CHECK(S(6.5) == static_cast<int>(SyncAm::State::Searching));
        CHECK(S(9.5) == static_cast<int>(SyncAm::State::Locked));          // the carrier returned and was re-acquired
        // no noise burst while it is lost: the output after the hold stays small (the channel holds noise of 0.01)
        double pk = 0;
        for (std::size_t i = at(fs, 3.0); i < at(fs, 6.9); ++i) { pk = std::max<double>(pk, std::fabs(y[i])); }
        std::printf("loss test: peak audio 3.0..6.9 s with only noise on the channel: %.4f\n", pk);
        CHECK(pk < 0.1);
    }
}

// ---- 6. robustness and interfaces ------------------------------------------------------------------------------------

void testRobustness() {
    const double fs = 48000.0;
    AmSpec s;
    s.seconds = 1.0;
    s.offsetHz = 60.0;
    const auto x = makeAm(s);
    // block-split invariance: bit-identical output for any chunking
    SyncAm a(fs), b(fs);
    std::vector<float> ya(x.size()), yb(x.size());
    a.process(x.data(), x.size(), ya.data());
    for (std::size_t i = 0; i < x.size();) {
        const std::size_t n = std::min<std::size_t>(1 + (i * 7919) % 997, x.size() - i);
        b.process(x.data() + i, n, yb.data() + i);
        i += n;
    }
    CHECK(ya == yb);
    // non-finite input reads as zero and does not poison the state
    std::vector<cf> bad = x;
    bad[1000] = cf(std::nanf(""), 0.0f);
    bad[1001] = cf(0.0f, INFINITY);
    SyncAm c(fs);
    std::vector<float> yc(bad.size());
    c.process(bad.data(), bad.size(), yc.data());
    bool finite = true;
    for (float v : yc) { finite = finite && std::isfinite(v); }
    CHECK(finite);
    CHECK(c.locked());
    // other rates
    for (double r : {24000.0, 96000.0}) {
        AmSpec q;
        q.fs = r;
        q.seconds = 3.0;
        q.offsetHz = -120.0;
        const auto xq = makeAm(q);
        SyncAm d(r);
        const auto yq = runSam(d, xq);
        const double t = thdn(yq, r, 1000.0, at(r, 2.0), at(r, 2.5));
        std::printf("rate %.0f: taps %zu latency %zu locked %d THD+N %.3f%%\n", r, d.taps(), d.latencySamples(), d.locked(), t * 100);
        CHECK(d.locked());
        CHECK(t < 0.01);
    }
    // settings: clamped, non-finite ignored
    SyncAm e(fs);
    e.setLoopBandwidthHz(1000.0);
    CHECK_NEAR(e.loopBandwidthHz(), 100.0, 0);
    e.setLoopBandwidthHz(std::nan(""));
    CHECK_NEAR(e.loopBandwidthHz(), 100.0, 0);
    e.setHoldSeconds(-1.0);
    CHECK_NEAR(e.holdSeconds(), 0.0, 0);
    // reset returns to Searching
    SyncAm f(fs);
    std::vector<float> yf(x.size());
    f.process(x.data(), x.size(), yf.data());
    CHECK(f.locked());
    f.reset();
    CHECK(f.state() == SyncAm::State::Searching);
    // no allocation in process()
    {
        std::vector<float> yo(4096);
        AllocProbe probe;
        for (int k = 0; k < 10; ++k) {
            f.setSideband(k % 3 == 0 ? SyncAm::Sideband::Upper : SyncAm::Sideband::Both);
            f.process(x.data(), 4096, yo.data());
        }
        CHECK(probe.news() == 0);
    }
}

// ---- 7. switching the sideband while running: no click above a stated threshold -----------------------------------------

void testSidebandSwitchNoClick() {
    const double fs = 48000.0;
    AmSpec s;
    s.seconds = 3.0;
    const auto x = makeAm(s);
    SyncAm d(fs);
    std::vector<float> y(x.size());
    std::size_t i = 0;
    const std::size_t chunk = 480;
    int k = 0;
    for (; i + chunk <= x.size(); i += chunk, ++k) {
        if (i >= at(fs, 1.0) && i % at(fs, 0.25) == 0) {
            d.setSideband(static_cast<SyncAm::Sideband>((i / at(fs, 0.25)) % 3));
        }
        d.process(x.data() + i, chunk, y.data() + i);
    }
    // a click shows as a sample-to-sample step far beyond what the 1 kHz tone (amplitude 0.5) makes: max step of a clean tone is
    // 0.5 * 2 pi 1000 / 48000 = 0.065
    double maxStep = 0, cleanStep = 0;
    for (std::size_t j = at(fs, 1.0); j + 1 < at(fs, 3.0); ++j) { maxStep = std::max<double>(maxStep, std::fabs(y[j + 1] - y[j])); }
    for (std::size_t j = at(fs, 0.8); j + 1 < at(fs, 0.95); ++j) { cleanStep = std::max<double>(cleanStep, std::fabs(y[j + 1] - y[j])); }
    std::printf("sideband switching every 250 ms: largest sample step %.4f, a clean tone's %.4f\n", maxStep, cleanStep);
    CHECK(maxStep < 1.5 * cleanStep);
}


// A harsher switch: a 0.5 interferer on the lower sideband is removed when Upper is selected mid-stream. Without the 10 ms crossfade the
// output would jump by up to the interferer's instantaneous value (up to 0.5); with it the largest sample step stays within the tone's.
void testSidebandSwitchWithInterferer() {
    const double fs = 48000.0;
    AmSpec s;
    s.seconds = 2.0;
    s.interfHz = -800.0;
    s.interfAmp = 0.5;
    const auto x = makeAm(s);
    SyncAm d(fs);
    std::vector<float> y(x.size());
    for (std::size_t i = 0; i + 480 <= x.size(); i += 480) {
        if (i == at(fs, 1.0)) { d.setSideband(SyncAm::Sideband::Upper); }
        d.process(x.data() + i, 480, y.data() + i);
    }
    double maxStep = 0;
    for (std::size_t j = at(fs, 0.99); j + 1 < at(fs, 1.1); ++j) { maxStep = std::max<double>(maxStep, std::fabs(y[j + 1] - y[j])); }
    // the unswitched stream holds two tones (0.5 at 1 kHz, 0.5 at 800 Hz): its largest step
    double base = 0;
    for (std::size_t j = at(fs, 0.8); j + 1 < at(fs, 0.95); ++j) { base = std::max<double>(base, std::fabs(y[j + 1] - y[j])); }
    std::printf("Both -> Upper with a 0.5 interferer: largest step across the switch %.4f, before it %.4f
", maxStep, base);
    CHECK(maxStep < 1.2 * base);
}

}  // namespace

int main() {
    testCleanCarrier();
    testOffsetAcquisition();
    testSelectiveFade();
    testSidebandSelection();
    testLockIndicator();
    testRobustness();
    testSidebandSwitchNoClick();
    testSidebandSwitchWithInterferer();
    return testSummary("test_sam");
}
