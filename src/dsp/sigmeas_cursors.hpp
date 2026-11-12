// sigmeas_cursors.hpp - HARMONIC and SIDEBAND CURSORS, and the EIGHT-CURSOR TUNING TOOL (the model
// only; the drawing is for the screen lane). Rows: "harmonic and sideband cursors" (REMAINING-GAPS.md,
// Signal analysis and identification) and "an eight-cursor tuning tool (Wavecom)" (DEPTH-GAPS.md
// Part B).
//
//  - `harmonicCursors(base, lo, hi)` lists k * base (k = 1, 2, ...) inside [lo, hi]; `sidebandCursors`
//    lists carrier +- k * spacing (k = 1 .. count) inside [lo, hi], carrier first. Each mark carries its
//    order and kind. `readLevels` lays a power spectrum (sigmeas_common.hpp Psd) under a list of
//    marks: each mark is snapped to the strongest bin within +-searchHz (parabolic interpolation on
//    the log power) and its level is read in dBFS per bin.
//  - `CursorSet8` is eight cursors in one of three modes:
//      Free           every cursor moves alone;
//      LockedSpacing  the cursors sit at anchor + i * spacing; moving one moves all, and
//                     `setSpacing` changes the pitch about cursor 0;
//      Harmonic       the cursors sit at (i + 1) * base: cursor 0 is the fundamental and the others
//                     are its harmonics; moving cursor i sets base = f / (i + 1).
//    The readout gives, for each cursor, its frequency, its OFFSET from the reference cursor (default
//    cursor 0), the RATIO to it (a harmonic set reads 1, 2, 3 ...), and a level when a spectrum has
//    been laid under it. `meanSpacingHz` and `spacingRmsHz` say how regular the set is (rms 0 for a
//    locked set). `snapToPeaks` (Free mode only: a snap would break a lock) moves each cursor to the
//    peak next to it.
//  A cursor outside [rangeLowHz, rangeHighHz] is flagged `inRange == false` (and never hidden: the
//  screen decides).
// Every setter refuses non-finite numbers and out-of-range indices and leaves the set unchanged.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_DSP_SIGMEAS_CURSORS_HPP
#define CASCADE_DSP_SIGMEAS_CURSORS_HPP

#include <array>
#include <cstddef>
#include <vector>

#include "dsp/sigmeas_common.hpp"

