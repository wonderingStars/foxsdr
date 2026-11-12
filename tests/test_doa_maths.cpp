// Tests for dsp/doa_maths.hpp: covariance, Jacobi eigen-decomposition, Bartlett
// and MUSIC spectra, peak picking, and common-source calibration.
//
// Every array snapshot is generated here from the plane-wave model written
// directly from element COORDINATES (x_m, y_m): phase 2 pi (x cos(theta) + y
// sin(theta)) / lambda (Werner et al. 2025, eq. 1), independently of the
// library's steering() so a wrong steering vector cannot hide. Sources are
// independent complex Gaussian sequences, noise is white complex Gaussian,
// seeds fixed. Bearing errors are Monte-Carlo RMS over random bearings.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "dsp/doa_maths.hpp"

#include "test_check.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <random>
#include <vector>

namespace {

using namespace cascade::dsp::doa;
using cf = std::complex<float>;
constexpr double kPi = 3.14159265358979323846;
constexpr double kFreq = 145.0e6;
const double kLambda = kSpeedOfLight / kFreq;

struct Elem { double x, y; };

std::vector<Elem> ucaElems(int n, double radius) {
    std::vector<Elem> e;
    for (int m = 0; m < n; ++m) {
        e.push_back({radius * std::cos(2.0 * kPi * m / n), radius * std::sin(2.0 * kPi * m / n)});
    }
    return e;
}

std::vector<Elem> ulaElems(int n, double spacing) {
    std::vector<Elem> e;
    for (int m = 0; m < n; ++m) { e.push_back({m * spacing, 0.0}); }
    return e;
}

struct Capture { std::vector<std::vector<cf>> ch; };

cd gauss(std::mt19937& g, double p) {
    std::normal_distribution<double> nd(0.0, std::sqrt(p / 2.0));
    return {nd(g), nd(g)};
}

// Plane waves from `bearings` (radians, each unit power) plus noise at snrDb
// per element; per-channel complex gain `chanGain` (empty = ideal).
Capture generate(const std::vector<Elem>& el, const std::vector<double>& bearings, double snrDb,
                 std::size_t t, unsigned seed, const std::vector<cd>& chanGain = {}) {
    std::mt19937 g(seed);
    const double pn = std::pow(10.0, -snrDb / 10.0);
    const double k = 2.0 * kPi / kLambda;
    Capture c;
    c.ch.assign(el.size(), std::vector<cf>(t));
    for (std::size_t i = 0; i < t; ++i) {
        std::vector<cd> s(bearings.size());
        for (cd& v : s) { v = gauss(g, 1.0); }
        for (std::size_t m = 0; m < el.size(); ++m) {
            cd x = gauss(g, pn);
            for (std::size_t b = 0; b < bearings.size(); ++b) {
                const double ph = k * (el[m].x * std::cos(bearings[b]) + el[m].y * std::sin(bearings[b]));
                x += s[b] * std::polar(1.0, ph);
            }
            if (!chanGain.empty()) { x *= chanGain[m]; }
            c.ch[m][i] = cf(static_cast<float>(x.real()), static_cast<float>(x.imag()));
        }
    }
    return c;
}

Matrix covOf(const Capture& c) {
    std::vector<const cf*> p;
    for (const auto& v : c.ch) { p.push_back(v.data()); }
    return sampleCovariance(p.data(), static_cast<int>(p.size()), c.ch[0].size());
}

double angDiff(double a, double b, double period) {
    double d = std::fmod(a - b, period);
    if (d > period / 2) { d -= period; }
    if (d < -period / 2) { d += period; }
    return d;
}

double deg(double r) { return r * 180.0 / kPi; }

struct Est { std::vector<Peak> peaks; };

Est estimate(const Matrix& r, const Geometry& g, int sources, bool music, double stepDeg = 0.5) {
    const std::vector<double> grid = bearingGrid(g, stepDeg * kPi / 180.0);
    std::vector<double> spec;
    if (music) { spec = musicSpectrum(hermitianEigen(r), g, grid, sources); }
    else { spec = bartlettSpectrum(r, g, grid); }
    return {pickPeaks(spec, grid, g.type == ArrayType::Circular, sources)};
}

// RMS bearing error (degrees) over `trials` random single-source bearings.
double rmsSingle(const Geometry& g, const std::vector<Elem>& el, double snrDb, bool music, int trials,
                 std::size_t snaps, unsigned seed) {
    std::mt19937 rg(seed);
    std::uniform_real_distribution<double> u(0.0, 1.0);
    const double span = g.type == ArrayType::Circular ? 2.0 * kPi : kPi;
    double acc = 0.0;
    for (int t = 0; t < trials; ++t) {
        // keep clear of the linear array's end-fire edges
        const double th = g.type == ArrayType::Circular ? u(rg) * span : (0.1 + 0.8 * u(rg)) * span;
        const Capture c = generate(el, {th}, snrDb, snaps, seed * 1000u + static_cast<unsigned>(t));
        const Est e = estimate(covOf(c), g, 1, music);
        if (e.peaks.empty()) { acc += 180.0 * 180.0; continue; }
        const double d = deg(angDiff(e.peaks[0].bearing, th, span == kPi ? 1e9 : span));
        acc += d * d;
    }
    return std::sqrt(acc / trials);
}

// RMS over both sources of a two-source scene (matched by nearest bearing).
double rmsTwo(const Geometry& g, const std::vector<Elem>& el, double b1, double b2, double snrDb,
              bool music, int trials, std::size_t snaps, unsigned seed, int* resolved) {
    double acc = 0.0;
    int ok = 0;
    const double period = g.type == ArrayType::Circular ? 2.0 * kPi : 1e9;
    for (int t = 0; t < trials; ++t) {
        const Capture c = generate(el, {b1, b2}, snrDb, snaps, seed * 1000u + static_cast<unsigned>(t));
        const Est e = estimate(covOf(c), g, 2, music);
        if (e.peaks.size() < 2) { acc += 2.0 * 90.0 * 90.0; continue; }
        const double a = deg(angDiff(e.peaks[0].bearing, b1, period)), b = deg(angDiff(e.peaks[1].bearing, b2, period));
        const double a2 = deg(angDiff(e.peaks[0].bearing, b2, period)), b2e = deg(angDiff(e.peaks[1].bearing, b1, period));
        const double s1 = a * a + b * b, s2 = a2 * a2 + b2e * b2e;
        const double best = std::min(s1, s2);
        acc += best;
        if (std::sqrt(best / 2.0) < 5.0) { ++ok; }
    }
    if (resolved) { *resolved = ok; }
    return std::sqrt(acc / (2.0 * trials));
}

}  // namespace

