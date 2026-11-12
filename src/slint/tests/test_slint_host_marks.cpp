// The names of plugin marks, decluttered (src/slint/mark_layout.hpp, lane S1 item 4): names that would overlap are stacked into rows, what fits no row is collapsed into "+N" tabs, a mark with no name
// or outside the window draws no name, the tooltip's source line, and the cost at 0, 64 and 256 marks (the per-tick work is measured and printed; it is held to a bound a frame can afford).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "mark_layout.hpp"
#include "test_check.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

using namespace foxsdr::slintmarks;

namespace {

constexpr double kPlot = 1000.0, kChar = 6.0;

// no two names in the same row overlap (a name is chars * kChar wide from its line), whatever the layout chose
bool noOverlap(const std::vector<MarkName>& m, const MarkLayout& l, double plot, double charPx) {
    for (std::size_t i = 0; i < m.size(); ++i) {
        for (std::size_t j = i + 1; j < m.size(); ++j) {
            if (l.row[i] < 0 || l.row[i] != l.row[j]) { continue; }
            const double a0 = m[i].pos * plot, a1 = a0 + static_cast<double>(m[i].chars) * charPx, b0 = m[j].pos * plot, b1 = b0 + static_cast<double>(m[j].chars) * charPx;
            if (a0 < b1 && b0 < a1) { return false; }
        }
    }
    return true;
}

void basics() {
    CHECK(declutter({}, kPlot, kChar, 3).row.empty());
    const MarkLayout one = declutter({{0.5, 8}}, kPlot, kChar, 3);
    CHECK(one.row.size() == 1 && one.row[0] == 0 && one.tabs.empty());
    // far apart: all on row 0
    const std::vector<MarkName> far{{0.1, 5}, {0.4, 5}, {0.8, 5}};
    const MarkLayout f = declutter(far, kPlot, kChar, 3);
    CHECK(f.row[0] == 0 && f.row[1] == 0 && f.row[2] == 0 && f.tabs.empty());
    // close: stacked into rows 0, 1, 2 in order
    const std::vector<MarkName> close{{0.100, 10}, {0.101, 10}, {0.102, 10}, {0.103, 10}};
    const MarkLayout c = declutter(close, kPlot, kChar, 3);
    CHECK(c.row[0] == 0 && c.row[1] == 1 && c.row[2] == 2);
    CHECK(c.row[3] == -1 && c.tabs.size() == 1 && c.tabs[0].items.size() == 1 && c.tabs[0].items[0] == 3);   // the fourth fits no row: a "+1" tab
    CHECK(noOverlap(close, c, kPlot, kChar));
    // input order does not matter: the layout follows the position
    const std::vector<MarkName> shuffled{{0.103, 10}, {0.100, 10}, {0.102, 10}, {0.101, 10}};
    const MarkLayout s = declutter(shuffled, kPlot, kChar, 3);
    CHECK(s.row[1] == 0 && s.row[3] == 1 && s.row[2] == 2 && s.row[0] == -1);
}

void notPlaced() {
    const std::vector<MarkName> m{{0.5, 0}, {-0.1, 6}, {1.2, 6}, {0.2, 6}};   // no name, left of the window, right of it, one that counts
    const MarkLayout l = declutter(m, kPlot, kChar, 3);
    CHECK(l.row[0] == -1 && l.row[1] == -1 && l.row[2] == -1 && l.row[3] == 0 && l.tabs.empty());   // these draw no name and are NOT counted into a tab
    CHECK(declutter(m, 0.0, kChar, 3).row[3] == -1);       // no plot
    CHECK(declutter(m, kPlot, kChar, 0).row[3] == -1);     // no rows
}

void tabs() {
    // 12 names crowded into 20 px: 3 take rows, 9 collapse into ONE tab (they are within a tab's width of each other)
    std::vector<MarkName> m;
    for (int i = 0; i < 12; ++i) { m.push_back({0.200 + 0.0015 * i, 12}); }
    const MarkLayout l = declutter(m, kPlot, kChar, 3);
    CHECK(std::count(l.row.begin(), l.row.end(), -1) == 9);
    CHECK(l.tabs.size() == 1 && l.tabs[0].items.size() == 9);
    CHECK(!l.tabs.empty() && l.tabs[0].pos == m[3].pos);        // anchored at the first collapsed name
    CHECK(noOverlap(m, l, kPlot, kChar));
    // two crowds far apart make two tabs
    std::vector<MarkName> two;
    for (int i = 0; i < 6; ++i) { two.push_back({0.100 + 0.001 * i, 12}); }
    for (int i = 0; i < 6; ++i) { two.push_back({0.700 + 0.001 * i, 12}); }
    const MarkLayout t = declutter(two, kPlot, kChar, 3);
    CHECK(t.tabs.size() == 2 && t.tabs[0].items.size() == 3 && t.tabs[1].items.size() == 3 && t.tabs[0].pos < t.tabs[1].pos);
    if (t.tabs.size() != 2) { return; }
    // every collapsed mark is in exactly one tab, and every tab's marks are collapsed
    std::size_t inTabs = 0;
    for (const MarkTab& tab : t.tabs) { for (const std::size_t i : tab.items) { CHECK(t.row[i] == -1); ++inTabs; } }
    CHECK(inTabs == static_cast<std::size_t>(std::count(t.row.begin(), t.row.end(), -1)));
}

void characters() {
    CHECK(characterCount("") == 0 && characterCount("abc") == 3);
    CHECK(characterCount("\xC3\xA4\xC3\xB6") == 2);                 // two characters, four bytes
    CHECK(characterCount("\xE6\x97\xA5\xE6\x9C\xAC") == 2);         // two characters, six bytes
}

void tips() {
    CHECK(tooltipText("BBC 0900 Eng", "5.975 MHz", "from EiBi") == "BBC 0900 Eng  5.975 MHz\nfrom EiBi");
    CHECK(tooltipText("BBC", "", "") == "BBC");                        // no source line when the mark came from no named plugin
    CHECK(tooltipText("", "5.975 MHz", "from EiBi") == "5.975 MHz\nfrom EiBi");
    CHECK(tooltipText("X", "1 MHz", "P") == "X  1 MHz\nP");
    CHECK(tooltipText("", "", "from P") == "from P");
}

// the cost: made-up marks, as many as the host allows today (64 a plugin) and 256 (fed below the ABI), 5000 ticks of the whole rule each
double perTickMs(int marks) {
    std::mt19937 rng(7);
    std::uniform_real_distribution<double> pos(0.0, 1.0);
    std::uniform_int_distribution<int> len(3, 32);
    std::vector<MarkName> m;
    for (int i = 0; i < marks; ++i) { m.push_back({pos(rng), static_cast<std::size_t>(len(rng))}); }
    const int ticks = 5000;
    std::size_t sink = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < ticks; ++i) { sink += declutter(m, 1000.0, 6.0, 3).tabs.size(); }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / ticks;
    CHECK(sink >= 0);
    const MarkLayout l = declutter(m, 1000.0, 6.0, 3);
    CHECK(noOverlap(m, l, 1000.0, 6.0));
    return ms;
}

void cost() {
    const double c0 = perTickMs(0), c64 = perTickMs(64), c256 = perTickMs(256);
    std::printf("marks tick cost (declutter alone): 0 marks %.5f ms, 64 marks %.5f ms, 256 marks %.5f ms\n", c0, c64, c256);
    CHECK(c256 < 0.5);   // a frame is 33 ms; half a millisecond for the whole rule at 256 marks, one radio, is the bound
    CHECK(c64 < 0.25);
}

}  // namespace

int main() {
    basics();
    notPlaced();
    tabs();
    characters();
    tips();
    cost();
    return testSummary("test_slint_host_marks");
}
