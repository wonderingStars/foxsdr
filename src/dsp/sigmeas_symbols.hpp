// sigmeas_symbols.hpp - SYMBOL PERIOD AND RATE, SYMBOL EXTRACTION, FSK ANALYSIS and the OOK/ASK
// PULSE-WIDTH SLICER on a selected, isolated channel. Rows (REMAINING-GAPS.md, Signal analysis
// and identification):
//   - "cursor symbol-period and symbol-rate measurement with symbol extraction to a file"
//   - "FSK analysis (shift, mark/space, baud) and an ASK/OOK pulse-width slicer"
//
// METHODS (textbook; no other program's source was read)
//  - The cursor model: two cursors that bracket `count` whole symbols give period = (t1-t0)/count
//    and rate = count/(t1-t0). `refineCursorPeriod` then snaps that guess to the actual edges inside
//    the cursors by least squares, so a hand-placed cursor that is a few percent off still reads
//    the true rate.
//  - Bit period from run lengths. A two-level signal is a sequence of runs, each a whole number of
//    symbol periods. For a candidate period T every run r_i is assigned n_i = round(r_i / T) and the
//    period refitted by least squares, T = sum(n_i r_i) / sum(n_i^2); the candidates are the run
//    clusters divided by 1..8, tried from the LARGEST down and the first whose residual is within
//    `tolerance` of T is taken (a smaller T always fits as well, so the largest consistent T is the
//    symbol period; the ambiguity this leaves - every run the same length - is stated in the
//    result as `ambiguous`: every run the same length, as in an alternating 0101 pattern). Edge times are the linear-interpolated crossings of the decision
//    level, so the answer is not quantised to a sample.
//  - FSK: the instantaneous frequency f[n] = arg(x[n] conj(x[n-1])) rate / 2 pi, smoothed to a
//    quarter of the symbol period; Otsu's two-class threshold (Otsu 1979) gives the decision level;
//    mark and space are the medians of the two classes, shift = mark - space; edges, runs and the
//    period as above, then symbols are read at their centres. A second pass re-smooths with the
//    measured period.
//  - OOK/ASK: the same on the envelope |x|; pulse widths are measured between the interpolated
//    crossings of the Otsu level; short and long pulse widths are the two clusters of the width
//    list when they differ by more than 1.5x (pulse-width keying).
//
// "Symbol extraction to a file": `extractSymbols` returns the value of the track at each symbol
// centre (mean of the middle half of the symbol); `writeSymbolFile` writes them as raw float32,
// text values or a 0/1 string. The file is written only when the caller asks (the host's panel);
// the block keeps nothing.
//
// ACCURACY, pinned in tests/test_sigmeas_symbols.cpp: baud within 0.5 % on FSK and OOK with
// +-8 % edge jitter, 20 dB SNR, a carrier offset and a 300 ppm clock error; FSK shift within 1 %.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_DSP_SIGMEAS_SYMBOLS_HPP
#define CASCADE_DSP_SIGMEAS_SYMBOLS_HPP

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "dsp/sigmeas_common.hpp"