namespace cascade::sigmeas {

enum class MarkKind { Fundamental, Harmonic, UpperSideband, LowerSideband, Carrier };

struct CursorMark {
    double freqHz = 0.0;
    int order = 1;                 // harmonic number or sideband order
    MarkKind kind = MarkKind::Harmonic;
    double snappedHz = kNaN;       // after readLevels
    double levelDbfs = kNaN;       // after readLevels (per bin)
    bool inRange = true;
};

inline std::vector<CursorMark> harmonicCursors(double baseHz, double loHz, double hiHz,
                                               std::size_t maxCount = 64) {
    std::vector<CursorMark> out;
    if (!std::isfinite(baseHz) || !std::isfinite(loHz) || !std::isfinite(hiHz) || !(baseHz != 0.0) ||
        !(hiHz > loHz)) {
        return out;
    }
    for (int k = 1; out.size() < maxCount && k < 100000; ++k) {
        const double f = baseHz * static_cast<double>(k);
        if ((baseHz > 0.0 && f > hiHz) || (baseHz < 0.0 && f < loHz)) { break; }
        if (f < loHz || f > hiHz) { continue; }
        CursorMark m;
        m.freqHz = f;
        m.order = k;
        m.kind = k == 1 ? MarkKind::Fundamental : MarkKind::Harmonic;
        out.push_back(m);
    }
    return out;
}

inline std::vector<CursorMark> sidebandCursors(double carrierHz, double spacingHz, int count, double loHz,
                                               double hiHz) {
    std::vector<CursorMark> out;
    if (!std::isfinite(carrierHz) || !std::isfinite(spacingHz) || !(spacingHz > 0.0) || count < 1 ||
        count > 1000 || !std::isfinite(loHz) || !std::isfinite(hiHz) || !(hiHz > loHz)) {
        return out;
    }
    CursorMark c;
    c.freqHz = carrierHz;
    c.order = 0;
    c.kind = MarkKind::Carrier;
    c.inRange = carrierHz >= loHz && carrierHz <= hiHz;
    if (c.inRange) { out.push_back(c); }
    for (int k = 1; k <= count; ++k) {
        for (int sign : {+1, -1}) {
            CursorMark m;
            m.freqHz = carrierHz + sign * static_cast<double>(k) * spacingHz;
            m.order = k;
            m.kind = sign > 0 ? MarkKind::UpperSideband : MarkKind::LowerSideband;
            if (m.freqHz >= loHz && m.freqHz <= hiHz) { out.push_back(m); }
        }
    }
    return out;
}

// Snaps `freqHz` to the strongest bin within +-searchHz; returns false (outputs NaN) if the spectrum is
// invalid or the window is outside it.
inline bool snapToPeak(const Psd& psd, double freqHz, double searchHz, double* snappedHz, double* levelDbfs) {
    if (snappedHz) { *snappedHz = kNaN; }
    if (levelDbfs) { *levelDbfs = kNaN; }
    if (!psd.valid || psd.p.size() < 4 || !std::isfinite(freqHz) || !(searchHz >= 0.0) || !std::isfinite(searchHz)) {
        return false;
    }
    const double centre = freqHz / psd.binHz + static_cast<double>(psd.nfft) / 2.0;
    const double span = std::max(1.0, searchHz / psd.binHz);
    const long a = std::max(1L, static_cast<long>(std::floor(centre - span)));
    const long b = std::min(static_cast<long>(psd.p.size()) - 2, static_cast<long>(std::ceil(centre + span)));
    if (a > b) { return false; }
    long pk = a;
    for (long k = a; k <= b; ++k) { if (psd.p[static_cast<std::size_t>(k)] > psd.p[static_cast<std::size_t>(pk)]) { pk = k; } }
    const double l = std::log(std::max(psd.p[static_cast<std::size_t>(pk) - 1], 1e-300));
    const double m = std::log(std::max(psd.p[static_cast<std::size_t>(pk)], 1e-300));
    const double u = std::log(std::max(psd.p[static_cast<std::size_t>(pk) + 1], 1e-300));
    const double den = l - 2.0 * m + u;
    const double delta = den != 0.0 ? std::min(0.5, std::max(-0.5, 0.5 * (l - u) / den)) : 0.0;
    if (snappedHz) { *snappedHz = (static_cast<double>(pk) + delta - static_cast<double>(psd.nfft) / 2.0) * psd.binHz; }
    if (levelDbfs) { *levelDbfs = toDb(psd.p[static_cast<std::size_t>(pk)]); }
    return true;
}

inline void readLevels(const Psd& psd, std::vector<CursorMark>& marks, double searchHz) {
    for (CursorMark& m : marks) { snapToPeak(psd, m.freqHz, searchHz, &m.snappedHz, &m.levelDbfs); }
}

// ---- the eight cursors --------------------------------------------------------------------------------
enum class CursorMode { Free, LockedSpacing, Harmonic };

struct CursorReadout {
    std::size_t index = 0;
    double freqHz = 0.0;
    double offsetHz = 0.0;     // from the reference cursor
    double ratio = kNaN;       // freq / reference freq (NaN if the reference is 0)
    double levelDbfs = kNaN;   // after layLevels
    bool inRange = true;
};

class CursorSet8 {
public:
    static constexpr std::size_t kCount = 8;

    CursorSet8() {
        for (std::size_t i = 0; i < kCount; ++i) { f_[i] = 1000.0 * static_cast<double>(i + 1); }
        level_.fill(kNaN);
    }

    CursorMode mode() const { return mode_; }
    double frequency(std::size_t i) const { return i < kCount ? f_[i] : kNaN; }
    std::size_t reference() const { return ref_; }
    double spacingHz() const { return spacing_; }
    double baseHz() const { return f_[0]; }

    // Changing the mode keeps the cursors where they are and takes the lock from them: LockedSpacing
    // uses the mean spacing of the present positions, Harmonic uses cursor 0 as the base and moves the
    // others to its multiples.
    bool setMode(CursorMode m) {
        if (m == CursorMode::LockedSpacing) {
            spacing_ = meanSpacingHz();
            if (!std::isfinite(spacing_)) { spacing_ = 1000.0; }
            layOut(f_[0]);
        } else if (m == CursorMode::Harmonic) {
            if (!(f_[0] != 0.0)) { return false; }
            for (std::size_t i = 0; i < kCount; ++i) { f_[i] = f_[0] * static_cast<double>(i + 1); }
        }
        mode_ = m;
        return true;
    }

