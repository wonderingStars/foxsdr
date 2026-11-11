// zoom_select.hpp - ZOOM TO A DRAGGED SELECTION on the spectrum / waterfall, as pure arithmetic: a rubber-band rectangle dragged over the plot becomes
// the new centre, span and dB range of the view. No clock, no window, no allocation; the window code only has to pass the rectangle in pixels.
//
// THE RULES (each tested):
//   - The rectangle is in plot pixels, origin top-left, y down; it is clamped to the plot and a drag from right to left or bottom to top is the same
//     rectangle. Frequency follows x linearly across the CURRENT view; dB follows y linearly from topDb (y = 0) to bottomDb (y = plotH).
//   - A CLICK is not a zoom: when the rectangle is narrower than kMinDragPx AND shorter than kMinDragPx nothing changes.
//   - A THIN rectangle zooms one axis only: narrower than kMinDragPx = the dB range zooms and the frequency view stays; shorter than kMinDragPx = the
//     frequency zooms and the dB range stays. ZoomAxes::FrequencyOnly always leaves the dB range alone (the waterfall has none).
//   - MINIMUM SPAN: the new span is never below limits.minSpanHz; a smaller selection is widened about its own centre. MINIMUM dB RANGE likewise.
//   - The new view never leaves the full range: the span is capped at the full span and the centre is shifted back inside [fullLo, fullHi]; the dB
//     range is shifted back inside [dbMin, dbMax] (range capped at dbMax - dbMin).
//   - Anything not finite, a plot of no size or a view of no span returns changed = false and the view as it was.
// ViewHistory keeps the views that were left (depth kViewHistoryDepth, oldest dropped) so a "back" key can return; zoomOut() widens about the centre.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace cascade::core {

struct SpectrumView {
    double centreHz = 0.0;
    double spanHz = 0.0;
    double topDb = 0.0;
    double bottomDb = -100.0;
    bool operator==(const SpectrumView&) const = default;
};

struct ZoomLimits {
    double fullLoHz = 0.0;       // the whole capture (or tuning range) the view must stay inside
    double fullHiHz = 0.0;
    double minSpanHz = 100.0;
    double dbMin = -160.0;
    double dbMax = 20.0;
    double minDbRange = 3.0;
};

struct DragRect {
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // plot pixels, any corner order
};

enum class ZoomAxes { Both, FrequencyOnly };
inline constexpr double kMinDragPx = 6.0;

struct ZoomResult {
    bool changed = false;
    SpectrumView view;
};

namespace zoom_detail {
inline bool fin(double v) { return std::isfinite(v); }

// Fits [lo, hi] inside [fullLo, fullHi] keeping the width if it can.
inline void fitInside(double& lo, double& hi, double fullLo, double fullHi) {
    const double w = std::min(hi - lo, fullHi - fullLo);
    if (lo < fullLo) { lo = fullLo; }
    hi = lo + w;
    if (hi > fullHi) { hi = fullHi; lo = hi - w; }
    hi = lo + w;
}
}  // namespace zoom_detail

inline bool viewIsSane(const SpectrumView& v) {
    return zoom_detail::fin(v.centreHz) && zoom_detail::fin(v.spanHz) && v.spanHz > 0.0 && zoom_detail::fin(v.topDb) && zoom_detail::fin(v.bottomDb) &&
           v.topDb > v.bottomDb;
}