namespace cascade::sigmeas {

// ---- the cursor model --------------------------------------------------------------------------
struct CursorPeriod {
    bool valid = false;
    const char* reason = why::kEmpty;
    double periodS = kNaN;
    double rateHz = kNaN;
};

inline CursorPeriod periodFromCursors(double t0S, double t1S, double symbolCount) {
    CursorPeriod r;
    if (!std::isfinite(t0S) || !std::isfinite(t1S) || !std::isfinite(symbolCount) ||
        !(symbolCount >= 1.0) || !(t1S > t0S)) {
        r.reason = why::kBadParam;
        return r;
    }
    r.periodS = (t1S - t0S) / symbolCount;
    r.rateHz = symbolCount / (t1S - t0S);
    r.valid = true;
    r.reason = why::kOk;
    return r;
}

// ---- bit period from run lengths ---------------------------------------------------------------
struct PeriodFit {
    bool valid = false;
    const char* reason = why::kEmpty;
    double period = kNaN;       // in the units of the runs
    double relResidual = kNaN;  // rms residual over the period
    bool ambiguous = false;     // every run is the same length: any whole fraction of it fits too
    std::vector<int> units;     // n_i per run
};

namespace detail {

inline PeriodFit fitPeriodWith(const std::vector<double>& runs, double T0, double tol) {
    PeriodFit f;
    const std::size_t n = runs.size();
    f.units.assign(n, 1);
    double T = T0;
    for (int it = 0; it < 3; ++it) {
        double sn = 0.0, snr = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            const int u = std::max(1, static_cast<int>(std::lround(runs[i] / T)));
            f.units[i] = u;
            sn += static_cast<double>(u) * u;
            snr += static_cast<double>(u) * runs[i];
        }
        if (!(sn > 0.0)) { return f; }
        T = snr / sn;
    }
    double ss = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double d = runs[i] - static_cast<double>(f.units[i]) * T;
        ss += d * d;
    }
    f.period = T;
    f.relResidual = std::sqrt(ss / static_cast<double>(n)) / T;
    f.valid = f.relResidual <= tol;
    return f;
}

}  // namespace detail

// runs: lengths in any one unit (samples, seconds). minPeriod: the smallest period to consider in
// that unit. Needs at least 3 runs.
inline PeriodFit fitSymbolPeriod(const std::vector<double>& runs, double minPeriod = 2.0,
                                 double tolerance = 0.15) {
    PeriodFit out;
    if (runs.size() < 3 || !allFinite(runs.data(), runs.size()) || !(minPeriod > 0.0) ||
        !(tolerance > 0.0)) {
        out.reason = runs.size() < 3 ? why::kTooShort : why::kBadParam;
        return out;
    }
    for (double r : runs) {
        if (!(r > 0.0)) { out.reason = why::kBadParam; return out; }
    }
    // cluster the run lengths (neighbours within 15 %)
    std::vector<double> sorted = runs;
    std::sort(sorted.begin(), sorted.end());
    std::vector<double> centres;
    {
        double sum = sorted[0];
        int cnt = 1;
        for (std::size_t i = 1; i < sorted.size(); ++i) {
            const double mean = sum / cnt;
            if (sorted[i] <= mean * 1.15) { sum += sorted[i]; ++cnt; }
            else { centres.push_back(mean); sum = sorted[i]; cnt = 1; }
        }
        centres.push_back(sum / cnt);
    }
    std::vector<double> cands;
    for (double c : centres) {
        for (int k = 1; k <= 8; ++k) {
            const double T = c / k;
            if (T >= minPeriod) { cands.push_back(T); }
        }
    }
    std::sort(cands.begin(), cands.end(), [](double a, double b) { return a > b; });
    for (double T0 : cands) {
        PeriodFit f = detail::fitPeriodWith(runs, T0, tolerance);
        if (f.valid && f.period >= minPeriod) {
            // ambiguity: a pattern whose runs are all one length (0101..) fits 1/2, 1/3 ... of it
            bool same = true;
            for (int u : f.units) { if (u != f.units[0]) { same = false; break; } }
            f.ambiguous = same;
            f.reason = why::kOk;
            return f;
        }
    }
    out.reason = why::kNoStructure;
    return out;
}

// Snaps a guessed period (hand-placed cursors) to the edges: the same least squares, one candidate.
inline PeriodFit refineCursorPeriod(const std::vector<double>& runs, double guess,
                                    double tolerance = 0.25) {
    PeriodFit out;
    if (runs.empty() || !(guess > 0.0) || !std::isfinite(guess) || !allFinite(runs.data(), runs.size())) {
        out.reason = why::kBadParam;
        return out;
    }
    PeriodFit f = detail::fitPeriodWith(runs, guess, tolerance);
    f.reason = f.valid ? why::kOk : why::kNoStructure;
    return f;
}

