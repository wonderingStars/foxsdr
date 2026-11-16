// The waterfall's row log (window-gaps lane 4, rows 182, 186, 187): the reception-gap break, the "applies to the top n lines" run and the range-changed rail.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cmath>
#include "wfrows_logic.hpp"
#include "test_check.hpp"

using namespace foxsdr::slinthost;

int main() {
    CHECK(elapsedText(45) == "45s");
    CHECK(elapsedText(150) == "2m30");
    CHECK(elapsedText(120) == "2m");
    CHECK(elapsedText(3900) == "1h05");
    CHECK(elapsedText(0) == "0s");
    CHECK(elapsedText(-3) == "0s");
    CHECK(elapsedText(1e12) == "99h59");
    CHECK(timeLabelStep(100, 5) == 20.0);
    CHECK(timeLabelStep(0, 5) == 0.0);
    CHECK(timeLabelStep(100, 0) == 0.0);
    CHECK(timeLabelStep(100000, 4) == 25200.0);

    // a steady receiver: a row every 0.1 s, one range: no break, no rail, no coverage note
    {
        WfRowLog log;
        log.reset(100);
        for (int i = 0; i < 100; ++i) { log.push(i * 0.1, -110, 0); }
        const WfChromeFig f = wfChrome(log, 10.0, 6);
        CHECK(f.gapPos < 0);
        CHECK(f.railPos < 0);
        CHECK(f.coveredRows == 0);
        CHECK(log.filled() == 100);
        CHECK(log.rangeRows() == 100);
    }
    // a ten-minute silence in the middle: the break is at the row after it, with the ages either side
    {
        WfRowLog log;
        log.reset(100);
        for (int i = 0; i < 50; ++i) { log.push(i * 0.1, -110, 0); }
        for (int i = 0; i < 50; ++i) { log.push(600.0 + i * 0.1, -110, 0); }
        const double now = 605.0;
        const WfChromeFig f = wfChrome(log, now, 6);
        CHECK(f.gapPos > 0.49 && f.gapPos < 0.51);
        CHECK(!f.gapAbove.empty());
        CHECK(f.gapBelow == "10m");
        const WfRowLog::Gap g = log.gap(now, 6);
        CHECK(g.row == 50);
        CHECK_NEAR(g.above, 5.0, 1e-9);
        CHECK_NEAR(g.below, now - 4.9, 1e-9);
    }
    // a break right under the strip's heading or at its foot is not drawn
    {
        WfRowLog log;
        log.reset(100);
        for (int i = 0; i < 99; ++i) { log.push(i * 0.1, -110, 0); }
        log.push(900.0, -110, 0);
        CHECK(wfChrome(log, 900.1, 6).gapPos < 0);    // the jump is at row 1
        WfRowLog log2;
        log2.reset(100);
        log2.push(0.0, -110, 0);
        for (int i = 0; i < 99; ++i) { log2.push(900.0 + i * 0.1, -110, 0); }
        CHECK(wfChrome(log2, 910.0, 6).gapPos < 0);   // the jump is at the last row
    }
    // the floor moves: the key claims only the newest rows, and the rail sits where the mapping changed
    {
        WfRowLog log;
        log.reset(100);
        for (int i = 0; i < 60; ++i) { log.push(i * 0.1, -110, 0); }
        for (int i = 0; i < 30; ++i) { log.push(6.0 + i * 0.1, -90, -10); }
        CHECK(log.rangeRows() == 30);
        const WfChromeFig f = wfChrome(log, 9.0, 6);
        CHECK(f.coveredRows == 30);
        CHECK_NEAR(f.railPos, 0.30, 1e-6);
        // once the run has grown past the ring the rail is gone
        for (int i = 0; i < 100; ++i) { log.push(9.0 + i * 0.1, -90, -10); }
        const WfChromeFig g = wfChrome(log, 20.0, 6);
        CHECK(g.railPos < 0);
        CHECK(g.coveredRows == 0);
    }
    // a degenerate range names no range; a reset forgets
    {
        WfRowLog log;
        log.reset(10);
        log.push(0, 0, 0);
        CHECK(!log.hasRange());
        log.push(1, -100, 0);
        CHECK(log.hasRange());
        log.reset(10);
        CHECK(log.filled() == 0);
        CHECK(!log.ages(5).valid);
        CHECK(wfChrome(log, 5, 6).gapPos < 0);
        log.reset(0);
        log.push(1, -100, 0);
        CHECK(log.filled() == 0);
    }
    // stamps are forced non-decreasing; a NaN stamp keeps the row before
    {
        WfRowLog log;
        log.reset(5);
        log.push(10, -100, 0);
        log.push(5, -100, 0);
        log.push(std::nan(""), -100, 0);
        CHECK_NEAR(log.rowAge(12, 0), 2.0, 1e-9);
        CHECK_NEAR(log.rowAge(12, 2), 2.0, 1e-9);
        CHECK(std::isnan(log.rowAge(12, 3)));
    }
    return testSummary("test_slint_host_wfrows");
}