int main() {
    // ---- Jacobi eigen-decomposition.
    {
        std::mt19937 g(5);
        for (int n = 1; n <= 8; ++n) {
            Matrix h(n);
            for (int i = 0; i < n; ++i) {
                for (int j = i; j < n; ++j) {
                    const cd v = gauss(g, 1.0);
                    h.at(i, j) = (i == j) ? cd(v.real(), 0.0) : v;
                    h.at(j, i) = std::conj(h.at(i, j));
                }
            }
            const Eigen e = hermitianEigen(h);
            double worstRes = 0.0, worstOrtho = 0.0, trace = 0.0, sum = 0.0;
            for (int k = 0; k < n; ++k) {
                for (int r = 0; r < n; ++r) {
                    cd hv(0.0, 0.0);
                    for (int c = 0; c < n; ++c) { hv += h.at(r, c) * e.vectors.at(c, k); }
                    worstRes = std::max(worstRes, std::abs(hv - e.values[static_cast<std::size_t>(k)] * e.vectors.at(r, k)));
                }
                for (int l = 0; l < n; ++l) {
                    cd d(0.0, 0.0);
                    for (int r = 0; r < n; ++r) { d += std::conj(e.vectors.at(r, k)) * e.vectors.at(r, l); }
                    worstOrtho = std::max(worstOrtho, std::abs(d - (k == l ? 1.0 : 0.0)));
                }
                sum += e.values[static_cast<std::size_t>(k)];
                trace += h.at(k, k).real();
            }
            CHECK(e.converged);
            CHECK(worstRes < 1e-11);
            CHECK(worstOrtho < 1e-12);
            CHECK(std::fabs(sum - trace) < 1e-10);
            for (int k = 1; k < n; ++k) { CHECK(e.values[static_cast<std::size_t>(k - 1)] >= e.values[static_cast<std::size_t>(k)]); }
        }
        // Degenerate: identity plus a rank-1 term (eigenvalue 1 with multiplicity 7).
        Matrix h(8);
        std::vector<cd> v(8);
        for (int i = 0; i < 8; ++i) { v[static_cast<std::size_t>(i)] = std::polar(1.0, 0.7 * i); }
        for (int i = 0; i < 8; ++i) {
            for (int j = 0; j < 8; ++j) {
                h.at(i, j) = 5.0 * v[static_cast<std::size_t>(i)] * std::conj(v[static_cast<std::size_t>(j)]) + (i == j ? 1.0 : 0.0);
            }
        }
        const Eigen e = hermitianEigen(h);
        CHECK(e.converged);
        CHECK(std::fabs(e.values[0] - 41.0) < 1e-9);
        for (int k = 1; k < 8; ++k) { CHECK(std::fabs(e.values[static_cast<std::size_t>(k)] - 1.0) < 1e-9); }
        // Zero matrix and bad input.
        const Eigen z = hermitianEigen(Matrix(3));
        CHECK(z.converged && z.values.size() == 3 && z.values[0] == 0.0);
        CHECK(hermitianEigen(Matrix()).values.empty());
        CHECK(sampleCovariance(nullptr, 3, 10).n == 0);
    }

    // ---- Steering vector against the coordinate formula.
    {
        Geometry g;
        g.type = ArrayType::Circular; g.elements = 5; g.sizeMetres = 0.425 * kLambda; g.frequencyHz = kFreq;
        const std::vector<Elem> el = ucaElems(5, g.sizeMetres);
        const double th = 1.1;
        const std::vector<cd> a = steering(g, th);
        double worst = 0.0;
        for (int m = 0; m < 5; ++m) {
            const cd ref = std::polar(1.0, 2.0 * kPi * (el[static_cast<std::size_t>(m)].x * std::cos(th) + el[static_cast<std::size_t>(m)].y * std::sin(th)) / kLambda);
            worst = std::max(worst, std::abs(a[static_cast<std::size_t>(m)] - ref));
        }
        CHECK(worst < 1e-12);
    }

    // ---- Single source: bearing error against SNR (printed, then pinned).
    Geometry uca5, uca8, ula4;
    uca5.type = ArrayType::Circular; uca5.elements = 5; uca5.sizeMetres = 0.425 * kLambda; uca5.frequencyHz = kFreq;
    uca8.type = ArrayType::Circular; uca8.elements = 8; uca8.sizeMetres = 0.65 * kLambda; uca8.frequencyHz = kFreq;
    ula4.type = ArrayType::Linear; ula4.elements = 4; ula4.sizeMetres = 0.5 * kLambda; ula4.frequencyHz = kFreq;
    const auto el5 = ucaElems(5, uca5.sizeMetres);
    const auto el8 = ucaElems(8, uca8.sizeMetres);
    const auto elL = ulaElems(4, ula4.sizeMetres);
    const double snrs[] = {-10.0, 0.0, 10.0, 20.0};
    double m5[4], b5[4], m8[4], mL[4];
    std::printf("doa single source, 1000 snapshots, 40 random bearings, RMS error in degrees\n");
    std::printf("  SNR dB   UCA5 MUSIC  UCA5 Bartlett  UCA8 MUSIC  ULA4 MUSIC\n");
    for (int i = 0; i < 4; ++i) {
        m5[i] = rmsSingle(uca5, el5, snrs[i], true, 40, 1000, 100 + static_cast<unsigned>(i));
        b5[i] = rmsSingle(uca5, el5, snrs[i], false, 40, 1000, 100 + static_cast<unsigned>(i));
        m8[i] = rmsSingle(uca8, el8, snrs[i], true, 40, 1000, 200 + static_cast<unsigned>(i));
        mL[i] = rmsSingle(ula4, elL, snrs[i], true, 40, 1000, 300 + static_cast<unsigned>(i));
        std::printf("  %6.0f   %10.2f  %13.2f  %10.2f  %10.2f\n", snrs[i], m5[i], b5[i], m8[i], mL[i]);
    }
    CHECK(m5[3] < 1.0);
    CHECK(m5[2] < 1.5);
    CHECK(m5[1] < 3.0);
    CHECK(m5[0] < 15.0);
    CHECK(b5[3] < 3.0);
    CHECK(m8[3] < 1.0);
    CHECK(m8[1] < 3.0);
    CHECK(mL[3] < 1.5);
    CHECK(m5[3] < m5[0]);   // error falls with SNR
    CHECK(m8[1] <= m5[1] * 1.5);

    // ---- Two sources (off-grid bearings so grid snapping cannot flatter the result).
    {
        const double d2r = kPi / 180.0;
        const double snr2[] = {-5.0, 5.0, 20.0};
        double u5[3], u8[3];
        int r5[3], r8[3];
        std::printf("doa two sources 40.3/110.7 deg, 1000 snapshots, 30 trials: RMS error deg (resolved within 5 deg of 30)\n");
        for (int i = 0; i < 3; ++i) {
            u5[i] = rmsTwo(uca5, el5, 40.3 * d2r, 110.7 * d2r, snr2[i], true, 30, 1000, 400, &r5[i]);
            u8[i] = rmsTwo(uca8, el8, 40.3 * d2r, 110.7 * d2r, snr2[i], true, 30, 1000, 400, &r8[i]);
            std::printf("  SNR %5.0f dB: UCA5 MUSIC %.2f (%d)  UCA8 MUSIC %.2f (%d)\n", snr2[i], u5[i], r5[i], u8[i], r8[i]);
        }
        int res5b = 0, resL = 0, resClose = 0, resTight = 0;
        const double e5b = rmsTwo(uca5, el5, 40.3 * d2r, 110.7 * d2r, 20.0, false, 30, 1000, 400, &res5b);
        const double eL = rmsTwo(ula4, elL, 60.3 * d2r, 110.7 * d2r, 20.0, true, 30, 1000, 500, &resL);
        const double e5c = rmsTwo(uca5, el5, 40.3 * d2r, 70.7 * d2r, 20.0, true, 30, 1000, 600, &resClose);
        const double e5t = rmsTwo(uca5, el5, 40.3 * d2r, 52.7 * d2r, 20.0, true, 30, 1000, 700, &resTight);
        std::printf("  UCA5 Bartlett 20 dB: %.2f (%d)   ULA4 MUSIC 60.3/110.7 20 dB: %.2f (%d)\n", e5b, res5b, eL, resL);
        std::printf("  UCA5 MUSIC 30 deg apart 20 dB: %.2f (%d)   12 deg apart: %.2f (%d)\n", e5c, resClose, e5t, resTight);
        CHECK(u5[2] < 1.0);
        CHECK(r5[2] >= 28);
        CHECK(u8[2] < 1.0);
        CHECK(u5[1] < 3.0);
        CHECK(u5[0] < 20.0);
        CHECK(u5[2] < u5[0]);
        CHECK(eL < 3.0);
        CHECK(e5b > u5[2]);   // MUSIC beats Bartlett on two sources
    }

    // ---- Linear array ambiguity: the mirror bearing (-theta) has the identical
    // steering vector and spacing beyond lambda/2 adds grating lobes.
    {
        double worst = 0.0;
        for (double th = 0.2; th < 3.0; th += 0.37) {
            const std::vector<cd> a = steering(ula4, th), b = steering(ula4, -th);
            for (std::size_t m = 0; m < a.size(); ++m) { worst = std::max(worst, std::abs(a[m] - b[m])); }
        }
        CHECK(worst < 1e-12);
        // Spacing = 1 lambda: bearings with cos(theta') = cos(theta) + 1 are indistinguishable.
        Geometry wide = ula4;
        wide.sizeMetres = 1.0 * kLambda;
        const double th = 100.0 * kPi / 180.0;
        const double th2 = std::acos(std::cos(th) + 1.0);
        const std::vector<cd> a = steering(wide, th), b = steering(wide, th2);
        double w2 = 0.0;
        for (std::size_t m = 0; m < a.size(); ++m) { w2 = std::max(w2, std::abs(a[m] - b[m])); }
        CHECK(w2 < 1e-9);
        // The scan grid of a linear array stops at 180 degrees.
        const std::vector<double> grid = bearingGrid(ula4, 0.5 * kPi / 180.0);
        CHECK(std::fabs(grid.back() - kPi) < 1e-9);
        std::printf("doa linear array: mirror steering mismatch %.1e, lambda-spacing grating-lobe mismatch %.1e\n", worst, w2);
    }

    // ---- Confidence: high with a strong signal, low on noise alone.
    {
        const Capture sig = generate(el5, {1.0}, 10.0, 1000, 71);
        const Capture noise = generate(el5, {}, 10.0, 1000, 72);
        const Est es = estimate(covOf(sig), uca5, 1, true);
        const Est en = estimate(covOf(noise), uca5, 1, true);
        std::printf("doa confidence: signal %.3f (%.1f dB over median), noise only %.3f (%.1f dB)\n",
                    es.peaks[0].confidence, es.peaks[0].peakToMedianDb, en.peaks[0].confidence, en.peaks[0].peakToMedianDb);
        CHECK(es.peaks[0].confidence > 0.9);
        CHECK(en.peaks[0].confidence < es.peaks[0].confidence - 0.2);
        CHECK(es.peaks[0].confidence <= 1.0 && es.peaks[0].confidence >= 0.0);
    }

    // ---- Calibration: injected per-channel gain and phase offsets recovered from a common source.
    {
        std::mt19937 rg(9);
        std::uniform_real_distribution<double> ur(-kPi, kPi), ug(-2.0, 2.0);
        std::vector<cd> inj(5);
        for (cd& v : inj) { v = std::polar(std::pow(10.0, ug(rg) / 20.0), ur(rg)); }
        // Common noise source: identical signal on every element, no geometry,
        // independent receiver noise 20 dB down.
        const Capture common = generate(std::vector<Elem>(5, Elem{0.0, 0.0}), {0.0}, 20.0, 4096, 81, inj);
        const Calibration cal = calibrateFromCommonSource(covOf(common));
        CHECK(cal.ok);
        CHECK(cal.dominance > 20.0);
        double worstPh = 0.0, worstGain = 0.0;
        for (int i = 0; i < 5; ++i) {
            // correction x injected must equal channel 0's injection (common offset allowed)
            const cd residual = cal.factor[static_cast<std::size_t>(i)] * inj[static_cast<std::size_t>(i)] / inj[0];
            worstPh = std::max(worstPh, std::fabs(deg(std::arg(residual))));
            worstGain = std::max(worstGain, std::fabs(20.0 * std::log10(std::abs(residual))));
        }
        std::printf("doa calibration (5 channels, 4096 snapshots, 20 dB): worst phase error %.3f deg, worst gain error %.3f dB, dominance %.0f\n",
                    worstPh, worstGain, cal.dominance);
        CHECK(worstPh < 1.0);
        CHECK(worstGain < 0.1);

        // Effect on bearing: same offsets on a plane wave, with and without calibration.
        double errRaw = 0.0, errCal = 0.0;
        const int trials = 20;
        for (int t = 0; t < trials; ++t) {
            const double th = ur(rg) + kPi;
            const Capture c = generate(el5, {th}, 20.0, 1000, 900 + static_cast<unsigned>(t), inj);
            const Matrix r = covOf(c);
            const Est raw = estimate(r, uca5, 1, true);
            const Est fixd = estimate(applyCalibration(r, cal.factor), uca5, 1, true);
            const double dr = deg(angDiff(raw.peaks[0].bearing, th, 2.0 * kPi));
            const double dc = deg(angDiff(fixd.peaks[0].bearing, th, 2.0 * kPi));
            errRaw += dr * dr;
            errCal += dc * dc;
        }
        errRaw = std::sqrt(errRaw / trials);
        errCal = std::sqrt(errCal / trials);
        std::printf("doa bearing RMS error with injected channel offsets: uncalibrated %.2f deg, calibrated %.2f deg\n", errRaw, errCal);
        CHECK(errCal < 1.5);
        CHECK(errRaw > 5.0 * errCal);
        // A capture that is not one common source is refused or flagged by dominance.
        const Capture indep = generate(std::vector<Elem>(5, Elem{0.0, 0.0}), {}, 0.0, 4096, 82);
        const Calibration bad = calibrateFromCommonSource(covOf(indep));
        CHECK(!bad.ok || bad.dominance < 3.0);
    }

    // ---- CPU cost, N = 8: covariance of 4096 snapshots, eigen, MUSIC on a 0.5 degree grid.
    {
        const Capture c = generate(el8, {0.8}, 10.0, 4096, 91);
        const auto t0 = std::chrono::steady_clock::now();
        const Matrix r = covOf(c);
        const auto t1 = std::chrono::steady_clock::now();
        const Eigen e = hermitianEigen(r);
        const auto t2 = std::chrono::steady_clock::now();
        const std::vector<double> grid = bearingGrid(uca8, 0.5 * kPi / 180.0);
        const std::vector<double> s = musicSpectrum(e, uca8, grid, 1);
        const auto t3 = std::chrono::steady_clock::now();
        const auto ms = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
        std::printf("doa cpu N=8: covariance(4096) %.2f ms, Jacobi %.3f ms (%d sweeps), MUSIC 720 bearings %.2f ms\n",
                    ms(t0, t1), ms(t1, t2), e.sweeps, ms(t2, t3));
        CHECK(s.size() == 720);
    }

    return testSummary("test_doa_maths");
}