    bool setCursor(std::size_t i, double freqHz) {
        if (i >= kCount || !std::isfinite(freqHz)) { return false; }
        switch (mode_) {
            case CursorMode::Free:
                f_[i] = freqHz;
                level_[i] = kNaN;   // a level read at the old place is stale
                break;
            case CursorMode::LockedSpacing:
                layOut(freqHz - static_cast<double>(i) * spacing_);
                break;
            case CursorMode::Harmonic: {
                if (freqHz == 0.0) { return false; }
                const double base = freqHz / static_cast<double>(i + 1);
                for (std::size_t j = 0; j < kCount; ++j) { f_[j] = base * static_cast<double>(j + 1); }
                level_.fill(kNaN);
                break;
            }
        }
        return true;
    }

    // LockedSpacing: the pitch about cursor 0. Free: lays the cursors out from cursor 0 at that pitch.
    // Harmonic: refused (the pitch of a harmonic set is its base).
    bool setSpacing(double hz) {
        if (!std::isfinite(hz) || mode_ == CursorMode::Harmonic) { return false; }
        spacing_ = hz;
        layOut(f_[0]);
        return true;
    }

    bool setReference(std::size_t i) {
        if (i >= kCount) { return false; }
        ref_ = i;
        return true;
    }

    bool setRange(double loHz, double hiHz) {
        if (!std::isfinite(loHz) || !std::isfinite(hiHz) || !(hiHz > loHz)) { return false; }
        lo_ = loHz;
        hi_ = hiHz;
        return true;
    }

    double offsetHz(std::size_t i, std::size_t from) const {
        return (i < kCount && from < kCount) ? f_[i] - f_[from] : kNaN;
    }
    double ratio(std::size_t i, std::size_t over) const {
        return (i < kCount && over < kCount && f_[over] != 0.0) ? f_[i] / f_[over] : kNaN;
    }

    // Mean of the seven steps between neighbouring cursors in INDEX order, and their rms deviation from it.
    double meanSpacingHz() const { return (f_[kCount - 1] - f_[0]) / static_cast<double>(kCount - 1); }
    double spacingRmsHz() const {
        const double mean = meanSpacingHz();
        double ss = 0.0;
        for (std::size_t i = 1; i < kCount; ++i) {
            const double d = (f_[i] - f_[i - 1]) - mean;
            ss += d * d;
        }
        return std::sqrt(ss / static_cast<double>(kCount - 1));
    }

    // Moves each cursor to the peak within +-searchHz and records its level. Free mode only.
    bool snapToPeaks(const Psd& psd, double searchHz) {
        if (mode_ != CursorMode::Free || !psd.valid) { return false; }
        std::array<double, kCount> nf = f_, nl{};
        for (std::size_t i = 0; i < kCount; ++i) {
            double s, l;
            if (!snapToPeak(psd, f_[i], searchHz, &s, &l)) { return false; }
            nf[i] = s;
            nl[i] = l;
        }
        f_ = nf;
        level_ = nl;
        return true;
    }
    // Reads levels under the cursors without moving them (any mode).
    bool layLevels(const Psd& psd, double searchHz) {
        if (!psd.valid) { return false; }
        for (std::size_t i = 0; i < kCount; ++i) {
            double s, l;
            if (!snapToPeak(psd, f_[i], searchHz, &s, &l)) { return false; }
            level_[i] = l;
        }
        return true;
    }

    std::array<CursorReadout, kCount> readout() const {
        std::array<CursorReadout, kCount> r{};
        for (std::size_t i = 0; i < kCount; ++i) {
            r[i].index = i;
            r[i].freqHz = f_[i];
            r[i].offsetHz = f_[i] - f_[ref_];
            r[i].ratio = f_[ref_] != 0.0 ? f_[i] / f_[ref_] : kNaN;
            r[i].levelDbfs = level_[i];
            r[i].inRange = f_[i] >= lo_ && f_[i] <= hi_;
        }
        return r;
    }

private:
    void layOut(double anchor) {
        for (std::size_t i = 0; i < kCount; ++i) { f_[i] = anchor + static_cast<double>(i) * spacing_; }
        level_.fill(kNaN);
    }

    std::array<double, kCount> f_{};
    std::array<double, kCount> level_{};
    CursorMode mode_ = CursorMode::Free;
    double spacing_ = 1000.0;
    std::size_t ref_ = 0;
    double lo_ = -std::numeric_limits<double>::infinity();
    double hi_ = std::numeric_limits<double>::infinity();
};

}  // namespace cascade::sigmeas

#endif  // CASCADE_DSP_SIGMEAS_CURSORS_HPP