// Least-squares fit of the edge TIMES to a grid, e_j = t0 + m_j T with m_j whole numbers. Much
// tighter than the run-length fit (every edge constrains T over the whole span, not over one run):
// the error of T falls with the span length cubed-root of nothing - it is edge jitter over the span
// in symbols. `period0` is the run-length estimate.
struct GridFit {
    bool valid = false;
    double period = kNaN;
    double t0 = kNaN;        // position of the grid point nearest the first edge
    double rmsUi = kNaN;     // rms edge residual in periods
};

inline GridFit fitEdgeGrid(const std::vector<double>& edges, double period0) {
    GridFit g;
    if (edges.size() < 3 || !(period0 > 0.0)) { return g; }
    double T = period0;
    double t0 = edges.front();
    std::vector<double> m(edges.size(), 0.0);
    for (int it = 0; it < 4; ++it) {
        // whole periods accumulate from edge to edge: a period that is a little off cannot lose
        // count over a long span, because each step is a few periods at most
        m[0] = 0.0;
        for (std::size_t j = 1; j < edges.size(); ++j) {
            m[j] = m[j - 1] + std::max(1.0, std::round((edges[j] - edges[j - 1]) / T));
        }
        double sm = 0.0, se = 0.0, smm = 0.0, sme = 0.0;
        const double cnt = static_cast<double>(edges.size());
        for (std::size_t j = 0; j < edges.size(); ++j) {
            sm += m[j]; se += edges[j]; smm += m[j] * m[j]; sme += m[j] * edges[j];
        }
        const double den = cnt * smm - sm * sm;
        if (!(den > 0.0)) { return g; }
        T = (cnt * sme - sm * se) / den;
        t0 = (se - T * sm) / cnt;
        if (!(T > 0.0)) { return g; }
    }
    double ss = 0.0;
    for (std::size_t j = 0; j < edges.size(); ++j) {
        const double d = edges[j] - (t0 + m[j] * T);
        ss += d * d;
    }
    g.period = T;
    g.t0 = t0;
    g.rmsUi = std::sqrt(ss / static_cast<double>(edges.size())) / T;
    g.valid = true;
    return g;
}

// ---- a track at a fractional position, decision-level edges -----------------------------------------
inline double valueAt(const std::vector<double>& v, double pos) {
    if (v.empty()) { return 0.0; }
    if (pos <= 0.0) { return v.front(); }
    const double last = static_cast<double>(v.size() - 1);
    if (pos >= last) { return v.back(); }
    const std::size_t i = static_cast<std::size_t>(pos);
    const double fr = pos - static_cast<double>(i);
    return v[i] + (v[i + 1] - v[i]) * fr;
}

// Edge positions (fractional samples) where the track crosses `thr`, with a hysteresis of +-hyst
// so noise on a flat part does not chatter. rising[j] = direction of edge j.
inline void findEdges(const std::vector<double>& v, double thr, double hyst, std::vector<double>& edges,
                      std::vector<char>& rising) {
    edges.clear();
    rising.clear();
    if (v.size() < 2) { return; }
    int state = v[0] > thr ? 1 : 0;
    double lastMid = -1.0;
    for (std::size_t i = 1; i < v.size(); ++i) {
        if ((v[i - 1] <= thr) != (v[i] <= thr)) { lastMid = crossingPos(v[i - 1], v[i], thr, i); }
        if (state == 0 && v[i] > thr + hyst && lastMid >= 0.0) {
            edges.push_back(lastMid);
            rising.push_back(1);
            state = 1;
        } else if (state == 1 && v[i] < thr - hyst && lastMid >= 0.0) {
            edges.push_back(lastMid);
            rising.push_back(0);
            state = 0;
        }
    }
}

inline std::vector<double> runsFromEdges(const std::vector<double>& edges) {
    std::vector<double> r;
    for (std::size_t i = 1; i < edges.size(); ++i) { r.push_back(edges[i] - edges[i - 1]); }
    return r;
}

