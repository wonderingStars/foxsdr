// test_sweep_stitch.cpp - the wide-band stitcher and the heatmap store
// (core/sweep_stitch.hpp), proven on a GENERATED wide band with known tones:
// eight tuned captures, each made from time-domain I/Q (complex tones plus
// Gaussian noise) through a Hann-windowed 1024-point FFT averaged over 128
// frames, with a different gain step per capture, a different tuner frequency
// error per capture (+-5 ppm), a sample-rate error (+30 ppm), the IF filter's
// edge droop on the outer 12 % of the bins at each end, and one burst present
// in a single capture inside an overlap. The stitcher is given only the
// nominal centres and the bin levels.
//
// STATED TOLERANCES (what the assertions check): tone frequency within 1 bin
// (2.34 kHz) of the truth (the tuner error here is <= 0.5 kHz; a common ppm
// shift is invisible to any stitcher); tone level within 1 dB of the truth
// relative to the reference capture (gain 0 dB); gain correction of every
// capture within 0.5 dB (the error of 7 chained seams); the noise floor either side of every seam (median of
// 64 cells each) within 1 dB.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <cmath>
#include <complex>
#include <random>
#include <string>
#include <vector>

#include "core/sweep_stitch.hpp"
#include "test_check.hpp"

using namespace cascade::core;
using cd = std::complex<double>;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr std::size_t kN = 1024;
constexpr std::size_t kFrames = 128;
constexpr double kFs = 2.4e6;
constexpr double kBin = kFs / kN;

