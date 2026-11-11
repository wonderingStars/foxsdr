// mark_layout.cpp - see mark_layout.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "mark_layout.hpp"

#include <algorithm>
#include <numeric>

namespace foxsdr::slintmarks {

std::size_t characterCount(const std::string& s) {
    std::size_t n = 0;
    for (unsigned char c : s) { if ((c & 0xC0) != 0x80) { ++n; } }
    return n;
}

MarkLayout declutter(const std::vector<MarkName>& marks, double plotPx, double charPx, int maxRows, double gapPx, double tabPx) {
    MarkLayout out;
    out.row.assign(marks.size(), -1);
    if (marks.empty() || plotPx <= 0 || maxRows <= 0) { return out; }
    std::vector<std::size_t> order;
    order.reserve(marks.size());
    for (std::size_t i = 0; i < marks.size(); ++i) {
        if (marks[i].chars > 0 && marks[i].pos >= 0.0 && marks[i].pos <= 1.0) { order.push_back(i); }
    }
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return marks[a].pos < marks[b].pos; });
    std::vector<double> rowEnd(static_cast<std::size_t>(maxRows), -1e9);
    std::vector<std::size_t> collapsed;
    for (const std::size_t i : order) {
        const double x = marks[i].pos * plotPx;
        const double w = static_cast<double>(marks[i].chars) * charPx + gapPx;
        int placed = -1;
        for (int r = 0; r < maxRows; ++r) {
            if (x >= rowEnd[static_cast<std::size_t>(r)]) { placed = r; break; }
        }
        if (placed < 0) { collapsed.push_back(i); continue; }
        rowEnd[static_cast<std::size_t>(placed)] = x + w;
        out.row[i] = placed;
    }
    // the tabs: collapsed names closer than a tab's width share one (the cluster is anchored at its first name)
    double clusterStart = -1e9;
    for (const std::size_t i : collapsed) {
        const double x = marks[i].pos * plotPx;
        if (out.tabs.empty() || x - clusterStart >= tabPx) {
            MarkTab t;
            t.pos = marks[i].pos;
            out.tabs.push_back(t);
            clusterStart = x;
        }
        out.tabs.back().items.push_back(i);
    }
    return out;
}

std::string tooltipText(const std::string& name, const std::string& hz, const std::string& sourceLine) {
    std::string t = name;
    if (!hz.empty()) { t += (t.empty() ? "" : "  ") + hz; }
    if (!sourceLine.empty()) { t += (t.empty() ? "" : "\n") + sourceLine; }
    return t;
}

}  // namespace foxsdr::slintmarks
