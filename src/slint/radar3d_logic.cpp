// radar3d_logic.cpp - see radar3d_logic.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "radar3d_logic.hpp"

#include <algorithm>
#include <cstdio>
#include <numeric>

namespace foxsdr::slintview3d {

void polarToEastNorth(double rangeNm, double bearingDeg, double& eastNm, double& northNm) {
    if (!std::isfinite(rangeNm) || !std::isfinite(bearingDeg)) { eastNm = 0.0; northNm = 0.0; return; }
    const double b = bearingDeg * 3.14159265358979323846 / 180.0;
    eastNm = rangeNm * std::sin(b);
    northNm = rangeNm * std::cos(b);
}

double flightLevel(double altFt) { return std::isfinite(altFt) ? altFt / 100.0 : std::numeric_limits<double>::quiet_NaN(); }

std::string flightLevelText(double altFt, const std::string& noAlt) {
    if (!std::isfinite(altFt)) { return noAlt; }
    char b[24];
    std::snprintf(b, sizeof(b), "FL%03d", static_cast<int>(std::lround(std::max(0.0, altFt) / 100.0)));
    return b;
}

int scaleFactorOfIndex(int index) { return kRadar3dScales[static_cast<std::size_t>(std::clamp(index, 0, 2))]; }

double sceneHeight(double altFt, int factor) {
    if (!std::isfinite(altFt)) { return 0.0; }
    const double fl = std::clamp(altFt / 100.0, 0.0, kRadar3dMaxFl);
    return fl / kRadar3dMaxFl * kRadar3dHeightAtX1 * static_cast<double>(std::max(1, factor));
}

std::vector<std::size_t> labelOrder(const std::vector<Radar3dAircraft>& aircraft, std::size_t maxLabels) {
    std::vector<std::size_t> order(aircraft.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    const auto range2 = [&](std::size_t i) { return aircraft[i].eastNm * aircraft[i].eastNm + aircraft[i].northNm * aircraft[i].northNm; };
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        if (aircraft[a].selected != aircraft[b].selected) { return aircraft[a].selected; }
        return range2(a) < range2(b);
    });
    if (order.size() > maxLabels) { order.resize(maxLabels); }
    return order;
}

}  // namespace foxsdr::slintview3d
