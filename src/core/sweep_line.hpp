// sweep_line.hpp - THE COMMON RECORD of the sweep lane: one line of a swept
// spectrum (start, stop, N dB values), and the frequency <-> point functions
// every other file of the lane shares (the stitcher, the heatmap's click-to-tune,
// the history files, the occupancy model, the tinySA source).
//
// A line is NOT personal data: frequencies, levels and a time. Memory only; the
// files that save it are in sweep_history.hpp and carry nothing else.
//
// Point i of an N-point line is at  start + i * (stop - start) / (N - 1)  (N >= 2);
// a one-point line sits at start and its stop must equal start. A level may be
// NaN, meaning "no data at this point" (a gap in a stitched trace); infinities
// are never valid.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cmath>
#include <cstddef>
#include <limits>
#include <string>
#include <vector>

namespace cascade::core {

// The bounds every parser and builder of the lane enforces, so one malformed
// file or one hostile frame cannot ask for gigabytes.
inline constexpr std::size_t kSweepMaxPointsPerLine = std::size_t{1} << 20;   // 1 048 576
inline constexpr double kSweepMaxFrequencyHz = 1.0e12;                        // 1 THz
inline constexpr double kSweepMinLevelDb = -400.0;                            // sanity bounds on a stored level
inline constexpr double kSweepMaxLevelDb = 400.0;

struct SweepLine {
    double startHz = 0.0;
    double stopHz = 0.0;
    std::vector<float> db;   // NaN = no data
    double timeS = 0.0;      // seconds; the caller's clock (Unix time for the occupancy model)

    std::size_t points() const { return db.size(); }
    bool operator==(const SweepLine& o) const;   // bitwise on levels (NaN equals NaN)
};

// Why a line is not usable, or an empty string when it is. Checks: at least one
// point, at most kSweepMaxPointsPerLine, frequencies finite and within
// [0, kSweepMaxFrequencyHz], stop >= start (== for one point), time finite,
// every level NaN or within [kSweepMinLevelDb, kSweepMaxLevelDb].
std::string sweepLineProblem(const SweepLine& line);

inline double sweepStepHz(const SweepLine& l) {
    return l.db.size() < 2 ? 0.0 : (l.stopHz - l.startHz) / static_cast<double>(l.db.size() - 1);
}

// The frequency of point i (i is clamped to the line).
inline double sweepPointHz(const SweepLine& l, std::size_t i) {
    if (l.db.empty()) { return l.startHz; }
    if (i >= l.db.size()) { i = l.db.size() - 1; }
    return l.startHz + sweepStepHz(l) * static_cast<double>(i);
}

// The point nearest a frequency, or -1 when the line is empty or the frequency
// is further than half a step outside either end (a click beside the trace
// tunes nothing).
long long sweepNearestPoint(const SweepLine& l, double hz);

}  // namespace cascade::core
