// airmarks_logic.cpp - see airmarks_logic.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "airmarks_logic.hpp"

#include <algorithm>

namespace foxsdr::slinthost {

std::vector<AirMarkFig> airbandMarks(const std::vector<AirChannelMark>& channels, double centreHz, double spanHz, double plotPx) {
    std::vector<AirMarkFig> out;
    if (!(spanHz > 0.0) || !(plotPx > 0.0)) { return out; }
    const double lo = centreHz - spanHz / 2.0, hi = centreHz + spanHz / 2.0;
    const double minHalf = 0.5 * static_cast<double>(kAirMarkMinPx) / plotPx;
    for (const AirChannelMark& c : channels) {
        if (!(c.hz >= lo && c.hz <= hi)) { continue; }   // also false for NaN
        const double half = c.bwHz > 0.0 ? 0.5 * c.bwHz : 0.0;
        const double xc = (c.hz - lo) / spanHz;
        const double a = std::min((c.hz - half - lo) / spanHz, xc - minHalf);
        const double b = std::max((c.hz + half - lo) / spanHz, xc + minHalf);
        out.push_back({static_cast<float>(std::clamp(a, 0.0, 1.0)), static_cast<float>(std::clamp(b, 0.0, 1.0)), c.open});
    }
    return out;
}

}  // namespace foxsdr::slinthost
