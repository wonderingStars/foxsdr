// wfrows_logic.cpp - see wfrows_logic.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "wfrows_logic.hpp"

#include <cstdio>

namespace foxsdr::slinthost {

std::string elapsedText(double seconds) {
    char buf[32];
    if (!(seconds > 0.0)) { return "0s"; }
    double whole = std::floor(seconds + 0.5);
    if (whole > 359999.0) { whole = 359999.0; }
    const long long total = static_cast<long long>(whole);
    if (total < 60) {
        std::snprintf(buf, sizeof(buf), "%llds", total);
    } else if (total < 3600) {
        const long long m = total / 60, s = total % 60;
        if (s == 0) { std::snprintf(buf, sizeof(buf), "%lldm", m); } else { std::snprintf(buf, sizeof(buf), "%lldm%02lld", m, s); }
    } else {
        std::snprintf(buf, sizeof(buf), "%lldh%02lld", total / 3600, (total % 3600) / 60);
    }
    return buf;
}

double timeLabelStep(double spanSeconds, int maxLabels) {
    if (!(spanSeconds > 0.0) || maxLabels < 1) { return 0.0; }
    static const double kLadder[] = {1.0, 2.0, 5.0, 10.0, 15.0, 20.0, 30.0, 60.0, 120.0, 300.0, 600.0, 900.0, 1800.0, 3600.0};
    const double want = spanSeconds / static_cast<double>(maxLabels);
    for (const double s : kLadder) {
        if (s >= want) { return s; }
    }
    return std::ceil(want / 3600.0) * 3600.0;
}

WfChromeFig wfChrome(const WfRowLog& log, double nowS, int labelRoom) {
    WfChromeFig f;
    const int h = log.height();
    if (h <= 0) { return f; }
    const WfRowLog::Gap g = log.gap(nowS, labelRoom);
    // Clear of the strip's heading (the first rows) and of its foot: a break in the last rows has nothing below it to name.
    if (g.found() && g.row >= 3 && g.row <= log.filled() - 2) {
        f.gapPos = static_cast<float>(g.row) / static_cast<float>(h);
        f.gapAbove = elapsedText(g.above);
        f.gapBelow = elapsedText(g.below);
    }
    if (log.hasRange() && log.rangeRows() > 0 && log.rangeRows() < log.filled()) {
        f.coveredRows = log.rangeRows();
        f.railPos = static_cast<float>(log.rangeRows()) / static_cast<float>(h);
    }
    return f;
}

}  // namespace foxsdr::slinthost
