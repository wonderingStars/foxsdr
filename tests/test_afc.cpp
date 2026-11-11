// Automatic frequency control tracker (src/dsp/afc.hpp). The receiver is simulated: a generated carrier (AM, FM or keyed)
// plus complex Gaussian noise is mixed down by the correction the tracker asked for (the retune the wiring would make), in
// 256-sample chunks, so the loop is closed exactly as it will be. Nothing here is a recording.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <complex>
#include <cstdio>
#include <functional>
#include <limits>
#include <random>

#include "alloc_probe.hpp"
#include "blk4_util.hpp"
#include "dsp/afc.hpp"
#include "test_check.hpp"

using cascade::dsp::AfcMode;
using cascade::dsp::AfcTracker;

namespace {
constexpr double kFs = 48000.0;
constexpr double kTwoPi = 6.283185307179586476925286766559;

// One simulated receiver + source.
struct Sim {
    AfcTracker& afc;
    std::mt19937 rng;
    std::normal_distribution<double> nd{0.0, 1.0};
    double sigma;                            // noise standard deviation per component
    double t = 0.0, psi = 0.0, phi = 0.0;    // time, source phase, mixer phase
    std::function<double(double)> carrierHz;  // instantaneous carrier frequency (relative to the ORIGINAL tuning), Hz
    std::function<double(double)> amp;        // envelope
    Sim(AfcTracker& a, double sig, std::uint32_t seed) : afc(a), rng(seed), sigma(sig) {
        carrierHz = [](double) { return 0.0; };
        amp = [](double) { return 0.3; };
    }
    // Runs `seconds`; calls `each(t, corr)` after every 256-sample chunk.
    void run(double seconds, const std::function<void(double, double)>& each = nullptr) {
        const std::size_t total = static_cast<std::size_t>(seconds * kFs);
        std::vector<std::complex<float>> chunk(256);
        for (std::size_t done = 0; done < total; done += 256) {
            const double corr = afc.correctionHz();
            for (auto& s : chunk) {
                psi += kTwoPi * carrierHz(t) / kFs;
                phi += kTwoPi * corr / kFs;
                const double a = amp(t);
                const std::complex<double> v = std::polar(a, psi - phi) + std::complex<double>(sigma * nd(rng), sigma * nd(rng));
                s = std::complex<float>(static_cast<float>(v.real()), static_cast<float>(v.imag()));
                t += 1.0 / kFs;
            }
            afc.process(chunk.data(), chunk.size());
            if (each) { each(t, afc.correctionHz()); }
        }
    }
};

double amAmp(double t) { return 0.3 * (1.0 + 0.3 * std::sin(kTwoPi * 1000.0 * t)); }

void setup(AfcTracker& a, AfcMode m) {
    a.setMode(m);
    a.setPullInHz(2500.0);
    a.setHoldHz(4000.0);
    a.setFreezeLevelDb(-30.0);  // carrier 0.3: -10 dB; noise sigma 0.003: -47 dB
    a.setEnabled(true);
}

void testPullIn() {
    // Sweep the carrier offset; find the largest that is acquired, both signs.
    double maxPos = 0, maxNeg = 0;
    for (int sign : {1, -1}) {
        for (double off = 0.0; off <= 4000.0; off += 250.0) {
            AfcTracker afc(kFs);
            setup(afc, AfcMode::MeanFrequency);
            Sim s(afc, 0.003, 7);
            const double f = sign * off;
            s.carrierHz = [f](double) { return f; };
            s.amp = amAmp;
            double lockT = -1;
            s.run(9.0, [&](double t, double) { if (lockT < 0 && afc.locked()) { lockT = t; } });
            const bool ok = afc.locked() && std::fabs(afc.correctionHz() - f) < 10.0;
            if (ok) { (sign > 0 ? maxPos : maxNeg) = off; }
            if (off == 1000.0 || off == 2500.0 || off == 2750.0) {
                std::printf("MEAS AM carrier %+.0f Hz off tune: %s, residual %.2f Hz, locked at %.2f s\n", f, ok ? "acquired" : "NOT acquired", afc.correctionHz() - f, lockT);
            }
        }
    }
    std::printf("MEAS pull-in range (pull-in setting 2500 Hz, AM, 250 Hz steps): acquired up to +%.0f and -%.0f Hz\n", maxPos, maxNeg);
    // The edge itself is noisy (the estimate wobbles by a few Hz around the limit): require 2250 acquired and 2750 not.
    CHECK(maxPos >= 2250.0 && maxPos <= 2500.0 && maxNeg >= 2250.0 && maxNeg <= 2500.0);
}

void testHold() {
    AfcTracker afc(kFs);
    setup(afc, AfcMode::MeanFrequency);
    Sim s(afc, 0.003, 8);
    s.carrierHz = [](double t) { return t * 150.0; };  // drifts 150 Hz/s, from 0
    s.amp = amAmp;
    double lostAt = -1, worstBefore = 0;
    s.run(40.0, [&](double t, double corr) {
        const double carrier = t * 150.0;
        if (t > 3.0 && lostAt < 0) {
            if (std::fabs(corr - carrier) > 60.0 && carrier > 500.0) { lostAt = carrier; } else if (carrier < 3900.0) { worstBefore = std::max(worstBefore, std::fabs(corr - carrier)); }
        }
    });
    std::printf("MEAS drift 150 Hz/s: tracked within %.1f Hz until the carrier was %.0f Hz off tune (hold setting 4000 Hz); lock at end: %d, correction at end %.0f Hz\n",
                worstBefore, lostAt, afc.locked() ? 1 : 0, afc.correctionHz());
    CHECK(worstBefore < 40.0);
    CHECK(lostAt > 3900.0 && lostAt < 4300.0);
    CHECK(!afc.locked());
    CHECK(afc.correctionHz() <= 4000.0 + 1e-6);
}

void testRateLimit() {
    for (double rate : {500.0, 100.0}) {
        AfcTracker afc(kFs);
        setup(afc, AfcMode::MeanFrequency);
        afc.setMaxRateHzPerS(rate);
        Sim s(afc, 0.003, 9);
        s.amp = amAmp;
        s.carrierHz = [](double t) { return t < 15.0 ? 0.0 : 2000.0; };
        s.run(15.0);
        const double before = afc.correctionHz();
        double maxSlew = 0, doneAt = -1;
        std::vector<double> hist{before};  // one entry per 256-sample chunk; slew is measured over one second
        s.run(60.0, [&](double t, double c) {
            hist.push_back(c);
            const std::size_t lag = static_cast<std::size_t>(kFs / 256.0);
            if (hist.size() > lag) { maxSlew = std::max(maxSlew, std::fabs(c - hist[hist.size() - 1 - lag])); }
            if (doneAt < 0 && std::fabs(c - 2000.0) < 20.0) { doneAt = t - 15.0; }
        });
        std::printf("MEAS 2000 Hz carrier step, rate limit %.0f Hz/s: largest slew over one second %.1f Hz/s, within 20 Hz after %.2f s\n", rate, maxSlew, doneAt);
        CHECK(maxSlew <= rate * 1.05);  // a one-second window can hold 24 of the 43 ms updates
        CHECK(doneAt > 2000.0 / rate * 0.9);
        CHECK(doneAt < 2000.0 / rate * 1.6 + 1.0);
    }
}

void testFreeze() {
    AfcTracker afc(kFs);
    setup(afc, AfcMode::MeanFrequency);
    Sim s(afc, 0.003, 10);
    s.amp = amAmp;
    s.carrierHz = [](double) { return 1000.0; };
    s.run(8.0);
    CHECK(afc.locked());
    const double held = afc.correctionHz();
    // The carrier vanishes for 5 s: only noise.
    s.amp = [](double) { return 0.0; };
    double lo = 1e9, hi = -1e9;
    bool everUnfrozen = false;
    double tn = 0.0;
    s.run(5.0, [&](double, double c) {
        tn += 256.0 / kFs;
        if (tn < 0.1) { return; }  // the first block still holds some of the carrier
        lo = std::min(lo, c); hi = std::max(hi, c);
        if (!afc.frozen()) { everUnfrozen = true; }
    });
    std::printf("MEAS carrier gone for 5 s (noise only): correction moved %.4f Hz (held at %.2f Hz), frozen %d, ever unfrozen %d\n",
                hi - lo, held, afc.frozen() ? 1 : 0, everUnfrozen ? 1 : 0);
    CHECK(hi - lo < 0.01);
    CHECK(afc.frozen() && !everUnfrozen);
    // It returns where it was: caught again at once (the lock was not lost).
    s.amp = amAmp;
    double relock = -1;
    s.run(2.0, [&](double t, double) { if (relock < 0 && !afc.frozen() && afc.locked()) { relock = t - 13.0; } });
    std::printf("MEAS carrier back at the same place: first unfrozen and locked block %.3f s after it returns\n", relock);
    CHECK(relock >= 0 && relock < 0.2);
    CHECK_NEAR(afc.correctionHz(), 1000.0, 10.0);
    // A strong interferer while the wiring says "no signal": nothing moves.
    afc.setSignalPresent(false);
    const double before = afc.correctionHz();
    s.carrierHz = [](double) { return 2200.0; };
    s.run(3.0);
    CHECK(afc.correctionHz() == before && afc.frozen());
    afc.setSignalPresent(true);
    s.run(8.0);
    CHECK_NEAR(afc.correctionHz(), 2200.0, 10.0);  // caught inside the pull-in range once the gate is back
}

void testFm() {
    AfcTracker afc(kFs);
    setup(afc, AfcMode::MeanFrequency);
    Sim s(afc, 0.003, 11);
    s.amp = [](double) { return 0.3; };
    s.carrierHz = [](double t) { return 1500.0 + 2500.0 * std::sin(kTwoPi * 1000.0 * t) + 1200.0 * std::sin(kTwoPi * 370.0 * t); };
    double sum = 0, worst = 0;
    int cnt = 0;
    s.run(14.0, [&](double t, double c) {
        if (t > 10.0) { sum += c - 1500.0; worst = std::max(worst, std::fabs(c - 1500.0)); ++cnt; }
    });
    std::printf("MEAS FM carrier +1500 Hz, +-3.7 kHz two-tone deviation: mean error %.2f Hz, worst %.2f Hz over the last 4 s, locked %d\n", sum / cnt, worst, afc.locked() ? 1 : 0);
    CHECK(afc.locked());
    CHECK(worst < 25.0);
}

void testPeak() {
    // Static carriers between FFT bins (bin spacing 23.4 Hz).
    for (double off : {0.0, 11.7, 600.3, -1234.5}) {
        AfcTracker afc(kFs);
        setup(afc, AfcMode::Peak);
        afc.setDeadbandHz(0.5);
        Sim s(afc, 0.003, 12);
        s.amp = [](double) { return 0.05; };
        s.carrierHz = [off](double) { return off; };
        s.run(12.0);
        std::printf("MEAS Peak mode, tone at %+.1f Hz: correction %.2f Hz (error %.2f), locked %d\n", off, afc.correctionHz(), afc.correctionHz() - off, afc.locked() ? 1 : 0);
        CHECK(std::fabs(afc.correctionHz() - off) < 4.0);
    }
    // Keyed carrier (60 ms on / 60 ms off) at +600 Hz, weak: off blocks are frozen by the peak test.
    AfcTracker afc(kFs);
    setup(afc, AfcMode::Peak);
    Sim s(afc, 0.01, 13);
    s.amp = [](double t) { return std::fmod(t, 0.12) < 0.06 ? 0.05 : 0.0; };
    s.carrierHz = [](double) { return 600.0; };
    double err = 0;
    int cnt = 0;
    s.run(20.0, [&](double t, double c) { if (t > 12.0) { err += std::fabs(c - 600.0); ++cnt; } });
    std::printf("MEAS Peak mode, keyed carrier 60 ms / 60 ms, tone 14 dB under the 48 kHz noise power sum: mean |error| %.2f Hz over the last 8 s, accepted %ld of %ld blocks\n",
                err / cnt, afc.acceptedUpdates(), afc.updates());
    CHECK(err / cnt < 6.0);
    CHECK(afc.acceptedUpdates() < afc.updates());
    // Noise only, level gate opened wide: how often does the peak test let a block through?
    AfcTracker nz(kFs);
    setup(nz, AfcMode::Peak);
    nz.setFreezeLevelDb(-200.0);
    Sim sn(nz, 0.01, 14);
    sn.amp = [](double) { return 0.0; };
    sn.run(600.0);
    std::printf("MEAS Peak mode on 600 s of noise only (level gate open): %ld of %ld blocks accepted, correction %.2f Hz\n", nz.acceptedUpdates(), nz.updates(), nz.correctionHz());
    CHECK(nz.acceptedUpdates() == 0);
    CHECK(nz.correctionHz() == 0.0);
}

void testSnrSweep() {
    // Mean-frequency AM tracking against carrier-to-noise ratio (carrier power over the noise power in the whole 48 kHz channel).
    for (double cnrDb : {40.0, 20.0, 10.0, 5.0}) {
        const double carrier = 0.3;
        const double sigma = carrier / std::sqrt(2.0 * std::pow(10.0, cnrDb / 10.0));  // per component
        AfcTracker afc(kFs);
        setup(afc, AfcMode::MeanFrequency);
        afc.setFreezeLevelDb(-80.0);
        Sim s(afc, sigma, 15);
        s.amp = [](double) { return 0.3; };
        s.carrierHz = [](double) { return 800.0; };
        double sq = 0;
        int cnt = 0;
        s.run(20.0, [&](double t, double c) { if (t > 10.0) { sq += (c - 800.0) * (c - 800.0); ++cnt; } });
        std::printf("MEAS AM tracking, carrier %.0f dB over the 48 kHz channel noise: rms error %.2f Hz over the last 10 s, locked %d\n", cnrDb, std::sqrt(sq / cnt), afc.locked() ? 1 : 0);
        if (cnrDb >= 20.0) { CHECK(std::sqrt(sq / cnt) < 5.0); }
        if (cnrDb == 5.0) { CHECK(std::sqrt(sq / cnt) < 60.0); }
    }
}

void testBasics() {
    // Disabled: nothing happens; switching off resets the correction to 0 once.
    AfcTracker afc(kFs);
    setup(afc, AfcMode::MeanFrequency);
    Sim s(afc, 0.003, 16);
    s.amp = amAmp;
    s.carrierHz = [](double) { return 900.0; };
    s.run(8.0);
    CHECK_NEAR(afc.correctionHz(), 900.0, 10.0);
    afc.setEnabled(false);
    std::vector<std::complex<float>> junk(256, std::complex<float>(0.3f, 0.0f));
    afc.process(junk.data(), junk.size());
    CHECK(afc.correctionHz() == 0.0 && !afc.locked());
    const long u = afc.updates();
    for (int i = 0; i < 100; ++i) { afc.process(junk.data(), junk.size()); }
    CHECK(afc.updates() == u);
    // Bad size falls back; setters clamp, NaN ignored.
    AfcTracker odd(kFs, 1000);
    CHECK(odd.blockSamples() == 2048);
    afc.setPullInHz(1e9);
    afc.setMaxRateHzPerS(std::nan(""));
    // Non-finite samples.
    afc.setEnabled(true);
    std::vector<std::complex<float>> bad(4096, std::complex<float>(std::numeric_limits<float>::quiet_NaN(), 1.0f));
    afc.process(bad.data(), bad.size());
    CHECK(std::isfinite(afc.correctionHz()) && std::isfinite(afc.levelDb()));
    // Chunking invariance: the same input in any chunking gives the same correction history end state.
    std::vector<std::complex<float>> in(200000);
    for (std::size_t i = 0; i < in.size(); ++i) { in[i] = std::polar(0.3f, static_cast<float>(kTwoPi * 700.0 * static_cast<double>(i) / kFs)); }
    double ends[3];
    int k = 0;
    for (std::size_t blk : {1u, 37u, 4096u}) {
        AfcTracker a(kFs);
        setup(a, AfcMode::MeanFrequency);
        for (std::size_t p = 0; p < in.size(); p += blk) { a.process(in.data() + p, std::min<std::size_t>(blk, in.size() - p)); }
        ends[k++] = a.correctionHz();
    }
    CHECK(ends[0] == ends[1] && ends[1] == ends[2]);
    // No allocation in process (both modes), with setting changes.
    AfcTracker al(kFs);
    long news = 0;
    for (int i = 0; i < 40; ++i) {
        AllocProbe p;
        al.setEnabled(true);
        al.setMode((i % 2) ? AfcMode::Peak : AfcMode::MeanFrequency);
        al.setPullInHz(2000 + i);
        al.process(in.data() + 1000 * i, 1000 * 3);
        news += p.news();
    }
    std::printf("MEAS operator new in 40 process calls (both modes, setting changes): %ld\n", news);
    CHECK(news == 0);
}
}  // namespace

int main() {
    testPullIn();
    testHold();
    testRateLimit();
    testFreeze();
    testFm();
    testPeak();
    testSnrSweep();
    testBasics();
    return testSummary("test_afc");
}
