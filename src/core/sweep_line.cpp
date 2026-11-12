// sweep_line.cpp - see sweep_line.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/sweep_line.hpp"

#include <cstring>

namespace cascade::core {

bool SweepLine::operator==(const SweepLine& o) const {
    if (startHz != o.startHz || stopHz != o.stopHz || timeS != o.timeS || db.size() != o.db.size()) {
        return false;
    }
    return db.empty() || std::memcmp(db.data(), o.db.data(), db.size() * sizeof(float)) == 0 ||
           [&] {
               for (std::size_t i = 0; i < db.size(); ++i) {
                   const bool an = std::isnan(db[i]);
                   const bool bn = std::isnan(o.db[i]);
                   if (an != bn || (!an && db[i] != o.db[i])) { return false; }
               }
               return true;
           }();
}

std::string sweepLineProblem(const SweepLine& l) {
    if (l.db.empty()) { return "no points"; }
    if (l.db.size() > kSweepMaxPointsPerLine) { return "too many points"; }
    if (!std::isfinite(l.startHz) || !std::isfinite(l.stopHz)) { return "frequency is not a number"; }
    if (l.startHz < 0.0 || l.stopHz > kSweepMaxFrequencyHz) { return "frequency out of range"; }
    if (l.stopHz < l.startHz) { return "stop below start"; }
    if (l.db.size() == 1 && l.stopHz != l.startHz) { return "one point needs stop equal to start"; }
    if (!std::isfinite(l.timeS)) { return "time is not a number"; }
    for (float v : l.db) {
        if (std::isnan(v)) { continue; }
        if (!std::isfinite(v) || v < kSweepMinLevelDb || v > kSweepMaxLevelDb) {
            return "level out of range";
        }
    }
    return {};
}

long long sweepNearestPoint(const SweepLine& l, double hz) {
    if (l.db.empty() || !std::isfinite(hz)) { return -1; }
    if (l.db.size() == 1) { return std::fabs(hz - l.startHz) <= 0.5 ? 0 : -1; }
    const double step = sweepStepHz(l);
    if (!(step > 0.0)) { return hz == l.startHz ? 0 : -1; }
    const double pos = (hz - l.startHz) / step;
    if (pos < -0.5 || pos > static_cast<double>(l.db.size() - 1) + 0.5) { return -1; }
    long long i = std::llround(pos);
    if (i < 0) { i = 0; }
    if (i > static_cast<long long>(l.db.size()) - 1) { i = static_cast<long long>(l.db.size()) - 1; }
    return i;
}

}  // namespace cascade::core