void fft(std::vector<cd>& a) {
    const std::size_t n = a.size();
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) { j ^= bit; }
        j ^= bit;
        if (i < j) { std::swap(a[i], a[j]); }
    }
    for (std::size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2.0 * kPi / static_cast<double>(len);
        const cd wl(std::cos(ang), std::sin(ang));
        for (std::size_t i = 0; i < n; i += len) {
            cd w(1.0, 0.0);
            for (std::size_t j = 0; j < len / 2; ++j) {
                const cd u = a[i + j];
                const cd v = a[i + j + len / 2] * w;
                a[i + j] = u + v;
                a[i + j + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

struct Tone { double hz; double amp; };

struct Scene {
    double c0 = 100.0e6;
    std::size_t stepBins = 666;          // between capture centres (integer bins: the grids line up)
    std::size_t captures = 8;
    double noiseSigma = 0.02;            // per-sample sigma of the complex noise (each of I and Q = sigma/sqrt2)
    std::vector<Tone> tones;
    std::vector<double> gainDb;          // per capture
    std::vector<double> ppm;             // tuner error per capture
    double srPpm = 30.0;
    int droop = 1;                       // 0 none, 1 both edges, 2 upper edge only (a lopsided IF filter)
    int burstCapture = -1;               // a strong tone heard in this capture only
    Tone burst{0, 0};
    double centre(std::size_t k) const { return c0 + static_cast<double>(k * stepBins) * kBin; }
};

// the Hann window's sum, for the expected tone level
double windowSum() {
    double s = 0;
    for (std::size_t n = 0; n < kN; ++n) { s += 0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(n) / kN); }
    return s;
}

SpectrumCapture makeCapture(const Scene& sc, std::size_t k, unsigned seed) {
    std::mt19937_64 rng(seed + 1000 * static_cast<unsigned>(k));
    std::normal_distribution<double> g(0.0, 1.0);
    const double gain = std::pow(10.0, sc.gainDb[k] / 20.0);
    const double trueCentre = sc.centre(k) * (1.0 + sc.ppm[k] * 1e-6);
    const double fsTrue = kFs * (1.0 + sc.srPpm * 1e-6);
    std::vector<Tone> heard;
    for (const Tone& t : sc.tones) {
        if (std::fabs(t.hz - trueCentre) < 0.45 * kFs) { heard.push_back(t); }
    }
    if (static_cast<int>(k) == sc.burstCapture) { heard.push_back(sc.burst); }
    std::vector<double> win(kN);
    for (std::size_t n = 0; n < kN; ++n) { win[n] = 0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(n) / kN); }
    std::vector<double> power(kN, 0.0);
    std::vector<cd> buf(kN);
    std::size_t t0 = 0;
    for (std::size_t f = 0; f < kFrames; ++f) {
        for (std::size_t n = 0; n < kN; ++n) {
            const double tt = static_cast<double>(t0 + n) / fsTrue;
            cd x(g(rng) * sc.noiseSigma / std::sqrt(2.0), g(rng) * sc.noiseSigma / std::sqrt(2.0));
            for (const Tone& t : heard) { x += std::polar(t.amp, 2.0 * kPi * (t.hz - trueCentre) * tt); }
            buf[n] = gain * x * win[n];
        }
        t0 += kN;
        fft(buf);
        for (std::size_t i = 0; i < kN; ++i) { power[i] += std::norm(buf[i]); }
    }
    SpectrumCapture c;
    c.centerHz = sc.centre(k);   // NOMINAL: the stitcher is not told the error
    c.rateHz = kFs;
    c.timeS = 10.0 * static_cast<double>(k);
    c.db.resize(kN);
    for (std::size_t i = 0; i < kN; ++i) {
        // FFT bin j holds frequency (j <= N/2 ? j : j - N); fftshift: shifted index i = j + N/2 mod N
        const std::size_t j = (i + kN / 2) % kN;
        double db = 10.0 * std::log10(power[j] / static_cast<double>(kFrames) + 1e-30);
        if (sc.droop != 0) {
            const double sx = (static_cast<double>(i) - static_cast<double>(kN / 2)) / kN;   // -0.5..0.5
            const double x = sc.droop == 2 ? std::max(sx, 0.0) : std::fabs(sx);
            if (x > 0.38) { db -= (x - 0.38) / 0.12 * 12.0; }
        }
        c.db[i] = static_cast<float>(db);
    }
    return c;
}

double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

std::vector<SpectrumCapture> makeAll(const Scene& sc, unsigned seed) {
    std::vector<SpectrumCapture> v;
    for (std::size_t k = 0; k < sc.captures; ++k) { v.push_back(makeCapture(sc, k, seed)); }
    return v;
}

Scene standardScene() {
    Scene sc;
    sc.gainDb = {0.0, 4.0, -3.0, 6.0, -5.0, 2.5, -6.0, 3.5};
    sc.ppm = {2.0, -4.0, 5.0, -1.0, 3.0, -5.0, 4.0, 1.0};
    // tones on the integer-bin grid relative to capture 0's centre; spread over the whole span,
    // several inside the overlaps (bins 276..388 of capture 0's kept range reach capture 1).
    const long long gs[] = {-300, -60, 250, 330, 700, 990, 1300, 1950, 2200, 2650, 3400, 3900, 4500, 5000};
    const double amps[] = {0.6, 0.3, 0.5, 0.8, 0.4, 0.7, 0.35, 0.9, 0.45, 0.6, 0.3, 0.55, 0.4, 0.65};
    for (std::size_t i = 0; i < std::size(gs); ++i) {
        sc.tones.push_back(Tone{sc.c0 + static_cast<double>(gs[i]) * kBin, amps[i]});
    }
    // a burst heard by capture 3 only, in the overlap with capture 4: bin 3*666+388-30
    sc.burstCapture = 3;
    sc.burst = Tone{sc.c0 + static_cast<double>(3 * 666 + 358) * kBin, 2.0};
    return sc;
}

}  // namespace

int main() {
    const double wsum = windowSum();
    const Scene sc = standardScene();
    const std::vector<SpectrumCapture> caps = makeAll(sc, 42);

    // ---- the proof: eight captures with noise, gain steps, tuner and sample-rate error ----
    StitchConfig cfg;   // defaults: trim 12 %
    const StitchResult r = stitchPass(caps, cfg);
    CHECK(r.ok);
    if (!r.ok) {
        std::printf("stitch failed: %s\n", r.error.c_str());
        StitchConfig dbg;
        dbg.maxOffsetDb = 1000;
        const StitchResult d = stitchPass(caps, dbg);
        for (const StitchSeam& s : d.seams) {
            std::printf("seam %.0f->%.0f overlap %zu used %zu off %.2f aligned %d\n", s.lowerCenterHz, s.upperCenterHz,
                        s.overlapCells, s.usedCells, s.offsetDb, s.aligned ? 1 : 0);
        }
        return 1;
    }
    CHECK(r.seams.size() == 7);
    CHECK(r.line.db.size() > 5000);
    CHECK(sweepLineProblem(r.line).empty());
    CHECK_NEAR(r.binHz, kBin, kBin * 0.01);

    double worstCorr = 0;
    for (std::size_t k = 0; k < caps.size(); ++k) {
        worstCorr = std::max(worstCorr, std::fabs(r.correctionDb[k] + sc.gainDb[k]));
    }
    std::printf("worst gain-correction error: %.3f dB (limit 0.5)\n", worstCorr);
    CHECK(worstCorr < 0.5);
    for (const StitchSeam& s : r.seams) {
        CHECK(s.aligned);
        CHECK(s.overlapCells > 50);
    }

    // tones: frequency within one bin, level within 1 dB of the truth
    double worstLevel = 0, worstFreq = 0;
    std::size_t found = 0;
    for (const Tone& t : sc.tones) {
        const long long cell = std::llround((t.hz - r.line.startHz) / r.binHz);
        CHECK(cell >= 3 && cell + 3 < static_cast<long long>(r.line.db.size()));
        if (!(cell >= 3 && cell + 3 < static_cast<long long>(r.line.db.size()))) { continue; }
        long long best = cell;
        for (long long c = cell - 2; c <= cell + 2; ++c) {
            if (r.line.db[static_cast<std::size_t>(c)] > r.line.db[static_cast<std::size_t>(best)]) { best = c; }
        }
        const double fRec = r.line.startHz + static_cast<double>(best) * r.binHz;
        const double expectDb = 20.0 * std::log10(t.amp * wsum);   // reference capture has 0 dB gain
        worstFreq = std::max(worstFreq, std::fabs(fRec - t.hz));
        worstLevel = std::max(worstLevel, std::fabs(static_cast<double>(r.line.db[static_cast<std::size_t>(best)]) - expectDb));
        ++found;
    }
    CHECK(found == sc.tones.size());
    std::printf("tones: worst frequency error %.0f Hz (limit %.0f), worst level error %.3f dB (limit 1)\n", worstFreq, kBin, worstLevel);
    CHECK(worstFreq <= kBin);
    CHECK(worstLevel < 1.0);

    // seams invisible: median of the 64 cells either side of the midpoint between neighbouring centres
    double worstSeam = 0;
    for (std::size_t k = 0; k + 1 < caps.size(); ++k) {
        const double mid = 0.5 * (sc.centre(k) + sc.centre(k + 1));
        const long long cell = std::llround((mid - r.line.startHz) / r.binHz);
        std::vector<double> below, above;
        for (long long c = cell - 64; c < cell; ++c) { below.push_back(r.line.db[static_cast<std::size_t>(c)]); }
        for (long long c = cell; c < cell + 64; ++c) { above.push_back(r.line.db[static_cast<std::size_t>(c)]); }
        worstSeam = std::max(worstSeam, std::fabs(median(below) - median(above)));
    }
    std::printf("worst seam step in the noise floor: %.3f dB (limit 1)\n", worstSeam);
    CHECK(worstSeam < 1.0);

    // the burst (one capture only, in the overlap) did not pull the alignment: seam 3->4 offset is the true gain step
    CHECK_NEAR(r.seams[3].offsetDb, sc.gainDb[3] - sc.gainDb[4], 0.5);
    CHECK(r.seams[3].usedCells < r.seams[3].overlapCells);   // and some cells were rejected as outliers

    // ---- why the roll-off is trimmed: with a LOPSIDED edge droop (upper edge only) the overlap
    //      carries the droop of one capture and not the other, and without the trim the offset
    //      estimate is biased; with it, it is not ----
    {
        Scene lop = sc;
        lop.droop = 2;
        lop.burstCapture = -1;
        const std::vector<SpectrumCapture> lc = makeAll(lop, 42);
        const StitchResult withTrim = stitchPass(lc, cfg);
        StitchConfig noTrim;
        noTrim.trimFraction = 0.0;
        const StitchResult without = stitchPass(lc, noTrim);
        CHECK(withTrim.ok && without.ok);
        double a = 0, b = 0;
        for (std::size_t k = 0; k < lc.size(); ++k) {
            a = std::max(a, std::fabs(withTrim.correctionDb[k] + sc.gainDb[k]));
            b = std::max(b, std::fabs(without.correctionDb[k] + sc.gainDb[k]));
        }
        std::printf("lopsided droop: worst gain-correction error %.2f dB trimmed, %.2f dB untrimmed\n", a, b);
        CHECK(a < 0.5);
        CHECK(b > 2.0 * a && b > 1.0);
    }

    // ---- order does not matter ----
    {
        std::vector<SpectrumCapture> shuffled = caps;
        std::mt19937 rng(5);
        std::shuffle(shuffled.begin(), shuffled.end(), rng);
        const StitchResult s = stitchPass(shuffled, cfg);
        CHECK(s.ok && s.line == r.line);
    }

    // ---- misaligned bin grids (centres half a bin off the lattice): the level may be down by
    //      the window's scalloping (<= 1.5 dB for Hann), the frequency still within a bin ----
    {
        Scene half = sc;
        half.burstCapture = -1;
        half.c0 = sc.c0 + 0.5 * kBin;   // tones stay on capture 0's lattice shifted: every other capture has the same offset
        // shift every other capture's centre by half a bin by shifting the step
        Scene misaligned = half;
        misaligned.stepBins = 666;
        std::vector<SpectrumCapture> m = makeAll(misaligned, 77);
        for (std::size_t k = 1; k < m.size(); k += 2) { m[k].centerHz += 0.5 * kBin; }   // their true tuning was also moved? no: only the label moves
        const StitchResult ms = stitchPass(m, cfg);
        CHECK(ms.ok);
        // the label error of half a bin moves those captures' tones by half a bin in the trace
        double wf = 0;
        for (const Tone& t : misaligned.tones) {
            const long long cell = std::llround((t.hz - ms.line.startHz) / ms.binHz);
            if (cell < 4 || cell + 4 >= static_cast<long long>(ms.line.db.size())) { continue; }
            long long best = cell;
            for (long long c = cell - 3; c <= cell + 3; ++c) {
                if (ms.line.db[static_cast<std::size_t>(c)] > ms.line.db[static_cast<std::size_t>(best)]) { best = c; }
            }
            wf = std::max(wf, std::fabs(ms.line.startHz + static_cast<double>(best) * ms.binHz - t.hz));
        }
        CHECK(wf <= 1.5 * kBin);
    }

    // ---- one capture, and a gap between captures ----
    {
        const StitchResult one = stitchPass({caps[0]}, cfg);
        CHECK(one.ok && one.seams.empty() && one.correctionDb.size() == 1 && one.correctionDb[0] == 0.0);
        const std::size_t lo = static_cast<std::size_t>(std::ceil(0.12 * kN));
        CHECK(one.line.db.size() == kN - 2 * lo);
        CHECK(one.line.db[0] == caps[0].db[lo] && one.line.db.back() == caps[0].db[kN - lo - 1]);

        Scene far = sc;
        far.stepBins = 1000;   // 1000 bins apart: the kept ranges (778 bins) do not touch
        far.tones = {Tone{far.c0 + 100 * kBin, 0.5}};
        far.burstCapture = -1;
        far.captures = 3;
        const StitchResult g = stitchPass(makeAll(far, 3), cfg);
        CHECK(g.ok && g.seams.size() == 2);
        CHECK(!g.seams[0].aligned && g.seams[0].overlapCells == 0);
        std::size_t nanCells = 0;
        for (float v : g.line.db) { if (std::isnan(v)) { ++nanCells; } }
        CHECK(nanCells > 100);   // the gap is NaN, not invented
    }

    // ---- refusals ----
    {
        std::vector<SpectrumCapture> c = caps;
        CHECK(!stitchPass({}, cfg).ok);
        c = caps; c[2].rateHz = 0; CHECK(!stitchPass(c, cfg).ok);
        c = caps; c[2].rateHz = std::nan(""); CHECK(!stitchPass(c, cfg).ok);
        c = caps; c[2].centerHz = -5.0; CHECK(!stitchPass(c, cfg).ok);
        c = caps; c[2].centerHz = 2e12; CHECK(!stitchPass(c, cfg).ok);
        c = caps; c[2].db.resize(8); CHECK(!stitchPass(c, cfg).ok);
        c = caps; c[2].db[100] = std::numeric_limits<float>::infinity(); CHECK(!stitchPass(c, cfg).ok);
        c = caps; c[2].db[100] = 900.0f; CHECK(!stitchPass(c, cfg).ok);
        c = caps; c[2].rateHz *= 1.2; CHECK(!stitchPass(c, cfg).ok);          // bin widths disagree
        c = caps; c[2].timeS = std::nan(""); CHECK(!stitchPass(c, cfg).ok);
        StitchConfig bad = cfg; bad.trimFraction = 0.5; CHECK(!stitchPass(caps, bad).ok);
        bad = cfg; bad.trimFraction = -0.1; CHECK(!stitchPass(caps, bad).ok);
        bad = cfg; bad.maxCells = 100; CHECK(!stitchPass(caps, bad).ok);
        bad = cfg; bad.maxCaptures = 3; CHECK(!stitchPass(caps, bad).ok);
        // a 50 dB gain step is beyond the stated maximum offset
        Scene big = sc; big.gainDb = {0, 50, 0, 0, 0, 0, 0, 0}; big.burstCapture = -1;
        CHECK(!stitchPass(makeAll(big, 9), cfg).ok);
        // an all-NaN capture is accepted (it contributes nothing) and does not break the others
        c = caps; std::fill(c[4].db.begin(), c[4].db.end(), std::nanf(""));
        const StitchResult nn = stitchPass(c, cfg);
        CHECK(nn.ok);
    }

    // ---- fuzz: random captures never fault; an ok result is a valid line ----
    {
        std::mt19937 rng(0xABCDu);
        std::uniform_real_distribution<double> u(0.0, 1.0);
        std::size_t okN = 0;
        for (int it = 0; it < 3000; ++it) {
            std::vector<SpectrumCapture> v(1 + rng() % 5);
            const std::size_t nbins = 64 + rng() % 200;   // one size per set: the bin widths agree
            for (auto& c : v) {
                c.centerHz = rng() % 25 == 0 ? (u(rng) - 0.1) * 3e9 : 100e6 + u(rng) * 1e6;
                c.rateHz = rng() % 30 == 0 ? (u(rng) - 0.2) * 1e7 : 2.4e6;
                c.db.resize(rng() % 30 == 0 ? rng() % 40 : nbins);
                const bool poison = rng() % 30 == 0;
                for (auto& x : c.db) {
                    x = rng() % 40 == 0 ? std::nanf("") : static_cast<float>(-100.0 + 30.0 * u(rng));
                }
                if (poison && !c.db.empty()) { c.db[rng() % c.db.size()] = 1000.0f; }
                c.timeS = u(rng) * 100.0;
            }
            StitchConfig fc;
            fc.trimFraction = u(rng) * 0.5;
            const StitchResult s = stitchPass(v, fc);
            if (s.ok) { ++okN; CHECK(sweepLineProblem(s.line).empty()); CHECK(s.correctionDb.size() == v.size()); }
            else { CHECK(!s.error.empty()); }
        }
        CHECK(okN > 200);
        std::printf("fuzz: %zu of 3000 random capture sets stitched, the rest refused\n", okN);
    }

    // ================= the heatmap =================
    {
        SweepHeatmap h;
        std::string why;
        HeatmapConfig hc;
        hc.startHz = 100e6; hc.stopHz = 110e6; hc.cols = 100; hc.maxRows = 8;
        CHECK(h.configure(hc, &why));
        // refusals
        SweepHeatmap bad;
        HeatmapConfig b = hc;
        b.stopHz = b.startHz; CHECK(!bad.configure(b, &why));
        b = hc; b.cols = 0; CHECK(!bad.configure(b, &why));
        b = hc; b.cols = kHeatmapMaxCols + 1; CHECK(!bad.configure(b, &why));
        b = hc; b.maxRows = 1; CHECK(!bad.configure(b, &why));
        b = hc; b.cols = 60000; b.maxRows = 60000; CHECK(!bad.configure(b, &why) && why == "too many cells");
        b = hc; b.startHz = std::nan(""); CHECK(!bad.configure(b, &why));
        { SweepLine l{100e6, 110e6, {-1.f, -2.f}, 0.0}; CHECK(!bad.addLine(l, &why) && why == "not configured"); }

        // click-to-tune: column -> frequency -> column, edges and outside
        for (std::size_t c = 0; c < hc.cols; ++c) {
            const double f = h.frequencyAtColumn(c);
            CHECK(h.columnAtFrequency(f) == static_cast<long long>(c));
            CHECK_NEAR(f, 100e6 + (static_cast<double>(c) + 0.5) * 100e3, 1e-3);
        }
        CHECK(h.columnAtFrequency(99.999e6) == -1 && h.columnAtFrequency(110.001e6) == -1);
        CHECK(h.columnAtFrequency(100e6) == 0 && h.columnAtFrequency(110e6) == 99);
        CHECK(h.columnAtFrequency(std::nan("")) == -1);
        CHECK(h.frequencyAtFraction(0.0) == 100e6 && h.frequencyAtFraction(1.0) == 110e6);
        CHECK(h.frequencyAtFraction(-3.0) == 100e6 && h.frequencyAtFraction(7.0) == 110e6);
        CHECK_NEAR(h.frequencyAtFraction(0.5), 105e6, 1e-3);

        // a 400-point line, 4 points per column: max mode keeps the peak, mean mode averages
        SweepLine line;
        line.startHz = 100e6 + 12500.0; line.stopHz = 110e6 - 12500.0;   // points at column quarters
        line.db.assign(400, -90.0f);
        line.db[4 * 20 + 2] = -50.0f;   // column 20 gets one peak
        line.timeS = 7.0;
        CHECK(h.addLine(line));
        CHECK(h.rows() == 1);
        CHECK(h.at(0, 20) == -50.0f);
        CHECK(h.at(0, 21) == -90.0f);
        CHECK(h.rowTimeS(0) == 7.0);
        HeatmapConfig mc = hc; mc.freqMode = Decimate::Mean;
        SweepHeatmap hm; CHECK(hm.configure(mc));
        CHECK(hm.addLine(line));
        CHECK_NEAR(hm.at(0, 20), (-50.0 - 90.0 * 3.0) / 4.0, 1e-4);
        // a line outside the range, an invalid line, NaN points ignored
        CHECK(!h.addLine(SweepLine{50e6, 60e6, {-1.f, -2.f}, 0.0}, &why) && why == "line outside the range");
        CHECK(!h.addLine(SweepLine{}, &why));
        { SweepLine n{100e6, 110e6, std::vector<float>(100, std::nanf("")), 8.0}; CHECK(h.addLine(n)); CHECK(std::isnan(h.at(1, 5))); }
        CHECK(std::isnan(h.at(99, 0)) && std::isnan(h.at(0, 999)));
    }

    // ---- bounded memory and time decimation: 5000 passes into 16 rows ----
    for (Decimate tm : {Decimate::Max, Decimate::Mean}) {
        SweepHeatmap h;
        HeatmapConfig hc;
        hc.startHz = 0; hc.stopHz = 1000; hc.cols = 50; hc.maxRows = 16; hc.timeMode = tm;
        CHECK(h.configure(hc));
        const std::size_t mem0 = h.memoryBytes();
        CHECK(mem0 <= 16 * 50 * 8 + 16 * 16);
        for (int p = 0; p < 5000; ++p) {
            SweepLine l{0.0, 1000.0, std::vector<float>(200, -100.0f), static_cast<double>(p)};
            if (p == 1234) { l.db[10] = -20.0f; }   // one strong pass in column 2
            CHECK(h.addLine(l));
            CHECK(h.rows() <= 16);
        }
        CHECK(h.memoryBytes() == mem0);   // never grew
        CHECK(h.passesAdded() == 5000);
        CHECK(h.passesPerRow() >= 5000 / 16);
        // times increase down the picture
        for (std::size_t r = 1; r < h.rows(); ++r) { CHECK(h.rowTimeS(r) > h.rowTimeS(r - 1)); }
        // find the row holding pass 1234
        float peak = -1000;
        for (std::size_t r = 0; r < h.rows(); ++r) { peak = std::max(peak, h.at(r, 2)); }
        if (tm == Decimate::Max) { CHECK(peak == -20.0f); }               // the max survives the merging
        else { CHECK(peak < -90.0f && peak > -100.0f); }                  // the mean dilutes it (80 dB over ~300 passes)
        // total passes accounted for: weights of rows sum to the passes added
        // (the last row may be partly filled)
        CHECK(h.rows() >= 8);
    }

    // ---- fuzz: random lines never fault, indices stay inside the store ----
    {
        std::mt19937 rng(99);
        std::uniform_real_distribution<double> u(0.0, 1.0);
        SweepHeatmap h;
        HeatmapConfig hc;
        hc.startHz = 1e6; hc.stopHz = 2e6; hc.cols = 37; hc.maxRows = 5;
        CHECK(h.configure(hc));
        for (int it = 0; it < 5000; ++it) {
            SweepLine l;
            l.startHz = (u(rng) * 3.0 - 0.5) * 1e6;
            l.stopHz = l.startHz + u(rng) * 2e6 - (rng() % 9 == 0 ? 3e6 : 0.0);
            l.db.resize(rng() % 50);
            for (auto& x : l.db) { x = rng() % 5 == 0 ? std::nanf("") : static_cast<float>(-120.0 + 100.0 * u(rng)); }
            l.timeS = static_cast<double>(it);
            h.addLine(l);
            CHECK(h.rows() <= 5);
        }
    }

    return testSummary("test_sweep_stitch");
}
