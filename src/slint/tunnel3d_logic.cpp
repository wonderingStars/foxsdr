// tunnel3d_logic.cpp - see tunnel3d_logic.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "tunnel3d_logic.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace foxsdr::slintview3d {

double availableMs(std::size_t samples, double rateHz) { return rateHz > 0.0 ? 1000.0 * static_cast<double>(samples) / rateHz : 0.0; }

double effectiveDepthMs(int index, double availMs, double rateHz) {
    const double want = kTunnelDepthsMs[static_cast<std::size_t>(std::clamp(index, 0, 15))];
    const double oneSample = rateHz > 0.0 ? 1000.0 / rateHz : 0.0;
    return std::max(oneSample, std::min(want, availMs));
}

std::size_t samplesFor(double depthMs, double rateHz, std::size_t have) {
    if (have == 0) { return 0; }
    const double n = rateHz > 0.0 ? std::ceil(depthMs * rateHz / 1000.0) : static_cast<double>(have);
    return std::clamp(static_cast<std::size_t>(std::max(2.0, n)), std::size_t{2}, std::max<std::size_t>(have, 2));
}

double niceAxis(double peak) {
    if (!(peak > 0.0) || !std::isfinite(peak)) { return 1.0; }
    for (const double a : {0.05, 0.1, 0.2, 0.25, 0.5, 1.0, 2.0, 5.0, 10.0}) {
        if (a >= peak * 1.05) { return a; }
    }
    return std::ceil(peak * 1.05);
}

std::string formatAxis(double v, double axis) {
    char b[32];
    std::snprintf(b, sizeof(b), axis >= 5.0 ? "%.1f" : "%.2f", v);
    std::string s = b;
    if (s == "-0.00" || s == "-0.0") { s = s.substr(1); }
    return s;
}

std::string formatMs(double ms, double spanMs) {
    char b[32];
    std::snprintf(b, sizeof(b), (spanMs <= 10.0 && std::fabs(ms - std::round(ms)) > 1e-6) ? "%.1f" : "%.0f", ms);
    return b;
}

int depthIndexOfSlider(float v) { return std::clamp(static_cast<int>(std::lround(std::clamp(v, 0.0f, 1.0f) * 15.0f)), 0, 15); }
float sliderOfDepthIndex(int index) { return static_cast<float>(std::clamp(index, 0, 15)) / 15.0f; }

}  // namespace foxsdr::slintview3d
