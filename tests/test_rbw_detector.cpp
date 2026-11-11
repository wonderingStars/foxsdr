// Tests for dsp/rbw_detector.hpp - RBW readout, detectors, VBW and the quasi-peak model, on generated tones and noise.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "dsp/rbw_detector.hpp"

#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

#include "rfmon_gen.hpp"
#include "test_check.hpp"

using namespace cascade::dsp;

namespace {

constexpr std::size_t kN = 1024;
constexpr double kFs = 1024.0;   // bin width exactly 1 Hz, so Hz and bins are the same number

double mean(const std::vector<float>& v) {
    double s = 0;
    for (float x : v) { s += x; }
    return v.empty() ? 0.0 : s / static_cast<double>(v.size());
}
double dbToPow(double db) { return std::pow(10.0, db / 10.0); }

void testEnbw() {
    std::printf("  ENBW: closed form equals the sums of the window, 1.0 and 1.5 are the published values\n");
    CHECK_NEAR(rbwEnbwBins(RbwWindow::Rectangular), 1.0, 1e-12);
    CHECK_NEAR(rbwEnbwBins(RbwWindow::Hann), 1.5, 1e-12);                       // Audio Precision: Hann 1.5, rectangular 1.0
    CHECK_NEAR(rbwEnbwBins(RbwWindow::Hamming), (0.54 * 0.54 + 0.46 * 0.46 / 2.0) / (0.54 * 0.54), 1e-12);
    const RbwWindow all[] = {RbwWindow::Rectangular, RbwWindow::Hann, RbwWindow::Hamming, RbwWindow::BlackmanHarris, RbwWindow::FlatTop};
    for (RbwWindow w : all) {
        std::vector<float> win(4096);
        rbwFillWindow(w, win.size(), win.data());
        double s = 0, s2 = 0;
        for (float x : win) { s += x; s2 += static_cast<double>(x) * x; }
        CHECK_NEAR(static_cast<double>(win.size()) * s2 / (s * s), rbwEnbwBins(w), 2e-5);
    }
    CHECK_NEAR(rbwEnbwHz(RbwWindow::Hann, 48000.0, 4800), 15.0, 1e-9);           // 1.5 bins of 10 Hz
    CHECK(rbwEnbwHz(RbwWindow::Hann, 0.0, 4800) == 0.0);
    CHECK(rbwEnbwHz(RbwWindow::Hann, 48000.0, 0) == 0.0);
    CHECK(rbwEnbwHz(RbwWindow::Hann, std::nan(""), 8) == 0.0);
}

void testFftSizeFor() {
    std::printf("  FFT size for a wanted RBW\n");
    const RbwFftChoice c = rbwFftSizeFor(RbwWindow::Hann, 1000000.0, 1000.0);   // ENBW 1.5 * 1e6/n <= 1000 -> n >= 1500 -> 2048
    CHECK(c.reachable);
    CHECK(c.n == 2048u);
    CHECK(c.rbwHz <= 1000.0);
    CHECK_NEAR(c.rbwHz, 1.5 * 1e6 / 2048.0, 1e-9);
    const RbwFftChoice fine = rbwFftSizeFor(RbwWindow::Hann, 1000000.0, 0.001, 64, 4096);
    CHECK(!fine.reachable);
    CHECK(fine.n == 4096u);
    const RbwFftChoice coarse = rbwFftSizeFor(RbwWindow::Rectangular, 1000.0, 1e9, 64, 4096);
    CHECK(coarse.reachable);
    CHECK(coarse.n == 64u);                                                      // minN, already finer than asked
    CHECK(rbwFftSizeFor(RbwWindow::Hann, -1.0, 100.0).n == 0u);
    CHECK(rbwFftSizeFor(RbwWindow::Hann, 1000.0, 0.0).n == 0u);
    CHECK(rbwFftSizeFor(RbwWindow::Hann, 1000.0, 100.0, 1, 4).n == 0u);
    CHECK(rbwFftSizeFor(RbwWindow::Hann, 1000.0, 100.0, 64, 32).n == 0u);
}

void testPlanAndToneLoss() {
    std::printf("  plan: clamp below ENBW, combine bins above, tone loss from the window\n");
    RbwPlan p = rbwPlan(RbwWindow::Hann, kFs, kN, 0.5);        // finer than the 1.5 Hz ENBW
    CHECK(p.valid && p.clamped && p.combine == 1u);
    CHECK_NEAR(p.rbwHz, 1.5, 1e-12);
    p = rbwPlan(RbwWindow::Hann, kFs, kN, 0.0);
    CHECK(p.valid && !p.clamped && p.combine == 1u);
    p = rbwPlan(RbwWindow::Hann, kFs, kN, 1.5);
    CHECK(p.valid && !p.clamped && p.combine == 1u);
    p = rbwPlan(RbwWindow::Hann, kFs, kN, 9.0);
    CHECK(p.valid && !p.clamped && p.combine == 9u);
    CHECK_NEAR(p.rbwHz, 9.0, 1e-12);
    CHECK(p.toneLossDb < 0.05);
    p = rbwPlan(RbwWindow::Hann, kFs, kN, std::nan(""));
    CHECK(p.valid && p.clamped && p.combine == 1u);
    CHECK(!rbwPlan(RbwWindow::Hann, 0.0, kN, 9.0).valid);
    CHECK(!rbwPlan(RbwWindow::Hann, kFs, 0, 9.0).valid);
    // A single bin under-reads a Hann tone that sits between bins (1.42 dB), seven bins do not.
    CHECK_NEAR(rbwToneLossDb(RbwWindow::Hann, 1), 1.4236, 0.02);                   // the familiar 1.42 dB scalloping loss of Hann
    CHECK(rbwToneLossDb(RbwWindow::Hann, 7) < 0.05);
    CHECK(rbwToneLossDb(RbwWindow::BlackmanHarris, 3) > rbwToneLossDb(RbwWindow::BlackmanHarris, 9));
    CHECK(rbwToneLossDb(RbwWindow::FlatTop, 11) < 0.05);
    CHECK(rbwToneLossDb(RbwWindow::Hann, 0) == rbwToneLossDb(RbwWindow::Hann, 1));   // m < 1 is 1
    CHECK_NEAR(rbwToneLossDb(RbwWindow::Rectangular, 1), 3.9224, 0.02);              // |sinc(0.5)|^2 = 0.405 -> 3.92 dB
}

// Builds frames from tone + noise, returns the trace after `frames` frames (Hz == bins).
struct Run {
    std::vector<float> db;
    RbwPlan plan;
};
Run runTrace(RbwTraceConfig cfg, const std::vector<rfgen::cd>& x, std::size_t frames, std::size_t hop = kN) {
    Run r;
    RbwTrace t;
    CHECK(t.configure(cfg));
    r.plan = t.plan();
    std::vector<float> win(kN);
    rbwFillWindow(cfg.window, kN, win.data());
    for (std::size_t f = 0; f < frames; ++f) {
        const std::vector<float> fr = rfgen::psdFrame(x, f * hop, kN, win.data(), kFs);
        CHECK(t.pushFrame(fr.data(), fr.size()));
    }
    r.db.resize(t.points());
    CHECK(t.takeTrace(r.db.data(), r.db.size(), 1.0));
    return r;
}

void testToneReadsItsPower() {
    std::printf("  a tone reads its true power through the combined RBW, at every position between bins\n");
    const RbwWindow wins[] = {RbwWindow::Hann, RbwWindow::BlackmanHarris, RbwWindow::FlatTop};
    for (RbwWindow w : wins) {
        for (double off : {0.0, 0.25, 0.5}) {
            std::vector<rfgen::cd> x(kN * 2, rfgen::cd(0, 0));
            rfgen::addTone(x, kFs, 100.0 + off, 2.0, 0.3);   // power 2 -> 3.01 dB
            RbwTraceConfig c;
            c.window = w;
            c.sampleRateHz = kFs;
            c.bins = kN;
            c.wantedRbwHz = 17.0;
            c.points = 0;
            c.detector = RbwDetector::Peak;
            const Run r = runTrace(c, x, 1);
            CHECK(r.plan.combine == 17u);
            float top = -1000.0f;
            for (float v : r.db) { top = std::max(top, v); }
            // The readout must lie between the stated worst-case loss and the true power (small leakage tolerance).
            CHECK(top <= 3.0103f + 0.06f);
            CHECK(top >= 3.0103f - static_cast<float>(r.plan.toneLossDb) - 0.06f);
        }
    }
    // Where the plan says a tone is under-read, it really is: single-bin Hann, tone half way between bins.
    std::vector<rfgen::cd> x(kN, rfgen::cd(0, 0));
    rfgen::addTone(x, kFs, 100.5, 1.0);
    RbwTraceConfig c;
    c.window = RbwWindow::Hann;
    c.sampleRateHz = kFs;
    c.bins = kN;
    c.detector = RbwDetector::Peak;
    const Run r = runTrace(c, x, 1);
    float top = -1000.0f;
    for (float v : r.db) { top = std::max(top, v); }
    CHECK_NEAR(top, -rbwToneLossDb(RbwWindow::Hann, 1), 0.05);
    CHECK_NEAR(rbwPlan(RbwWindow::Hann, kFs, kN, 0.0).toneLossDb, 1.4236, 0.02);
}

void testNoiseReadsDensityTimesBandwidth() {
    std::printf("  noise reads density x RBW: native ENBW, and M combined bins\n");
    const double p = 1.024;                                    // power -> density 1e-3 W/Hz at fs 1024
    const double d = p / kFs;
    const std::vector<rfgen::cd> x = rfgen::noise(kN * 120, p, 11);
    for (RbwWindow w : {RbwWindow::Hann, RbwWindow::BlackmanHarris}) {
        RbwTraceConfig c;
        c.window = w;
        c.sampleRateHz = kFs;
        c.bins = kN;
        c.points = 1;
        c.detector = RbwDetector::Rms;
        Run r = runTrace(c, x, 120);                           // native
        CHECK_NEAR(r.db[0], 10.0 * std::log10(d * rbwEnbwHz(w, kFs, kN)), 0.08);
        c.wantedRbwHz = 8.0;
        r = runTrace(c, x, 120);
        CHECK(r.plan.combine == 8u);
        CHECK_NEAR(r.db[0], 10.0 * std::log10(d * 8.0), 0.08);
        c.wantedRbwHz = 32.0;
        const Run r2 = runTrace(c, x, 120);
        CHECK_NEAR(r2.db[0] - r.db[0], 10.0 * std::log10(4.0), 0.08);   // 4x RBW = +6.02 dB
    }
}

void testDetectorRelationsOnNoise() {
    std::printf("  detector relations on noise: voltage average -1.05 dB, log average -2.51 dB, max of K = H_K, min of K = 1/K\n");
    const double p = 1.024;
    const std::vector<rfgen::cd> x = rfgen::noise(kN * 130, p, 21);
    RbwTraceConfig c;
    c.window = RbwWindow::Hann;
    c.sampleRateHz = kFs;
    c.bins = kN;
    c.points = 1;
    c.detector = RbwDetector::Rms;
    const double rms = runTrace(c, x, 120).db[0];
    c.detector = RbwDetector::AvgVolt;
    const double av = runTrace(c, x, 120).db[0];
    c.detector = RbwDetector::AvgLog;
    const double al = runTrace(c, x, 120).db[0];
    CHECK_NEAR(av - rms, 10.0 * std::log10(3.14159265358979 / 4.0), 0.1);
    CHECK_NEAR(al - rms, -2.5068, 0.1);                         // 10 log10(exp(-gamma))
    // Per-bin detectors across K = 16 independent frames: points = bins.
    c.points = 0;
    double h16 = 0;
    for (int k = 1; k <= 16; ++k) { h16 += 1.0 / k; }
    c.detector = RbwDetector::Peak;
    {
        const Run r = runTrace(c, x, 16);
        double s = 0;
        for (float v : r.db) { s += dbToPow(v); }
        const double mu = p / kFs * rbwEnbwHz(RbwWindow::Hann, kFs, kN);
        CHECK_NEAR(s / static_cast<double>(r.db.size()) / mu, h16, 0.06 * h16);
    }
    c.detector = RbwDetector::NegPeak;
    {
        const Run r = runTrace(c, x, 16);
        double s = 0;
        for (float v : r.db) { s += dbToPow(v); }
        const double mu = p / kFs * rbwEnbwHz(RbwWindow::Hann, kFs, kN);
        CHECK_NEAR(s / static_cast<double>(r.db.size()) / mu, 1.0 / 16.0, 0.008);
    }
}

void testAllDetectorsAgreeOnATone() {
    std::printf("  a steady tone: every detector reads the same\n");
    std::vector<rfgen::cd> x(kN * 8, rfgen::cd(0, 0));
    rfgen::addTone(x, kFs, 200.0, 1.0);
    for (std::size_t i = 0; i < x.size(); ++i) { x[i] += rfgen::cd(0, 0); }
    const RbwDetector dets[] = {RbwDetector::Peak, RbwDetector::Rms, RbwDetector::AvgVolt, RbwDetector::AvgLog, RbwDetector::Sample};
    double first = 0;
    bool haveFirst = false;
    for (RbwDetector d : dets) {
        RbwTraceConfig c;
        c.window = RbwWindow::Hann;
        c.sampleRateHz = kFs;
        c.bins = kN;
        c.wantedRbwHz = 9.0;
        c.detector = d;
        // Look at the point under the tone (bin 512 + 200 in the shifted frame).
        const Run r = runTrace(c, x, 8);
        const double v = r.db[712];
        if (!haveFirst) { first = v; haveFirst = true; }
        CHECK_NEAR(v, first, 0.02);
        CHECK_NEAR(v, 0.0, 0.05);
    }
}

void testSampleDetector() {
    std::printf("  sample detector: the centre bin of the last frame, exactly\n");
    RbwTraceConfig c;
    c.window = RbwWindow::Rectangular;
    c.sampleRateHz = kFs;
    c.bins = 8;
    c.points = 4;                                              // two bins per point; the centre is the second (index 2p+1 .. rounded)
    c.detector = RbwDetector::Sample;
    RbwTrace t;
    CHECK(t.configure(c));
    const float f1[8] = {1, 1, 1, 1, 1, 1, 1, 1};
    const float f2[8] = {2, 3, 4, 5, 6, 7, 8, 9};
    CHECK(t.pushFrame(f1, 8));
    CHECK(t.pushFrame(f2, 8));
    float out[4] = {};
    CHECK(t.takeTrace(out, 4, 1.0));
    // Native RBW of the rectangular window is 1 bin = 1 Hz of 128 Hz bins... bin = fs/8 = 128 Hz, ENBW = 128 Hz.
    const double scale = 128.0;
    // Centre bin index of point p is (2p+1)*8/(2*4) = p*2+1.
    CHECK_NEAR(out[0], 10.0 * std::log10(3.0 * scale), 1e-4);
    CHECK_NEAR(out[1], 10.0 * std::log10(5.0 * scale), 1e-4);
    CHECK_NEAR(out[2], 10.0 * std::log10(7.0 * scale), 1e-4);
    CHECK_NEAR(out[3], 10.0 * std::log10(9.0 * scale), 1e-4);
}

void testBinMappingMoreAndFewerPointsThanBins() {
    std::printf("  display points: fewer than the bins (peak per bucket), more than the bins (a bin may serve several points)\n");
    RbwTraceConfig c;
    c.window = RbwWindow::Rectangular;
    c.sampleRateHz = 8.0;
    c.bins = 8;
    c.points = 2;
    c.detector = RbwDetector::Peak;
    RbwTrace t;
    CHECK(t.configure(c));
    const float f[8] = {1, 9, 1, 1, 1, 1, 5, 1};
    CHECK(t.pushFrame(f, 8));
    float o[2];
    CHECK(t.takeTrace(o, 2, 1.0));
    CHECK_NEAR(o[0], 10.0 * std::log10(9.0), 1e-4);
    CHECK_NEAR(o[1], 10.0 * std::log10(5.0), 1e-4);
    c.points = 16;
    CHECK(t.configure(c));
    CHECK(t.pushFrame(f, 8));
    float o16[16];
    CHECK(t.takeTrace(o16, 16, 1.0));
    CHECK_NEAR(o16[2], 10.0 * std::log10(9.0), 1e-4);          // points 2 and 3 are bin 1
    CHECK_NEAR(o16[3], 10.0 * std::log10(9.0), 1e-4);
    CHECK_NEAR(o16[12], 10.0 * std::log10(5.0), 1e-4);
}

void testVbw() {
    std::printf("  VBW: step response, variance ratio alpha/(2-alpha), off states\n");
    CHECK(RbwTrace::vbwAlpha(0.0, 1.0) == 1.0);
    CHECK(RbwTrace::vbwAlpha(-1.0, 1.0) == 1.0);
    CHECK(RbwTrace::vbwAlpha(1.0, 0.0) == 1.0);
    CHECK(RbwTrace::vbwAlpha(std::nan(""), 1.0) == 1.0);
    CHECK(RbwTrace::vbwAlpha(1000.0, 1.0) > 0.999999);
    const double alpha = RbwTrace::vbwAlpha(0.1, 1.0);
    CHECK_NEAR(alpha, 1.0 - std::exp(-0.1 * kRbwTwoPi), 1e-12);
    RbwTraceConfig c;
    c.window = RbwWindow::Rectangular;
    c.sampleRateHz = 1.0;
    c.bins = 2;
    c.points = 1;
    c.detector = RbwDetector::Rms;
    c.vbwHz = 0.1;
    c.vbwDomain = VbwDomain::Power;
    RbwTrace t;
    CHECK(t.configure(c));
    // Step: 1.0 then 5.0 forever. Rectangular window: enbw = bin = fs/2 = 0.5 Hz; reading = psd * 0.5.
    const float lo[2] = {2.0f, 2.0f};
    const float hi[2] = {10.0f, 10.0f};
    float o = 0;
    CHECK(t.pushFrame(lo, 2));
    CHECK(t.takeTrace(&o, 1, 1.0));
    CHECK_NEAR(o, 10.0 * std::log10(1.0), 1e-4);               // first trace passes through
    for (int k = 1; k <= 5; ++k) {
        CHECK(t.pushFrame(hi, 2));
        CHECK(t.takeTrace(&o, 1, 1.0));
        const double expect = 1.0 + (5.0 - 1.0) * (1.0 - std::pow(1.0 - alpha, k));
        CHECK_NEAR(o, 10.0 * std::log10(expect), 1e-3);
    }
    t.resetVideo();
    CHECK(t.pushFrame(hi, 2));
    CHECK(t.takeTrace(&o, 1, 1.0));
    CHECK_NEAR(o, 10.0 * std::log10(5.0), 1e-4);               // memory forgotten
    // Variance of iid input falls by alpha / (2 - alpha) (log video, dB values uniform in [0, 10]).
    c.vbwDomain = VbwDomain::Log;
    CHECK(t.configure(c));
    std::mt19937 g(5);
    std::uniform_real_distribution<double> u(0.0, 10.0);
    const int n = 40000;
    std::vector<double> in(n), outv(n);
    for (int i = 0; i < n; ++i) {
        const double db = u(g);
        in[i] = db;
        const float v = static_cast<float>(std::pow(10.0, db / 10.0) / 0.5);   // psd so that psd * 0.5 reads db
        const float fr[2] = {v, v};
        CHECK(t.pushFrame(fr, 2));
        float r = 0;
        CHECK(t.takeTrace(&r, 1, 1.0));
        outv[i] = r;
    }
    auto var = [](const std::vector<double>& v) {
        double m = 0, s = 0;
        for (double x : v) { m += x; }
        m /= static_cast<double>(v.size());
        for (double x : v) { s += (x - m) * (x - m); }
        return s / static_cast<double>(v.size());
    };
    CHECK_NEAR(var(outv) / var(in), alpha / (2.0 - alpha), 0.02);
    // With the VBW off the trace is the detector's output untouched.
    c.vbwHz = 0.0;
    CHECK(t.configure(c));
    const float fr[2] = {20.0f, 20.0f};
    CHECK(t.pushFrame(fr, 2));
    CHECK(t.takeTrace(&o, 1, 1.0));
    CHECK_NEAR(o, 10.0, 1e-4);
}

void testFailuresAndEdges() {
    std::printf("  failure paths: bad configuration, wrong frame length, empty trace, small buffer, non-finite bins\n");
    RbwTrace t;
    RbwTraceConfig c;
    c.sampleRateHz = kFs;
    c.bins = 0;
    CHECK(!t.configure(c));
    CHECK(!t.valid());
    c.bins = 1;
    CHECK(!t.configure(c));
    c.bins = 64;
    c.sampleRateHz = 0.0;
    CHECK(!t.configure(c));
    c.sampleRateHz = std::nan("");
    CHECK(!t.configure(c));
    c.sampleRateHz = kFs;
    c.points = kRbwMaxPoints + 1;
    CHECK(!t.configure(c));
    c.points = 0;
    c.bins = kRbwMaxBins + 1;
    CHECK(!t.configure(c));
    c.bins = 64;
    CHECK(t.configure(c));
    std::vector<float> f(64, 1.0f);
    float out[64];
    CHECK(!t.takeTrace(out, 64, 1.0));                         // nothing pushed
    CHECK(!t.pushFrame(f.data(), 63));
    CHECK(!t.pushFrame(nullptr, 64));
    CHECK(t.pushFrame(f.data(), 64));
    CHECK(!t.takeTrace(out, 63, 1.0));                         // capacity too small
    CHECK(!t.takeTrace(nullptr, 64, 1.0));
    f[3] = std::nanf("");
    f[4] = std::numeric_limits<float>::infinity();
    f[5] = -4.0f;
    CHECK(t.pushFrame(f.data(), 64));
    CHECK(t.takeTrace(out, 64, 1.0));
    for (float v : out) { CHECK(std::isfinite(v)); }
    // A trace that was taken starts the next one afresh.
    CHECK(!t.takeTrace(out, 64, 1.0));
    RbwTrace unconfigured;
    CHECK(!unconfigured.pushFrame(f.data(), 64));
}

void testQuasiPeak() {
    std::printf("  quasi-peak: published time constants, carrier reads itself, pulse train matches the closed form, falls with repetition rate\n");
    const QpTimes a = qpTimesFor(CisprBand::A), b = qpTimesFor(CisprBand::B), cd = qpTimesFor(CisprBand::CD);
    CHECK_NEAR(a.chargeS, 0.045, 1e-12);
    CHECK_NEAR(a.dischargeS, 0.5, 1e-12);
    CHECK_NEAR(a.meterS, 0.16, 1e-12);
    CHECK_NEAR(b.chargeS, 0.001, 1e-12);
    CHECK_NEAR(b.dischargeS, 0.16, 1e-12);
    CHECK_NEAR(b.meterS, 0.16, 1e-12);
    CHECK_NEAR(cd.chargeS, 0.001, 1e-12);
    CHECK_NEAR(cd.dischargeS, 0.55, 1e-12);
    CHECK_NEAR(cd.meterS, 0.1, 1e-12);
    CHECK(qpFramePeriodOk(CisprBand::B, 0.0005));
    CHECK(!qpFramePeriodOk(CisprBand::B, 0.0006));
    CHECK(!qpFramePeriodOk(CisprBand::B, 0.0));
    CHECK(!qpFramePeriodOk(CisprBand::B, std::nan("")));
    QuasiPeakDetector q;
    CHECK(!q.configure(CisprBand::B, 0.01, 4));                // too slow a frame rate for a 1 ms charge
    CHECK(!q.configure(CisprBand::B, 0.0005, 0));
    CHECK(!q.valid());
    CHECK(q.configure(CisprBand::B, 0.0005, 2));
    float amp[2] = {1.0f, 0.0f};
    float m[2];
    CHECK(!q.step(amp, 3, m));
    CHECK(!q.step(nullptr, 2, m));
    for (int i = 0; i < 20000; ++i) { CHECK(q.step(amp, 2, m)); }   // 10 s of a carrier
    CHECK_NEAR(m[0], 1.0, 1e-4);
    CHECK_NEAR(m[1], 0.0, 1e-9);
    q.reset();
    CHECK(q.detectorValue(0) == 0.0);
    // A pulse train: one frame of amplitude 1 every k frames (frame 0.5 ms, band C/D).
    const double dt = 0.0005;
    const double ac = 1.0 - std::exp(-dt / 0.001), ad = 1.0 - std::exp(-dt / 0.55);
    double prev = 2.0;
    for (int k : {2, 20, 200, 2000}) {
        QuasiPeakDetector p;
        CHECK(p.configure(CisprBand::CD, dt, 1));
        float one = 1.0f, zero = 0.0f, mo = 0;
        double top = 0;
        float meterMax = 0;
        for (int i = 0; i < 40000; ++i) {                       // 20 s: well past the settling of the 550 ms discharge
            const bool pulse = (i % k) == 0;
            CHECK(p.step(pulse ? &one : &zero, 1, &mo));
            if (i > 36000) {
                if (pulse) { top = p.detectorValue(0); }
                meterMax = std::max(meterMax, mo);
            }
        }
        const double expect = ac / (1.0 - (1.0 - ac) * std::pow(1.0 - ad, k - 1));
        CHECK_NEAR(top, expect, 1e-6);
        CHECK(top < prev);                                      // falls with repetition rate
        CHECK(meterMax <= static_cast<float>(top) + 1e-6f);
        if (k == 2) { CHECK(top > 0.99); }                      // 1000 pulses/s: a carrier
        prev = top;
    }
    CHECK(prev < 0.5);                                          // 1 pulse/s reads well below the peak (closed form 0.437)
    // Non-finite and negative amplitudes count as zero.
    CHECK(q.configure(CisprBand::B, 0.0005, 2));
    float bad[2] = {std::nanf(""), -3.0f};
    CHECK(q.step(bad, 2, m));
    CHECK(m[0] == 0.0f && m[1] == 0.0f);
}

}  // namespace

int main() {
    testEnbw();
    testFftSizeFor();
    testPlanAndToneLoss();
    testToneReadsItsPower();
    testNoiseReadsDensityTimesBandwidth();
    testDetectorRelationsOnNoise();
    testAllDetectorsAgreeOnATone();
    testSampleDetector();
    testBinMappingMoreAndFewerPointsThanBins();
    testVbw();
    testFailuresAndEdges();
    testQuasiPeak();
    return testSummary("test_rbw_detector");
}
