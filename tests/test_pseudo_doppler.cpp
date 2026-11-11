// Tests for dsp/pseudo_doppler.hpp - bearing from one receiver and a switched four-antenna array, on generated signals built from the
// PHYSICAL model (antenna coordinates and a plane wave), with noise, carrier frequency offset, switching clock error and switch settling.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "dsp/pseudo_doppler.hpp"

#include <cmath>
#include <complex>
#include <cstdio>
#include <random>
#include <vector>

#include "rfmon_gen.hpp"
#include "test_check.hpp"

using namespace cascade::dsp;

namespace {

struct Gen {
    double thetaDeg = 0.0;       // true bearing (towards the transmitter, counter-clockwise from antenna 0)
    double ringWavelengths = 0.25;
    double snrDb = 40.0;
    double rho = 0.0;            // carrier offset, rad per sample
    double dwell = 100.0;        // nominal samples per dwell
    double clockError = 0.0;     // fractional error of the true dwell length
    double trueStart = 0.0;
    std::size_t dwells = 400;
    int settle = 2;              // samples at the start of a dwell that are junk
    std::uint32_t seed = 1;
};

std::vector<std::complex<float>> generate(const Gen& g) {
    const double dTrue = g.dwell * (1.0 + g.clockError);
    const std::size_t n = static_cast<std::size_t>(g.trueStart + dTrue * static_cast<double>(g.dwells)) + 8;
    std::mt19937 rng(g.seed);
    std::normal_distribution<double> nd(0.0, std::sqrt(std::pow(10.0, -g.snrDb / 10.0) / 2.0));
    std::uniform_real_distribution<double> ph(0.0, rfgen::kTwoPi);
    const double th = g.thetaDeg * rfgen::kTwoPi / 360.0;
    const double ux = std::cos(th), uy = std::sin(th);
    double a[4];
    for (int k = 0; k < 4; ++k) {                                // plane wave on antenna k at (r cos, r sin) wavelengths: phase advance 2 pi (p . u)
        const double px = g.ringWavelengths * std::cos(k * rfgen::kTwoPi / 4.0);
        const double py = g.ringWavelengths * std::sin(k * rfgen::kTwoPi / 4.0);
        a[k] = rfgen::kTwoPi * (px * ux + py * uy);
    }
    const double phi0 = 0.7;
    std::vector<std::complex<float>> x(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double rel = static_cast<double>(i) - g.trueStart;
        const long j = static_cast<long>(std::floor(rel / dTrue));
        const int k = static_cast<int>(((j % 4) + 4) % 4);
        const double posInDwell = rel - static_cast<double>(j) * dTrue;
        double phase = g.rho * static_cast<double>(i) + phi0 + a[k];
        if (posInDwell < static_cast<double>(g.settle)) { phase = ph(rng); }   // the switch has not settled
        x[i] = std::complex<float>(static_cast<float>(std::cos(phase) + nd(rng)), static_cast<float>(std::sin(phase) + nd(rng)));
    }
    return x;
}

PseudoDopplerResult run(const Gen& g, double nominalStart = 0.0) {
    const std::vector<std::complex<float>> x = generate(g);
    return estimatePseudoDopplerBearing(x.data(), x.size(), g.dwell, nominalStart);
}

void testCleanBearings() {
    std::printf("  clean signal: bearing within half a degree at every 15 degrees, and the sign convention (90 = towards antenna 1)\n");
    for (int t = 0; t < 360; t += 15) {
        Gen g;
        g.thetaDeg = t;
        g.snrDb = 60.0;
        g.dwells = 64;
        const PseudoDopplerResult r = run(g);
        CHECK(r.valid);
        CHECK(pdBearingErrorDeg(r.bearingDeg, t) < 0.5);
        CHECK(r.quality > 0.95);
        CHECK(r.rotations >= 14u);
    }
    // The four antenna directions by name.
    for (int k = 0; k < 4; ++k) {
        Gen g;
        g.thetaDeg = 90.0 * k;
        g.snrDb = 60.0;
        g.dwells = 64;
        const PseudoDopplerResult r = run(g);
        CHECK(pdBearingErrorDeg(r.bearingDeg, 90.0 * k) < 0.5);
    }
    // The ring radius drops out of the bearing.
    for (double rw : {0.1, 0.2, 0.28}) {
        Gen g;
        g.thetaDeg = 200.0;
        g.ringWavelengths = rw;
        g.snrDb = 60.0;
        g.dwells = 64;
        CHECK(pdBearingErrorDeg(run(g).bearingDeg, 200.0) < 0.6);
    }
}

void testOffsetsAndClock() {
    std::printf("  carrier offset, switching clock error and a late start do not move the bearing\n");
    for (double rho : {0.0, 0.003, -0.01, 0.02, -0.04}) {
        Gen g;
        g.thetaDeg = 137.0;
        g.rho = rho;
        g.snrDb = 30.0;
        g.dwells = 128;
        const PseudoDopplerResult r = run(g);
        CHECK(r.valid);
        CHECK(pdBearingErrorDeg(r.bearingDeg, 137.0) < 3.0);
        CHECK_NEAR(r.freqOffsetRadPerSample, rho, 0.002);
    }
    for (double ce : {-0.004, -0.001, 0.001, 0.004}) {
        Gen g;
        g.thetaDeg = 250.0;
        g.clockError = ce;
        g.snrDb = 30.0;
        g.dwells = 400;                                          // over 400 dwells the boundary slides by up to 160 samples
        g.rho = 0.01;
        const PseudoDopplerResult r = run(g);
        CHECK(r.valid);
        CHECK(pdBearingErrorDeg(r.bearingDeg, 250.0) < 3.0);
        CHECK(std::fabs(r.clockOffsetSamples - ce * g.dwell * static_cast<double>(g.dwells)) < 0.35 * g.dwell);
    }
    for (double st : {3.0, 17.0, 40.0, 49.0}) {                   // the caller's start is early by up to half a dwell
        Gen g;
        g.thetaDeg = 20.0;
        g.trueStart = st;
        g.snrDb = 30.0;
        g.dwells = 128;
        const PseudoDopplerResult r = run(g, 0.0);
        CHECK(r.valid);
        CHECK(pdBearingErrorDeg(r.bearingDeg, 20.0) < 3.0);
    }
    {   // fractional dwell length
        Gen g;
        g.thetaDeg = 310.0;
        g.dwell = 97.5;
        g.snrDb = 30.0;
        g.dwells = 200;
        const PseudoDopplerResult r = run(g);
        CHECK(pdBearingErrorDeg(r.bearingDeg, 310.0) < 3.0);
    }
    {   // short dwells
        Gen g;
        g.thetaDeg = 75.0;
        g.dwell = 24.0;
        g.snrDb = 30.0;
        g.dwells = 400;
        CHECK(pdBearingErrorDeg(run(g).bearingDeg, 75.0) < 4.0);
    }
}

void testErrorVersusSnr() {
    std::printf("  bearing error against SNR (36 bearings x 4 seeds each, carrier offset and clock error on): rms and worst, pinned\n");
    struct Row {
        double snr;
        double rmsLimit;
        double worstLimit;
        double minQualityMean;
        double maxQualityMean;   // below the working limit the figure must say so
    };
    // Limits are set from the measured values (printed) with a margin; the run prints what it measured.
    // The working limit of this estimator on 128 dwells of 100 samples is about -7 dB per sample: between -5 and -10 dB it falls off a cliff
    // (a boundary or a carrier-offset estimate is lost), and below it the bearing is noise - which the quality figure must show.
    const Row rows[] = {{30.0, 0.1, 0.2, 0.99, 1.0}, {10.0, 0.3, 0.7, 0.95, 1.0}, {0.0, 0.8, 2.0, 0.9, 1.0}, {-5.0, 1.6, 4.0, 0.8, 1.0},
                        {-10.0, 999.0, 999.0, 0.0, 0.8}, {-15.0, 999.0, 999.0, 0.0, 0.4}};
    double prevRms = 0.0;
    for (const Row& row : rows) {
        double sum2 = 0.0, worst = 0.0, qsum = 0.0;
        int count = 0;
        for (int t = 0; t < 360; t += 10) {
            for (std::uint32_t s = 1; s <= 4; ++s) {
                Gen g;
                g.thetaDeg = t + 3.0;
                g.snrDb = row.snr;
                g.dwells = 128;
                g.rho = 0.012;
                g.clockError = 0.001;
                g.seed = 1000 * s + static_cast<std::uint32_t>(t);
                const PseudoDopplerResult r = run(g);
                const double e = r.valid ? pdBearingErrorDeg(r.bearingDeg, g.thetaDeg) : 180.0;
                sum2 += e * e;
                worst = std::max(worst, e);
                qsum += r.quality;
                ++count;
            }
        }
        const double rms = std::sqrt(sum2 / count);
        std::printf("    SNR %4.1f dB per sample: rms %.2f deg, worst %.2f deg, mean quality %.2f\n", row.snr, rms, worst, qsum / count);
        CHECK(rms < row.rmsLimit);
        CHECK(worst < row.worstLimit);
        CHECK(qsum / count >= row.minQualityMean);
        CHECK(qsum / count <= row.maxQualityMean);
        CHECK(rms >= prevRms * 0.7);        // the error grows (or stays) as the SNR falls
        prevRms = rms;
    }
}

void testFailures() {
    std::printf("  failure paths: unusable arguments, too short a record, noise alone\n");
    Gen g;
    g.dwells = 64;
    const std::vector<std::complex<float>> x = generate(g);
    CHECK(!estimatePseudoDopplerBearing(nullptr, 1000, 100.0, 0.0).valid);
    CHECK(!estimatePseudoDopplerBearing(x.data(), x.size(), 4.0, 0.0).valid);          // dwell below the minimum
    CHECK(!estimatePseudoDopplerBearing(x.data(), x.size(), std::nan(""), 0.0).valid);
    CHECK(!estimatePseudoDopplerBearing(x.data(), x.size(), 100.0, -1.0).valid);
    CHECK(!estimatePseudoDopplerBearing(x.data(), x.size(), 100.0, std::nan("")).valid);
    CHECK(!estimatePseudoDopplerBearing(x.data(), 0, 100.0, 0.0).valid);
    CHECK(!estimatePseudoDopplerBearing(x.data(), 800, 100.0, 0.0).valid);             // fewer than 9 dwells
    CHECK(!estimatePseudoDopplerBearing(x.data(), x.size(), 100.0, 1e9).valid);
    CHECK(estimatePseudoDopplerBearing(x.data(), 1100, 100.0, 0.0).valid || true);     // just enough: must not crash either way
    // Noise alone: valid but the quality says there is nothing coherent.
    double qsum = 0.0;
    int cnt = 0;
    for (std::uint32_t s = 1; s <= 20; ++s) {
        const std::vector<rfgen::cd> w = rfgen::noise(12800, 1.0, s);
        std::vector<std::complex<float>> f(w.size());
        for (std::size_t i = 0; i < w.size(); ++i) { f[i] = std::complex<float>(static_cast<float>(w[i].real()), static_cast<float>(w[i].imag())); }
        const PseudoDopplerResult r = estimatePseudoDopplerBearing(f.data(), f.size(), 100.0, 0.0);
        if (r.valid) {
            qsum += r.quality;
            ++cnt;
            CHECK(r.bearingDeg >= 0.0 && r.bearingDeg < 360.0);
        }
    }
    CHECK(cnt > 0);
    CHECK(qsum / std::max(cnt, 1) < 0.3);
    // A strong signal reads better than noise by a wide margin.
    Gen strong;
    strong.dwells = 128;
    strong.snrDb = 20.0;
    CHECK(run(strong).quality > 0.7);
    // Non-finite samples do not escape as a non-finite bearing.
    std::vector<std::complex<float>> y = x;
    y[500] = std::complex<float>(std::nanf(""), 0.0f);
    const PseudoDopplerResult ry = estimatePseudoDopplerBearing(y.data(), y.size(), 100.0, 0.0);
    CHECK(!ry.valid || std::isfinite(ry.bearingDeg));
    CHECK_NEAR(pdBearingErrorDeg(359.0, 1.0), 2.0, 1e-12);
    CHECK_NEAR(pdBearingErrorDeg(10.0, 190.0), 180.0, 1e-12);
    CHECK_NEAR(pdBearingErrorDeg(-10.0, 10.0), 20.0, 1e-12);
}

}  // namespace

int main() {
    testCleanBearings();
    testOffsetsAndClock();
    testErrorVersusSnr();
    testFailures();
    return testSummary("test_pseudo_doppler");
}