// ---- symbol extraction ---------------------------------------------------------------------------------
// The track's value at each symbol centre (mean of the middle half of the symbol), symbols starting
// at `firstEdge` (samples) with `period` (samples). At most maxCount values.
inline std::vector<float> extractSymbols(const std::vector<double>& track, double firstEdge,
                                         double period, std::size_t maxCount = 1u << 20) {
    std::vector<float> out;
    if (track.empty() || !(period >= 1.0) || !std::isfinite(period) || !std::isfinite(firstEdge)) {
        return out;
    }
    const double end = static_cast<double>(track.size());
    for (std::size_t k = 0; out.size() < maxCount; ++k) {
        const double a = firstEdge + (static_cast<double>(k) + 0.25) * period;
        const double b = firstEdge + (static_cast<double>(k) + 0.75) * period;
        if (b > end || a < 0.0) { if (a < 0.0) { continue; } break; }
        const std::size_t i0 = static_cast<std::size_t>(std::ceil(a));
        const std::size_t i1 = static_cast<std::size_t>(std::floor(b));
        double acc = 0.0;
        std::size_t cnt = 0;
        for (std::size_t i = i0; i <= i1 && i < track.size(); ++i) { acc += track[i]; ++cnt; }
        out.push_back(cnt > 0 ? static_cast<float>(acc / static_cast<double>(cnt))
                              : static_cast<float>(valueAt(track, 0.5 * (a + b))));
    }
    return out;
}

enum class SymbolFileFormat { Float32LE, Text, BitString };

// Writes the extracted values. BitString slices at `threshold`. Returns false if the file could not
// be written (never throws).
inline bool writeSymbolFile(const std::string& path, const std::vector<float>& values,
                            SymbolFileFormat fmt, double threshold = 0.0) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) { return false; }
    switch (fmt) {
        case SymbolFileFormat::Float32LE:
            // host byte order is little-endian on every platform FoxSDR runs on
            f.write(reinterpret_cast<const char*>(values.data()),
                    static_cast<std::streamsize>(values.size() * sizeof(float)));
            break;
        case SymbolFileFormat::Text: {
            char buf[48];
            for (float v : values) {
                const int n = std::snprintf(buf, sizeof buf, "%.6g\n", static_cast<double>(v));
                if (n > 0) { f.write(buf, n); }
            }
            break;
        }
        case SymbolFileFormat::BitString: {
            std::string s;
            s.reserve(values.size() + values.size() / 64 + 1);
            std::size_t k = 0;
            for (float v : values) {
                s.push_back(static_cast<double>(v) > threshold ? '1' : '0');
                if (++k % 64 == 0) { s.push_back('\n'); }
            }
            s.push_back('\n');
            f.write(s.data(), static_cast<std::streamsize>(s.size()));
            break;
        }
    }
    f.flush();
    return static_cast<bool>(f);
}

// ---- FSK analysis ------------------------------------------------------------------------------------------
struct SymbolParams {
    double minBaudHz = 0.0;       // 0 = no floor: the period is at least 2 samples
    double maxBaudHz = 0.0;       // 0 = rate / 6 (at least 6 samples per symbol for the slicer)
    double tolerance = 0.15;      // run-fit residual over the period
    double minSeparation = 0.55;  // Otsu between-class share for "two levels"
    std::size_t maxBits = 4096;   // symbols returned
    std::size_t maxPulses = 256;  // OOK pulses returned
};

struct FskResult {
    bool valid = false;
    const char* reason = why::kEmpty;
    double markHz = kNaN;       // higher tone
    double spaceHz = kNaN;      // lower tone
    double shiftHz = kNaN;
    double centreHz = kNaN;     // (mark + space) / 2, from the span's centre
    double baudHz = kNaN;
    double periodSamples = kNaN;
    double jitterUi = kNaN;     // rms edge deviation from the fitted grid, in symbol periods
    double separation = 0.0;
    std::size_t edges = 0;
    bool ambiguous = false;
    double firstEdgeSample = kNaN;
    std::vector<std::uint8_t> bits;   // 1 = mark, from the first edge on
};

struct Pulse {
    double startS = 0.0;
    double widthS = 0.0;
    double gapAfterS = 0.0;  // to the next pulse (0 for the last)
};

