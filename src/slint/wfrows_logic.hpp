// wfrows_logic.hpp - what the waterfall's chrome needs to know about its ROWS (window-gaps lane 4, rows 182, 186, 187).
//
// The classic window's WaterfallView keeps, for every row it was handed, WHEN the row arrived and the dB pair it was mapped through
// (gui/waterfall_view.cpp addLine). From those two it draws three things the Slint components already take as inputs but the host never
// supplied: the reception-gap break (the single row where the picture jumps furthest in age), the strength key's "applies to the top n
// lines" note, and the rail across the picture where the floor and ceiling changed. This is that bookkeeping and the three readings,
// without ImGui or Slint, so a test can hold each rule.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <deque>
#include <limits>
#include <string>

namespace foxsdr::slinthost {

// "45s", "2m30", "1h05": whole seconds taken once, the fields split from that one integer (the classic's formatElapsed).
std::string elapsedText(double seconds);

// The spacing of the age labels for a strip spanning `spanSeconds` with room for `maxLabels` (the classic's timeLabelStep): the smallest value of
// the 1/2/5/10/15/20/30 s, 1/2/5/10/15/30 min, 1 h ladder that is at least span / maxLabels; whole hours beyond; 0 for a span that is not positive.
double timeLabelStep(double spanSeconds, int maxLabels);

class WfRowLog {
public:
    struct Gap {
        int row = -1;                 // the row below the break (counted from the newest, 0); -1 none
        double above = 0.0;           // age of the row above the break, seconds
        double below = 0.0;           // age of the row below it
        bool found() const { return row > 0; }
    };
    struct Ages {
        bool valid = false;
        int rows = 0;
        double newest = 0.0, oldest = 0.0, span = 0.0;
    };

    // The picture's height in rows; forgets every row (a refit, a cleared history).
    void reset(int height) {
        height_ = std::max(0, height);
        rows_.clear();
        rangeRows_ = 0;
        hasRange_ = false;
    }
    // One row has scrolled in at `atS` (any steady clock), mapped through [dbMin, dbMax]. A stamp earlier than the row before it, or not finite, takes that row's.
    void push(double atS, double dbMin, double dbMax) {
        if (height_ <= 0) { return; }
        const bool had = !rows_.empty();
        double stamp = atS;
        if (!std::isfinite(stamp)) { stamp = had ? rows_.front().at : 0.0; }
        if (had && stamp < rows_.front().at) { stamp = rows_.front().at; }
        const bool same = had && rows_.front().lo == dbMin && rows_.front().hi == dbMax;
        hasRange_ = dbMax > dbMin;
        rangeRows_ = same ? std::min(rangeRows_ + 1, height_) : 1;
        rows_.push_front(Row{stamp, dbMin, dbMax});
        while (static_cast<int>(rows_.size()) > height_) { rows_.pop_back(); }
    }

    int height() const { return height_; }
    int filled() const { return static_cast<int>(rows_.size()); }
    bool hasRange() const { return hasRange_; }
    // The run of newest rows that share the newest pair (all the strength key may claim), never more than the rows held.
    int rangeRows() const { return std::min(rangeRows_, filled()); }

    Ages ages(double nowS) const {
        Ages a;
        if (rows_.empty()) { return a; }
        a.valid = true;
        a.rows = filled();
        a.newest = std::max(0.0, nowS - rows_.front().at);
        a.oldest = std::max(a.newest, nowS - rows_.back().at);
        a.span = a.oldest - a.newest;
        return a;
    }
    // Age of a row counted from the newest (0); NaN outside the rows held.
    double rowAge(double nowS, int fromNewest) const {
        if (fromNewest < 0 || fromNewest >= filled()) { return std::numeric_limits<double>::quiet_NaN(); }
        return std::max(0.0, nowS - rows_[static_cast<std::size_t>(fromNewest)].at);
    }
    // THE BREAK: the one row where the picture jumps furthest in age, if that jump is wider than a whole label interval (a steady receiver's rows differ by a
    // frame time and never reach it). `labelRoom` is how many age labels the strip has room for (the classic: strip height / 35 px, 2 to 8).
    Gap gap(double nowS, int labelRoom) const {
        Gap g;
        const Ages a = ages(nowS);
        if (!a.valid || a.rows < 2 || !(a.span > 0.0)) { return g; }
        const double step = timeLabelStep(a.span, std::clamp(labelRoom, 2, 8));
        double widest = 0.0;
        for (int i = 1; i < a.rows; ++i) {
            const double above = rowAge(nowS, i - 1), below = rowAge(nowS, i);
            if (below - above > widest) { widest = below - above; g.row = i; g.above = above; g.below = below; }
        }
        if (!(widest > step)) { g = Gap{}; }
        return g;
    }

private:
    struct Row { double at; double lo, hi; };
    int height_ = 0;
    std::deque<Row> rows_;       // newest first
    int rangeRows_ = 0;
    bool hasRange_ = false;
};

// What the main window's waterfall properties take.
struct WfChromeFig {
    float gapPos = -1.0f;            // 0..1 of the picture height, -1 none
    std::string gapAbove, gapBelow;
    float railPos = -1.0f;           // where the range changed, -1 none
    int coveredRows = 0;             // > 0 only while the key is true of fewer rows than the picture holds
};

// `labelRoom` as for gap(). The break is drawn only where it clears the strip's heading and its foot (the classic's firstY / strip bottom).
WfChromeFig wfChrome(const WfRowLog& log, double nowS, int labelRoom);

}  // namespace foxsdr::slinthost
