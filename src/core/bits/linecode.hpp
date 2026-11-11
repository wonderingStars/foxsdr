// BIT-VIEW LIBRARY, part 2: line-code decoders (and the matching encoders the tests use).
//
// A "chip" stream is the signal sliced to levels at the chip clock: one chip per bit for the NRZ
// family, two chips (half-bits) per bit for Manchester, differential Manchester, bi-phase and
// Miller. Pulse-width and pulse-position codes work on run lengths instead (see PulseRun).
// Each decoder returns the bits and the number of CODE VIOLATIONS - a pair of equal chips in a
// Manchester stream, a missing boundary transition in bi-phase, and so on - which is what the
// polarity/phase search ranks on.
//
// Definitions are the textbook ones (stated at each enumerator).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

#include "core/bits/bits.hpp"

namespace cascade::bits {

enum class LineCode : uint8_t {
    NrzL,              // level is the bit
    NrzM,              // a transition marks a 1 (no transition = 0)
    NrzS,              // a transition marks a 0 (= NRZI as USB/HDLC use the word)
    ManchesterIeee,    // 2 chips/bit, chips 01 = 1, 10 = 0 (IEEE 802.3 reading)
    ManchesterThomas,  // 2 chips/bit, chips 10 = 1, 01 = 0 (G. E. Thomas reading)
    DiffManchester,    // 2 chips/bit, mid-bit transition always; NO transition at the bit start = 1
    DiffManchesterInv, // same, a transition at the bit start = 1
    BiphaseMark,       // 2 chips/bit, transition at every bit start, an extra mid-bit one for a 1
    BiphaseSpace,      // 2 chips/bit, transition at every bit start, an extra mid-bit one for a 0
    Miller,            // 2 chips/bit, delay modulation: 1 = mid-bit transition, 0 after 0 = boundary transition
};

inline constexpr size_t kLineCodeCount = 10;

inline const char* lineCodeName(LineCode c) {
    switch (c) {
        case LineCode::NrzL: return "NRZ-L";
        case LineCode::NrzM: return "NRZ-M";
        case LineCode::NrzS: return "NRZ-S/NRZI";
        case LineCode::ManchesterIeee: return "Manchester (IEEE 802.3)";
        case LineCode::ManchesterThomas: return "Manchester (G.E. Thomas)";
        case LineCode::DiffManchester: return "Differential Manchester";
        case LineCode::DiffManchesterInv: return "Differential Manchester (inverted)";
        case LineCode::BiphaseMark: return "Bi-phase mark";
        case LineCode::BiphaseSpace: return "Bi-phase space";
        case LineCode::Miller: return "Miller";
    }
    return "?";
}

inline size_t chipsPerBit(LineCode c) { return c <= LineCode::NrzS ? 1 : 2; }

struct Decoded {
    Bits bits;
    size_t violations = 0;            // code violations seen
    size_t units = 0;                 // bits examined (violation rate = violations / units)
    std::vector<size_t> violationAt;  // first 64 violation positions (bit index)
};

namespace detail {
inline void violate(Decoded& d, size_t at) {
    ++d.violations;
    if (d.violationAt.size() < 64) d.violationAt.push_back(at);
}
}  // namespace detail

// `chips` is read from chip index `phase` (0 or 1 for the two-chip codes: the clock-phase
// choice); `prevLevel` is the level before the first chip, for the codes that depend on it.
inline Decoded decodeLineCode(const Bits& chips, LineCode code, size_t phase = 0, unsigned prevLevel = 0) {
    Decoded d;
    const size_t cpb = chipsPerBit(code);
    if (phase >= chips.size()) return d;
    const size_t n = (chips.size() - phase) / cpb;
    d.bits.reserve(n);
    d.units = n;
    unsigned prev = prevLevel & 1u;  // level of the previous chip
    int prevBit = -1;                // previous decoded bit (Miller)
    for (size_t i = 0; i < n; ++i) {
        const unsigned c0 = chips[phase + i * cpb] & 1u;
        const unsigned c1 = cpb == 2 ? (chips[phase + i * cpb + 1] & 1u) : 0u;
        unsigned bit = 0;
        switch (code) {
            case LineCode::NrzL: bit = c0; break;
            case LineCode::NrzM: bit = c0 != prev; break;
            case LineCode::NrzS: bit = c0 == prev; break;
            case LineCode::ManchesterIeee:
                if (c0 == c1) detail::violate(d, i);
                bit = c1;  // 01 -> 1, 10 -> 0
                break;
            case LineCode::ManchesterThomas:
                if (c0 == c1) detail::violate(d, i);
                bit = c0;  // 10 -> 1, 01 -> 0
                break;
            case LineCode::DiffManchester:
            case LineCode::DiffManchesterInv: {
                if (c0 == c1) detail::violate(d, i);  // the mid-bit transition is the clock
                const bool startTransition = c0 != prev;
                bit = (code == LineCode::DiffManchester) ? !startTransition : startTransition;
                break;
            }
            case LineCode::BiphaseMark:
                if (i > 0 && c0 == prev) detail::violate(d, i);
                bit = c0 != c1;
                break;
            case LineCode::BiphaseSpace:
                if (i > 0 && c0 == prev) detail::violate(d, i);
                bit = c0 == c1;
                break;
            case LineCode::Miller: {
                bit = c0 != c1;  // a mid-bit transition is a 1
                if (i > 0) {
                    const bool boundaryTransition = c0 != prev;
                    const bool mustTransition = (prevBit == 0 && bit == 0);
                    if (boundaryTransition != mustTransition) detail::violate(d, i);
                }
                break;
            }
        }
        d.bits.push_back(static_cast<uint8_t>(bit));
        prev = cpb == 2 ? c1 : c0;
        prevBit = static_cast<int>(bit);
    }
    return d;
}

// The encoders: bits -> chips, the inverse of decodeLineCode started from `prevLevel`.
inline Bits encodeLineCode(const Bits& data, LineCode code, unsigned prevLevel = 0) {
    Bits chips;
    unsigned prev = prevLevel & 1u;
    int prevBit = -1;
    for (uint8_t b0 : data) {
        const unsigned b = b0 & 1u;
        switch (code) {
            case LineCode::NrzL: chips.push_back(static_cast<uint8_t>(b)); prev = b; break;
            case LineCode::NrzM: prev = b ? prev ^ 1u : prev; chips.push_back(static_cast<uint8_t>(prev)); break;
            case LineCode::NrzS: prev = b ? prev : prev ^ 1u; chips.push_back(static_cast<uint8_t>(prev)); break;
            case LineCode::ManchesterIeee:
                chips.push_back(static_cast<uint8_t>(b ^ 1u));
                chips.push_back(static_cast<uint8_t>(b));
                prev = b;
                break;
            case LineCode::ManchesterThomas:
                chips.push_back(static_cast<uint8_t>(b));
                chips.push_back(static_cast<uint8_t>(b ^ 1u));
                prev = b ^ 1u;
                break;
            case LineCode::DiffManchester:
            case LineCode::DiffManchesterInv: {
                const bool startTransition = (code == LineCode::DiffManchester) ? !b : b;
                const unsigned c0 = startTransition ? prev ^ 1u : prev;
                chips.push_back(static_cast<uint8_t>(c0));
                chips.push_back(static_cast<uint8_t>(c0 ^ 1u));
                prev = c0 ^ 1u;
                break;
            }
            case LineCode::BiphaseMark:
            case LineCode::BiphaseSpace: {
                const unsigned c0 = prev ^ 1u;
                const bool mid = (code == LineCode::BiphaseMark) ? b == 1 : b == 0;
                const unsigned c1 = mid ? c0 ^ 1u : c0;
                chips.push_back(static_cast<uint8_t>(c0));
                chips.push_back(static_cast<uint8_t>(c1));
                prev = c1;
                break;
            }
            case LineCode::Miller: {
                unsigned c0 = prev;
                if (prevBit == 0 && b == 0) c0 ^= 1u;
                const unsigned c1 = b ? c0 ^ 1u : c0;
                chips.push_back(static_cast<uint8_t>(c0));
                chips.push_back(static_cast<uint8_t>(c1));
                prev = c1;
                break;
            }
        }
        prevBit = static_cast<int>(b);
    }
    return chips;
}

// ---- runs -> chips (the slicer's output, one run per level, to the chip stream) -----------------

struct LevelRun {
    uint8_t level = 0;      // 0 or 1
    double duration = 0.0;  // any unit (samples, microseconds); the same unit as the chip period
};

// Estimate the chip period of a run list: the T for which the runs sit closest to whole multiples
// (run = n * T, n >= 1). A grid over [0.4, 1.6] x the 10th-percentile run is scored by the mean
// squared distance to the nearest allowed multiple; because a half-size T also fits clean data
// exactly, the LARGEST T within 0.004 of the best score is taken, then refined by least squares.
// Runs of more than 8 chips (an idle gap) are ignored. Returns 0 for fewer than two usable runs.
struct PeriodFit {
    double T = 0.0;
    double cost = 1e9;  // mean squared distance (in chips) of the usable runs from whole multiples of T; lower is cleaner
};

inline PeriodFit fitChipPeriod(const std::vector<LevelRun>& runs) {
    PeriodFit fit;
    std::vector<double> d;
    d.reserve(runs.size());
    for (const auto& r : runs)
        if (r.duration > 0) d.push_back(r.duration);
    if (d.size() < 2) return fit;
    std::vector<double> sorted(d);
    std::sort(sorted.begin(), sorted.end());
    const double p = sorted[sorted.size() / 10];
    auto cost = [&](double T) {
        double c = 0;
        size_t n = 0;
        for (double x : d) {
            const double q = x / T;
            if (q > 8.5) continue;
            const double k = std::max(1.0, std::round(q));
            c += (q - k) * (q - k);
            ++n;
        }
        return n ? c / static_cast<double>(n) : 1e9;
    };
    constexpr int kSteps = 360;
    std::vector<double> cs(kSteps + 1);
    double best = 1e18;
    for (int i = 0; i <= kSteps; ++i) {
        const double T = p * (0.4 + 1.2 * i / kSteps);
        cs[static_cast<size_t>(i)] = cost(T);
        best = std::min(best, cs[static_cast<size_t>(i)]);
    }
    double T = p;
    for (int i = kSteps; i >= 0; --i)
        if (cs[static_cast<size_t>(i)] <= best + 0.004) { T = p * (0.4 + 1.2 * i / kSteps); break; }
    for (int it = 0; it < 2; ++it) {
        double num = 0, den = 0;
        for (double x : d) {
            const double n = std::max(1.0, std::round(x / T));
            if (n > 8) continue;
            num += x * n;
            den += n * n;
        }
        if (den > 0) T = num / den;
    }
    fit.T = T;
    fit.cost = cost(T);
    return fit;
}

inline double estimateChipPeriod(const std::vector<LevelRun>& runs) { return fitChipPeriod(runs).T; }

// Run list -> chip stream at chip period T. A run counts max(1, round(duration / T)) chips; with
// `clipLong` a run over 8 chips (an idle gap) is cut to 8. `maxChips` bounds the output.
inline Bits runsToChips(const std::vector<LevelRun>& runs, double T, size_t maxChips = 1u << 20, bool clipLong = false) {
    Bits chips;
    if (!(T > 0)) return chips;
    for (const auto& r : runs) {
        double nd = std::round(r.duration / T);
        if (nd < 1) nd = 1;
        if (clipLong && nd > 8) nd = 8;
        const size_t n = static_cast<size_t>(std::min(nd, 1e9));
        for (size_t k = 0; k < n && chips.size() < maxChips; ++k) chips.push_back(r.level & 1u);
        if (chips.size() >= maxChips) break;
    }
    return chips;
}

// ---- pulse width / pulse position ------------------------------------------------------------

struct PulseRun {
    double mark = 0.0;   // on time
    double space = 0.0;  // off time after it
};

// 1-D two-means on a set of widths: {short centre, long centre, split threshold}; with a spread
// under `minRatio` (long / short) the set is judged to hold ONE class and long = short.
struct WidthClasses {
    double shortW = 0, longW = 0, threshold = 0;
    bool two = false;
};

inline WidthClasses classifyWidths(std::vector<double> w, double minRatio = 1.3) {
    WidthClasses c;
    w.erase(std::remove_if(w.begin(), w.end(), [](double x) { return !(x > 0); }), w.end());
    if (w.empty()) return c;
    std::sort(w.begin(), w.end());
    double lo = w.front(), hi = w.back();
    for (int it = 0; it < 32; ++it) {
        const double mid = (lo + hi) / 2;
        double sl = 0, sh = 0;
        size_t nl = 0, nh = 0;
        for (double x : w) {
            if (x < mid) { sl += x; ++nl; }
            else { sh += x; ++nh; }
        }
        if (!nl || !nh) break;
        const double nlo = sl / static_cast<double>(nl), nhi = sh / static_cast<double>(nh);
        if (nlo == lo && nhi == hi) break;
        lo = nlo;
        hi = nhi;
    }
    c.shortW = lo;
    c.longW = hi;
    c.two = hi / lo >= minRatio;
    c.threshold = (lo + hi) / 2;
    if (!c.two) c.longW = c.shortW;
    return c;
}

// Pulse-width modulation: the MARK width carries the bit (short = 0, long = 1 unless inverted).
// Pulse-position (PPM / pulse-distance): the SPACE width carries it. `threshold` 0 = estimate it.
struct PulseDecoded {
    Bits bits;
    WidthClasses classes;
};

inline PulseDecoded decodePulseWidth(const std::vector<PulseRun>& runs, bool useSpace, bool longIsOne = true,
                                     double threshold = 0.0) {
    PulseDecoded out;
    std::vector<double> w;
    w.reserve(runs.size());
    for (const auto& r : runs) w.push_back(useSpace ? r.space : r.mark);
    out.classes = classifyWidths(w);
    const double thr = threshold > 0 ? threshold : out.classes.threshold;
    out.bits.reserve(runs.size());
    for (const auto& r : runs) {
        const double x = useSpace ? r.space : r.mark;
        out.bits.push_back(static_cast<uint8_t>((x >= thr) == longIsOne ? 1u : 0u));
    }
    return out;
}

// ---- polarity and clock-phase search ----------------------------------------------------------

struct CodeHypothesis {
    LineCode code = LineCode::NrzL;
    bool inverted = false;
    size_t phase = 0;
    size_t violations = 0;
    size_t units = 0;
    double violationRate = 1.0;
    Bits bits;
};

// Try every code in `codes` (default all) x both polarities x every clock phase; keep those whose
// violation rate is at most `maxRate`, best (lowest rate, a two-chip code before the one-chip
// codes that fit anything, then more bits) first. NRZ-L never violates anything, so on its own it
// proves nothing; it is listed after two-chip codes that fit equally well.
inline std::vector<CodeHypothesis> searchLineCodes(const Bits& chips, double maxRate = 0.1, size_t keep = 16,
                                                   const std::vector<LineCode>& codes = {}) {
    std::vector<LineCode> list = codes;
    if (list.empty())
        for (size_t i = 0; i < kLineCodeCount; ++i) list.push_back(static_cast<LineCode>(i));
    std::vector<CodeHypothesis> out;
    for (LineCode lc : list)
        for (int inv = 0; inv < 2; ++inv)
            for (size_t ph = 0; ph < chipsPerBit(lc); ++ph) {
                const Bits src = inv ? inverted(chips) : chips;
                Decoded d = decodeLineCode(src, lc, ph);
                if (d.units == 0) continue;
                CodeHypothesis h;
                h.code = lc;
                h.inverted = inv != 0;
                h.phase = ph;
                h.violations = d.violations;
                h.units = d.units;
                h.violationRate = static_cast<double>(d.violations) / static_cast<double>(d.units);
                h.bits = std::move(d.bits);
                if (h.violationRate <= maxRate) out.push_back(std::move(h));
            }
    std::stable_sort(out.begin(), out.end(), [](const CodeHypothesis& a, const CodeHypothesis& b) {
        if (a.violationRate != b.violationRate) return a.violationRate < b.violationRate;
        const bool an = a.code <= LineCode::NrzS, bn = b.code <= LineCode::NrzS;
        if (an != bn) return !an;
        return a.units > b.units;
    });
    if (out.size() > keep) out.resize(keep);
    return out;
}

// A run list has two chip-period readings when the code has no one-chip runs (Miller, for one: its
// shortest run is two chips): T as estimated, and T / 2.
struct RunHypothesis : CodeHypothesis {
    double chipPeriod = 0.0;
};

// searchLineCodes over a run list, once per period reading (T, T/2); the result is ranked as
// searchLineCodes ranks, and says which period each hypothesis used.
inline std::vector<RunHypothesis> searchLineCodesFromRuns(const std::vector<LevelRun>& runs, double maxRate = 0.1, size_t keep = 16,
                                                          const std::vector<LineCode>& codes = {}) {
    std::vector<RunHypothesis> out;
    const double T = estimateChipPeriod(runs);
    if (!(T > 0)) return out;
    for (double period : {T, T / 2}) {
        const Bits chips = runsToChips(runs, period);
        for (auto& h : searchLineCodes(chips, maxRate, keep, codes)) {
            RunHypothesis r;
            static_cast<CodeHypothesis&>(r) = std::move(h);
            r.chipPeriod = period;
            out.push_back(std::move(r));
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const RunHypothesis& a, const RunHypothesis& b) {
        if (a.violationRate != b.violationRate) return a.violationRate < b.violationRate;
        const bool an = a.code <= LineCode::NrzS, bn = b.code <= LineCode::NrzS;
        if (an != bn) return !an;
        return a.units > b.units;
    });
    if (out.size() > keep) out.resize(keep);
    return out;
}

}  // namespace cascade::bits