struct OokResult {
    bool valid = false;
    const char* reason = why::kEmpty;
    double onLevel = kNaN;
    double offLevel = kNaN;
    double threshold = kNaN;
    double baudHz = kNaN;           // unit element of the on/off pattern
    double periodS = kNaN;
    double jitterUi = kNaN;
    double separation = 0.0;
    std::size_t pulseCount = 0;     // all pulses in the span
    bool pulseWidthKeyed = false;   // two distinct pulse widths
    double shortWidthS = kNaN;
    double longWidthS = kNaN;
    double repeatPeriodS = kNaN;    // median start-to-start of the pulses (0 .. NaN when < 3)
    bool ambiguous = false;
    std::vector<Pulse> pulses;      // first maxPulses
    std::vector<std::uint8_t> bits; // on = 1, sampled on the fitted grid from the first edge
};

namespace detail {

inline std::size_t oddAtLeast(double v, std::size_t lo) {
    std::size_t n = static_cast<std::size_t>(std::max(v, 1.0));
    n = std::max(n, lo);
    return n | 1u;
}


// The slicer shared by FSK (on the frequency track) and OOK (on the envelope): smooths the raw
// track over a ladder of lengths (3, 7, 15 ... samples) until the Otsu split is clean and the
// run-length fit succeeds with the period at least three smoothing lengths long, then smooths again
// at a quarter of the found period, re-splits at the midpoint of the two class medians and refits.
struct Sliced {
    bool ok = false;
    const char* reason = why::kNoStructure;
    std::vector<double> track;   // the smoothed track used
    std::vector<double> edges;   // fractional samples
    std::vector<char> rising;
    double thr = 0.0, hi = 0.0, lo = 0.0, sep = 0.0;
    PeriodFit fit;
};

inline bool splitLevels(const std::vector<double>& v, Sliced& s) {
    double sep = 0.0;
    const double otsu = otsuThreshold(v.data(), v.size(), &sep);
    std::vector<double> up, dn;
    for (double d : v) { (d > otsu ? up : dn).push_back(d); }
    s.sep = sep;
    if (up.size() < 8 || dn.size() < 8) { return false; }
    s.hi = percentileInPlace(up, 0.5);
    s.lo = percentileInPlace(dn, 0.5);
    s.thr = 0.5 * (s.hi + s.lo);
    return s.hi > s.lo;
}

inline Sliced sliceTwoLevel(const std::vector<double>& raw, double minPeriod, const SymbolParams& prm) {
    Sliced out;
    std::vector<double> scratch;
    const std::size_t maxL = std::max<std::size_t>(3, raw.size() / 32);
    std::size_t L = 3;
    bool first = true;
    for (;; L = L * 2 + 1) {
        Sliced s;
        s.track = raw;
        movingAverage(s.track, L, scratch);
        if (splitLevels(s.track, s) && s.sep >= prm.minSeparation) {
            findEdges(s.track, s.thr, 0.2 * (s.hi - s.lo), s.edges, s.rising);
            PeriodFit f = fitSymbolPeriod(runsFromEdges(s.edges), std::max(minPeriod, 3.0 * static_cast<double>(L)),
                                          prm.tolerance);
            if (f.valid) {
                s.fit = f;
                s.ok = true;
                out = s;
                break;
            }
            out.reason = f.reason;
        } else if (first) {
            out.reason = why::kNoStructure;
        }
        first = false;
        out.sep = std::max(out.sep, s.sep);
        if (L >= maxL) { break; }
    }
    if (!out.ok) { return out; }
    // refine at a quarter of the period
    Sliced s;
    s.track = raw;
    movingAverage(s.track, detail::oddAtLeast(out.fit.period / 4.0, 3), scratch);
    if (splitLevels(s.track, s)) {
        findEdges(s.track, s.thr, 0.2 * (s.hi - s.lo), s.edges, s.rising);
        const PeriodFit f = fitSymbolPeriod(runsFromEdges(s.edges), minPeriod, prm.tolerance);
        if (f.valid) {
            s.fit = f;
            s.ok = true;
            return s;
        }
    }
    return out;
}

}  // namespace detail