inline ZoomResult zoomToSelection(const SpectrumView& cur, const DragRect& drag, double plotW, double plotH, const ZoomLimits& lim,
                                  ZoomAxes axes = ZoomAxes::Both) {
    using namespace zoom_detail;
    ZoomResult res;
    res.view = cur;
    if (!viewIsSane(cur) || !fin(plotW) || !fin(plotH) || plotW <= 0.0 || plotH <= 0.0 || !fin(drag.x0) || !fin(drag.y0) || !fin(drag.x1) || !fin(drag.y1) ||
        !fin(lim.fullLoHz) || !fin(lim.fullHiHz) || !(lim.fullHiHz > lim.fullLoHz) || !fin(lim.minSpanHz) || !fin(lim.dbMin) || !fin(lim.dbMax) ||
        !(lim.dbMax > lim.dbMin) || !fin(lim.minDbRange)) {
        return res;
    }
    const double xa = std::clamp(std::min(drag.x0, drag.x1), 0.0, plotW), xb = std::clamp(std::max(drag.x0, drag.x1), 0.0, plotW);
    const double ya = std::clamp(std::min(drag.y0, drag.y1), 0.0, plotH), yb = std::clamp(std::max(drag.y0, drag.y1), 0.0, plotH);
    const bool wide = (xb - xa) >= kMinDragPx;
    const bool tall = (yb - ya) >= kMinDragPx && axes == ZoomAxes::Both;
    if (!wide && !tall) { return res; }  // a click
    SpectrumView out = cur;
    if (wide) {
        const double lo0 = cur.centreHz - cur.spanHz / 2.0;
        double span = (xb - xa) / plotW * cur.spanHz;
        double lo = lo0 + xa / plotW * cur.spanHz;
        double centre = lo + span / 2.0;
        const double fullSpan = lim.fullHiHz - lim.fullLoHz;
        span = std::min(std::max(span, std::max(lim.minSpanHz, 1e-9)), fullSpan);
        double a = centre - span / 2.0, b = centre + span / 2.0;
        fitInside(a, b, lim.fullLoHz, lim.fullHiHz);
        out.centreHz = (a + b) / 2.0;
        out.spanHz = b - a;
    }
    if (tall) {
        const double range = cur.topDb - cur.bottomDb;
        double top = cur.topDb - ya / plotH * range;
        double bottom = cur.topDb - yb / plotH * range;
        const double fullRange = lim.dbMax - lim.dbMin;
        double r = std::min(std::max(top - bottom, std::min(std::max(lim.minDbRange, 1e-6), fullRange)), fullRange);
        const double mid = (top + bottom) / 2.0;
        bottom = mid - r / 2.0;
        top = mid + r / 2.0;
        fitInside(bottom, top, lim.dbMin, lim.dbMax);
        out.topDb = top;
        out.bottomDb = bottom;
    }
    res.changed = !(out == cur);
    res.view = out;
    return res;
}

// Widens the view by `factor` (> 1) about its centre, kept inside the limits; the dB range is left alone.
inline SpectrumView zoomOut(const SpectrumView& cur, double factor, const ZoomLimits& lim) {
    if (!viewIsSane(cur) || !zoom_detail::fin(factor) || factor <= 1.0 || !(lim.fullHiHz > lim.fullLoHz)) { return cur; }
    SpectrumView v = cur;
    const double span = std::min(cur.spanHz * factor, lim.fullHiHz - lim.fullLoHz);
    double a = cur.centreHz - span / 2.0, b = cur.centreHz + span / 2.0;
    zoom_detail::fitInside(a, b, lim.fullLoHz, lim.fullHiHz);
    v.centreHz = (a + b) / 2.0;
    v.spanHz = b - a;
    return v;
}

inline constexpr std::size_t kViewHistoryDepth = 16;

class ViewHistory {
public:
    // Remembers `v` (the view being left). A view equal to the newest entry is not stored twice.
    void push(const SpectrumView& v) {
        if (n_ > 0 && items_[(head_ + kViewHistoryDepth - 1) % kViewHistoryDepth] == v) { return; }
        items_[head_] = v;
        head_ = (head_ + 1) % kViewHistoryDepth;
        if (n_ < kViewHistoryDepth) { ++n_; }
    }
    bool pop(SpectrumView& out) {
        if (n_ == 0) { return false; }
        head_ = (head_ + kViewHistoryDepth - 1) % kViewHistoryDepth;
        out = items_[head_];
        --n_;
        return true;
    }
    std::size_t size() const { return n_; }
    void clear() { n_ = 0; head_ = 0; }

private:
    std::array<SpectrumView, kViewHistoryDepth> items_{};
    std::size_t head_ = 0, n_ = 0;
};

}  // namespace cascade::core
