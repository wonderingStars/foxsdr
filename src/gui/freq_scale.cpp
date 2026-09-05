// Frequency-axis view transform — implementation.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/freq_scale.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace cascade::gui {

namespace {

// The pitch is set by the WIDEST label the axis can produce and by what the
// panel does with the two END labels. Measured over every band, rate and zoom
// this product reaches (tests/test_spectrum_waterfall_type.cpp), the widest
// label is 65.55 px at the 17 px lettering of 0.79.0. A label is centred on
// its tick, but the panel slides the two end labels inboard so the frame does
// not slice them - by up to half a label - which can carry an end label into
// its neighbour. So the pitch has to hold a whole label, half a label of slide
// and the 3 px gap: 1.5 * 65.55 + 3 = 101.3, and 104 is that with a margin.
// (80 px was enough for the 54 px labels of 14 px lettering; the 17 px type
// dropped two labels in 109 of the 6560 measured cases at 80.)
constexpr double kMinTickSpacingPx = 104.0;

// The zoom floor as a fraction of the full span. Deeper zoom shows single
// FFT bins as flat plateaus — no extra information, plenty of numeric risk.
constexpr double kMinSpanFraction = 1e-3;

// Smallest step from {1, 2, 5} * 10^k that is >= minHz (minHz > 0, finite).
// Also outputs the chosen step's power-of-ten exponent so the label
// formatter can derive its decimal count without a second, rounding-prone
// log10 of the step itself.
double niceStepAtLeast(double minHz, int& exp10) {
    const int e = static_cast<int>(std::floor(std::log10(minHz)));
    const double base = std::pow(10.0, static_cast<double>(e));
    // Relative slack: a minHz that IS a nice value up to FP noise must not
    // get bumped a whole mantissa notch up (100.00000001 px pitch is fine;
    // jumping from 80 px to 200 px pitch is not).
    const double target = minHz * (1.0 - 1e-9);
    exp10 = e;
    if (base >= target) { return base; }
    if (2.0 * base >= target) { return 2.0 * base; }
    if (5.0 * base >= target) { return 5.0 * base; }
    exp10 = e + 1;
    return 10.0 * base;
}

}  // namespace

void FreqScale::setSpan(double centerHz, double sampleRateHz) {
    if (!std::isfinite(centerHz) || !std::isfinite(sampleRateHz) ||
        !(sampleRateHz > 0.0)) {
        // Collapse to the inert state instead of storing a poisoned span:
        // every member has defined inert behavior, whereas a NaN bound would
        // leak into draw coordinates.
        rate_ = 0.0;
        fullLow_ = fullHigh_ = viewLow_ = viewHigh_ =
            std::isfinite(centerHz) ? centerHz : 0.0;
        return;
    }
    const double newLow = centerHz - 0.5 * sampleRateHz;
    const double newHigh = centerHz + 0.5 * sampleRateHz;
    // Keep the user's zoom across a retune when it still satisfies the class
    // invariant against the NEW span: entirely inside it and no narrower
    // than the new zoom floor. (The floor check also covers the very first
    // setSpan, where the inert zero-width view can never qualify.)
    const bool keepView = viewLow_ >= newLow && viewHigh_ <= newHigh &&
                          (viewHigh_ - viewLow_) >= sampleRateHz * kMinSpanFraction;
    fullLow_ = newLow;
    fullHigh_ = newHigh;
    rate_ = sampleRateHz;
    if (!keepView) {
        viewLow_ = newLow;
        viewHigh_ = newHigh;
    }
}

void FreqScale::zoomAt(double frac01, double factor) {
    if (!(rate_ > 0.0)) { return; }
    if (!std::isfinite(frac01) || !std::isfinite(factor) || !(factor > 0.0)) {
        return;
    }
    const double f = std::clamp(frac01, 0.0, 1.0);
    const double span = viewHigh_ - viewLow_;
    // The frequency under the cursor — the point that must not move.
    const double anchorHz = viewLow_ + f * span;
    double newSpan = span / factor;
    newSpan = std::clamp(newSpan, rate_ * kMinSpanFraction, fullHigh_ - fullLow_);
    // Place the new window so the anchor keeps its fractional position; only
    // the shift back inside the full span below may override that, because
    // showing frequencies outside the baseband is never acceptable.
    double lo = anchorHz - f * newSpan;
    double hi = lo + newSpan;
    if (lo < fullLow_) {
        hi += fullLow_ - lo;
        lo = fullLow_;
    }
    if (hi > fullHigh_) {
        lo -= hi - fullHigh_;
        hi = fullHigh_;
        // Shifting left can undershoot by an ulp when newSpan == full span;
        // re-pin rather than let the invariant drift.
        if (lo < fullLow_) { lo = fullLow_; }
    }
    viewLow_ = lo;
    viewHigh_ = hi;
}