inline FskResult analyseFsk(const Cf* x, std::size_t n, double rate, const SymbolParams& prm = {}) {
    FskResult r;
    if (const char* bad = checkIq(x, n, rate, 256)) { r.reason = bad; return r; }
    if (!(prm.tolerance > 0.0) || !(prm.minSeparation > 0.0 && prm.minSeparation < 1.0) ||
        !(prm.minBaudHz >= 0.0) || !(prm.maxBaudHz >= 0.0) || !std::isfinite(prm.minBaudHz) ||
        !std::isfinite(prm.maxBaudHz)) {
        r.reason = why::kBadParam;
        return r;
    }
    const double maxBaud = prm.maxBaudHz > 0.0 ? prm.maxBaudHz : rate / 6.0;
    const double minPeriod = std::max(2.0, rate / maxBaud);

    std::vector<double> mag(n), raw(n);
    for (std::size_t i = 0; i < n; ++i) { mag[i] = std::abs(std::complex<double>(x[i])); }
    {
        std::vector<double> tmp = mag;
        const double med = percentileInPlace(tmp, 0.5);
        if (!(med > 1e-9)) { r.reason = why::kNoSignal; return r; }
        instantaneousFrequency(x, n, rate, raw.data());
        const double gate = 0.1 * med;
        for (std::size_t i = 1; i < n; ++i) {
            if (mag[i] < gate || mag[i - 1] < gate) { raw[i] = raw[i - 1]; }
        }
    }
    const detail::Sliced sl = detail::sliceTwoLevel(raw, minPeriod, prm);
    if (!sl.ok) { r.reason = sl.reason; r.separation = sl.sep; return r; }
    const std::vector<double>& f2 = sl.track;
    const std::vector<double>& edges = sl.edges;
    const PeriodFit& fit = sl.fit;
    const double thr = sl.thr, hi = sl.hi, lo = sl.lo, sep = sl.sep;
    r.separation = sep;
    r.markHz = hi;
    r.spaceHz = lo;
    r.shiftHz = hi - lo;
    r.centreHz = 0.5 * (hi + lo);
    const GridFit grid = fitEdgeGrid(edges, fit.period);
    const double period = grid.valid ? grid.period : fit.period;
    r.periodSamples = period;
    r.baudHz = rate / period;
    r.ambiguous = fit.ambiguous;
    r.edges = edges.size();
    const double firstEdge = grid.valid ? grid.t0 : edges.front();
    r.jitterUi = grid.valid ? grid.rmsUi : kNaN;
    r.firstEdgeSample = firstEdge;
    // Tone levels from the symbol-centre means of the RAW track (noise averages out inside a symbol;
    // class medians of the smoothed track read the shift low in noise because the classes are cut
    // by the threshold). Bits come from the same values.
    const std::vector<float> sym = extractSymbols(raw, firstEdge, period);
    {
        double sh = 0.0, sl2 = 0.0;
        std::size_t nh = 0, nl = 0;
        for (float v : sym) { if (static_cast<double>(v) > thr) { sh += v; ++nh; } else { sl2 += v; ++nl; } }
        if (nh > 0 && nl > 0) {
            r.markHz = sh / static_cast<double>(nh);
            r.spaceHz = sl2 / static_cast<double>(nl);
            r.shiftHz = r.markHz - r.spaceHz;
            r.centreHz = 0.5 * (r.markHz + r.spaceHz);
        }
    }
    const std::size_t nb = std::min<std::size_t>(sym.size(), prm.maxBits);
    r.bits.reserve(nb);
    for (std::size_t k = 0; k < nb; ++k) { r.bits.push_back(static_cast<double>(sym[k]) > thr ? 1 : 0); }
    (void)f2;
    if (prm.minBaudHz > 0.0 && r.baudHz < prm.minBaudHz) {
        r.reason = why::kNoStructure;
        return r;
    }
    r.valid = true;
    r.reason = why::kOk;
    return r;
}

