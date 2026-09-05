// Frequency-axis view transform shared by the spectrum and waterfall panels.
//
// One object owns the mapping between widget x coordinates, absolute Hz, and
// fftshifted bin indices, so the two panels can never disagree about what
// frequency a pixel column shows. Pure math + snprintf only — no ImGui types
// anywhere, so every behavior is testable headless.
//
// State model: a FULL span (the tuned baseband, [center - rate/2,
// center + rate/2]) and a VIEW window inside it. Class invariant after any
// mutator: fullLow <= viewLow < viewHigh <= fullHigh and
// viewSpan >= sampleRate/1000 (the zoom floor — deeper zoom than 1/1000 of
// the baseband shows individual FFT bins as flat plateaus, which carries no
// extra information and invites numeric trouble).
//
// A default-constructed scale is inert (zero span everywhere): mutators
// no-op, ticks() returns 0, and the transforms return defined values
// (documented per member) instead of dividing by zero.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

namespace cascade::gui {

class FreqScale {
public:
    // Sets the full baseband span [centerHz - rate/2, centerHz + rate/2].
    // The current view is PRESERVED when it still satisfies the class
    // invariant against the new span (entirely inside it, and at least the
    // new minimum span rate/1000 wide) — so a small retune does not throw
    // away the user's zoom. Otherwise the view resets to the full span.
    // A non-finite or non-positive rate (or non-finite center) collapses the
    // scale to the inert default state rather than storing a poisoned span.
    void setSpan(double centerHz, double sampleRateHz);

    // Zooms about a fractional widget x position: the Hz under frac01 stays
    // under frac01 afterwards (the scroll-wheel-zoom-at-cursor contract).
    // factor > 1 zooms IN (view span divided by factor). The new span clamps
    // to [sampleRate/1000, full span]; the new window is then shifted (not
    // shrunk) back inside the full span, which is the only case where the
    // cursor invariant intentionally gives way — the view must never show
    // frequencies outside the baseband. frac01 is clamped into [0, 1];
    // non-finite frac01/factor or factor <= 0 no-op.
    void zoomAt(double frac01, double factor);

    // Shifts the view by fracDelta * (current view span), clamped so the view
    // stays inside the full span (span preserved, offending edge pinned).
    // Non-finite fracDelta no-ops.
    void pan(double fracDelta);

    // View := full span. (Also the state setSpan resets to.)
    void resetView();

    double viewLowHz() const { return viewLow_; }
    double viewHighHz() const { return viewHigh_; }

    // Fractional widget x -> absolute Hz, as a pure linear map: 0 -> viewLow,
    // 1 -> viewHigh. Deliberately unclamped so callers can extrapolate (e.g.
    // dragging past the widget edge); inert scale returns viewLow.
    double xToHz(double frac01) const {
        return viewLow_ + frac01 * (viewHigh_ - viewLow_);
    }

    // Absolute Hz -> fractional x; may land outside [0, 1] when hz is
    // off-view, which is exactly how a caller culls off-screen markers.
    // Inert scale (zero view span) returns 0.0 for every input.
    double hzToX(double hz) const {
        const double span = viewHigh_ - viewLow_;
        if (!(span > 0.0)) { return 0.0; }
        return (hz - viewLow_) / span;
    }

    // Which slice of the fftshifted spectrum the view shows, as FRACTIONAL
    // bin indices so a renderer can interpolate during smooth zoom.
    //
    // Convention (matches how SpectrumView spreads its polyline): bin 0 sits
    // at the full span's low edge and bin fftSize-1 at its high edge, linear
    // in between. Full view therefore reports exactly [0, fftSize-1]; a view
    // covering the lower half reports [0, (fftSize-1)/2]. fftSize <= 0
    // reports [0, 0]; an inert scale reports the whole range [0, fftSize-1]
    // ("no view constraint" degrades to "show everything").
    void visibleBinRange(int fftSize, double& firstBin, double& lastBin) const;

    // Axis ticks for the current view. Writes up to `cap` tick frequencies
    // (ascending, each an exact multiple of the chosen step, all inside the
    // view) plus their labels; returns the count written.
    //
    // Step rule: the smallest "nice" step from {1, 2, 5} * 10^k whose pixel
    // pitch at widthPx is >= 104 px — the widest label the axis produces is
    // 65.55 px at the current lettering, an end label may slide inboard by
    // half of that to clear the frame, and 3 px of gap must remain (see
    // kMinTickSpacingPx in freq_scale.cpp for the arithmetic).
    //
    // Label rule (ONE rule, pinned by tests): every label on the axis shares
    // one unit, chosen from the larger view-endpoint magnitude — >= 1 GHz ->
    // "GHz", >= 1 MHz -> "MHz", >= 1 kHz -> "kHz", else "Hz" — and one
    // decimal count: exactly enough digits to render the step in that unit
    // (clamped to [0, 9] so a label always fits its 16-char buffer), so
    // columns line up and no label carries noise digits. Format:
    // "<value> <unit>", e.g. step 100 kHz on a 100 MHz axis -> "100.3 MHz";
    // step 20 kHz on a 500 kHz axis -> "420 kHz".
    //
    // Null pointers, cap <= 0, non-positive/non-finite widthPx, or an inert
    // scale return 0.
    int ticks(double widthPx, double* tickHz, char (*labels)[16], int cap) const;

private:
    double fullLow_ = 0.0;   // center - rate/2
    double fullHigh_ = 0.0;  // center + rate/2
    double rate_ = 0.0;      // > 0 iff the scale is live
    double viewLow_ = 0.0;
    double viewHigh_ = 0.0;
};

}  // namespace cascade::gui