void FreqScale::pan(double fracDelta) {
    if (!(rate_ > 0.0) || !std::isfinite(fracDelta)) { return; }
    const double span = viewHigh_ - viewLow_;
    double lo = viewLow_ + fracDelta * span;
    // Pin the offending edge exactly (span preserved). Left clamp runs last:
    // span <= full span, so lo = fullLow can only overshoot the high edge by
    // rounding, while an unclamped lo could be arbitrarily wrong.
    if (lo + span > fullHigh_) { lo = fullHigh_ - span; }
    if (lo < fullLow_) { lo = fullLow_; }
    viewLow_ = lo;
    viewHigh_ = lo + span;
}

void FreqScale::resetView() {
    viewLow_ = fullLow_;
    viewHigh_ = fullHigh_;
}

void FreqScale::visibleBinRange(int fftSize, double& firstBin, double& lastBin) const {
    if (fftSize <= 0) {
        firstBin = 0.0;
        lastBin = 0.0;
        return;
    }
    const double fullSpan = fullHigh_ - fullLow_;
    if (!(fullSpan > 0.0)) {
        // No view constraint -> show everything; the renderer then draws the
        // whole spectrum, which is the least surprising inert behavior.
        firstBin = 0.0;
        lastBin = static_cast<double>(fftSize - 1);
        return;
    }
    // Bin 0 at fullLow, bin N-1 at fullHigh — the same spread SpectrumView
    // uses for its polyline, so a full view is exactly [0, N-1] and slices
    // line up with what is on screen.
    const double binsPerHz = static_cast<double>(fftSize - 1) / fullSpan;
    firstBin = (viewLow_ - fullLow_) * binsPerHz;
    lastBin = (viewHigh_ - fullLow_) * binsPerHz;
}

int FreqScale::ticks(double widthPx, double* tickHz, char (*labels)[16], int cap) const {
    if (tickHz == nullptr || labels == nullptr || cap <= 0) { return 0; }
    if (!std::isfinite(widthPx) || !(widthPx > 0.0)) { return 0; }
    const double span = viewHigh_ - viewLow_;
    if (!(span > 0.0)) { return 0; }

    int stepExp = 0;
    const double step = niceStepAtLeast(span * kMinTickSpacingPx / widthPx, stepExp);

    // First/last step multiples inside the view. ceil/floor decide from a
    // rounded quotient, so re-check against the actual products — a tick one
    // ulp outside the view would draw in the neighboring panel's margin.
    double n0 = std::ceil(viewLow_ / step);
    double n1 = std::floor(viewHigh_ / step);
    while (n0 * step < viewLow_) { n0 += 1.0; }
    while (n1 * step > viewHigh_) { n1 -= 1.0; }

    // One unit and one decimal count for the whole axis (see header): unit
    // from the larger endpoint magnitude, decimals from the step, so labels
    // form aligned columns with no noise digits.
    const double maxAbs = std::max(std::fabs(viewLow_), std::fabs(viewHigh_));
    const char* unitName = "Hz";
    int unitExp = 0;
    if (maxAbs >= 1e9) {
        unitName = "GHz";
        unitExp = 9;
    } else if (maxAbs >= 1e6) {
        unitName = "MHz";
        unitExp = 6;
    } else if (maxAbs >= 1e3) {
        unitName = "kHz";
        unitExp = 3;
    }
    const double unit = std::pow(10.0, static_cast<double>(unitExp));
    // Clamped so the value part stays within the 16-char buffer even in
    // absurd regimes (sub-Hz steps on a GHz axis).
    const int decimals = std::clamp(unitExp - stepExp, 0, 9);

    int count = 0;
    for (double n = n0; n <= n1 && count < cap; n += 1.0) {
        double hz = n * step;
        // n == -0.0 would print as "-0.0 MHz"; normalize the sign away.
        if (hz == 0.0) { hz = 0.0; }
        tickHz[count] = hz;
        std::snprintf(labels[count], sizeof labels[count], "%.*f %s", decimals,
                      hz / unit, unitName);
        ++count;
    }
    return count;
}

}  // namespace cascade::gui