inline OokResult sliceOok(const Cf* x, std::size_t n, double rate, const SymbolParams& prm = {}) {
    OokResult r;
    if (const char* bad = checkIq(x, n, rate, 256)) { r.reason = bad; return r; }
    if (!(prm.tolerance > 0.0) || !(prm.minSeparation > 0.0 && prm.minSeparation < 1.0) ||
        !(prm.minBaudHz >= 0.0) || !(prm.maxBaudHz >= 0.0) || !std::isfinite(prm.minBaudHz) ||
        !std::isfinite(prm.maxBaudHz)) {
        r.reason = why::kBadParam;
        return r;
    }
    const double maxBaud = prm.maxBaudHz > 0.0 ? prm.maxBaudHz : rate / 6.0;
    const double minPeriod = std::max(2.0, rate / maxBaud);

    std::vector<double> raw(n);
    for (std::size_t i = 0; i < n; ++i) { raw[i] = std::abs(std::complex<double>(x[i])); }
    const detail::Sliced sl = detail::sliceTwoLevel(raw, minPeriod, prm);
    if (!sl.ok) { r.reason = sl.reason; r.separation = sl.sep; return r; }
    const std::vector<double>& e = sl.track;
    const std::vector<double>& edges = sl.edges;
    const std::vector<char>& rising = sl.rising;
    const PeriodFit& fit = sl.fit;
    const double thr = sl.thr, hi = sl.hi, lo = sl.lo, sep = sl.sep;
    r.separation = sep;
    r.onLevel = hi;
    r.offLevel = lo;
    r.threshold = thr;
    const GridFit grid = fitEdgeGrid(edges, fit.period);
    const double period = grid.valid ? grid.period : fit.period;
    r.periodS = period / rate;
    r.baudHz = rate / period;
    r.ambiguous = fit.ambiguous;
    const double firstEdge = grid.valid ? grid.t0 : edges.front();
    r.jitterUi = grid.valid ? grid.rmsUi : kNaN;

    // pulses: a rising edge followed by the next falling edge
    std::vector<double> widths, starts;
    for (std::size_t j = 0; j + 1 < edges.size(); ++j) {
        if (rising[j] && !rising[j + 1]) {
            widths.push_back((edges[j + 1] - edges[j]) / rate);
            starts.push_back(edges[j] / rate);
        }
    }
    r.pulseCount = widths.size();
    for (std::size_t k = 0; k < widths.size() && r.pulses.size() < prm.maxPulses; ++k) {
        Pulse p;
        p.startS = starts[k];
        p.widthS = widths[k];
        p.gapAfterS = k + 1 < widths.size() ? starts[k + 1] - starts[k] - widths[k] : 0.0;
        r.pulses.push_back(p);
    }
    if (widths.size() >= 3) {
        std::vector<double> gaps;
        for (std::size_t k = 0; k + 1 < starts.size(); ++k) { gaps.push_back(starts[k + 1] - starts[k]); }
        r.repeatPeriodS = percentileInPlace(gaps, 0.5);
    }
    // two-cluster test on the widths
    if (widths.size() >= 4) {
        double sepW = 0.0;
        const double tw = otsuThreshold(widths.data(), widths.size(), &sepW);
        std::vector<double> s, l;
        for (double w : widths) { (w > tw ? l : s).push_back(w); }
        if (!s.empty() && !l.empty()) {
            const double ms = percentileInPlace(s, 0.5), ml = percentileInPlace(l, 0.5);
            if (ml > 1.5 * ms && sepW > 0.7) {
                r.pulseWidthKeyed = true;
                r.shortWidthS = ms;
                r.longWidthS = ml;
            }
        }
    }
    const std::vector<float> sym = extractSymbols(e, firstEdge, period, prm.maxBits);
    r.bits.reserve(sym.size());
    for (float v : sym) { r.bits.push_back(static_cast<double>(v) > thr ? 1 : 0); }
    r.valid = true;
    r.reason = why::kOk;
    return r;
}

}  // namespace cascade::sigmeas

#endif  // CASCADE_DSP_SIGMEAS_SYMBOLS_HPP
