// Spectrum line-plot widget: dB bins -> ImDrawList polyline over a dB grid,
// inside the bench's framed, annotated well.
//
// THIS HEADER STAYS FREE OF ImGui TYPES ON PURPOSE. Everything below is a
// pointer, a float or a plain struct of them, so tests/test_spectrum_view.cpp
// can include it and exercise the display math — dbToY, gridlineDbs,
// binToXFrac, hitTest and now peakInBand — with no graphics context at all.
// The moment a member here needs an ImVec2, that whole test file stops
// building, which is the separation doing its job.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

namespace cascade::gui {

// Maps a dB value onto a vertical pixel coordinate: dbMax lands on yTop,
// dbMin on yBottom, linear in between, and values outside [dbMin, dbMax]
// clamp to the corresponding edge so a hot signal or a deep notch never
// escapes the panel. Kept as a free function with no ImGui types because it
// is the one piece of display math the tests must exercise headlessly.
//
// Degenerate ranges (dbMin >= dbMax, or any NaN endpoint) collapse the map:
// every input returns yBottom. Rationale: a zero-height dB range carries no
// ordering information, and pinning to the floor is the same answer clamping
// gives an at-or-below-minimum value — so the degenerate case is continuous
// with the clamped case instead of dividing by zero. A NaN db input likewise
// returns yBottom: a poisoned bin drops to the floor instead of leaking NaN
// into draw coordinates (or spiking to full scale).
float dbToY(float db, float dbMin, float dbMax, float yTop, float yBottom);

class SpectrumView {
public:
    // The VFO band as the caller computed it: fractions of the panel width
    // (0 = left edge, 1 = right edge). Pixel fractions rather than bins or
    // Hz so the overlay and hit test are independent of whatever zoom window
    // the spectrum was drawn with — the caller already solved that mapping.
    struct VfoBand {
        double x0Frac, x1Frac;  // band edges; order does not matter
        bool dragging;          // caller's drag state; brightens the fill
    };

    // One mark on the frequency axis along the bottom of the well. Position
    // is a panel-width fraction for the same reason VfoBand's is: the widget
    // must not need to know the caller's frequency scale to draw its axis.
    //
    // `label` is drawn as-is and is NOT parsed, formatted or unit-converted
    // here. The caller owns the axis's labelling rule (FreqScale::ticks has
    // one, and it is pinned by that class's own tests); a second formatter
    // living here could only ever disagree with it.
    struct AxisTick {
        float xFrac;        // 0 = panel left edge, 1 = right edge
        const char* label;  // nullptr or "" draws the tick with no label
    };

    // WHAT THE PANEL IS ALLOWED TO SAY ABOUT ITSELF.
    //
    // The reference artboard letters this well with six figures: the bin
    // count, the averaging depth, the peak in the passband, how long ago that
    // peak was measured, the frequency axis and the span. Two of them
    // (the bin count, the peak) the widget can measure from what it was
    // handed; the rest belong to objects it cannot see — the estimator's
    // configuration, the wall clock, the frequency scale — and they arrive
    // here or they do not get drawn. NOTHING in this struct has a plausible
    // default that would be printed as a reading: every field's "not
    // supplied" value removes its element from the panel instead.
    //
    // A null Chrome pointer is a legitimate caller state (a panel not yet
    // wired, or a second view with no scale behind it): the well, its frame,
    // the dB axis and the trace still draw, and every annotation that needs
    // an input silently stays off.
    struct Chrome {
        // Line 1 of the header names what is being shown. nullptr or ""
        // takes the default, "SPECTRUM". The bin count is appended from the
        // `n` passed to draw()/drawBinRange() — the widget counts what it was
        // actually given rather than being told.
        const char* title = nullptr;

        // The spectrum estimator's EMA weight: avg = a*newest + (1-a)*avg.
        //
        // ONLY 0 < emaAlpha < 1 IS AN AVERAGE. a == 1 means the estimator is
        // passing each block straight through, so both the "EMA x.xx" term in
        // the header and the "AVERAGE - COMPUTED, NOT HEARD" caveat beneath it
        // are omitted for it — a caveat about averaging on an unaveraged trace
        // is a false statement about the picture, not a harmless extra line.
        // The default (-1) is "not supplied" and behaves the same way.
        //
        // NOTE FOR THE CALLER: this is an EMA weight, not a boxcar depth. The
        // reference artboard prints "AVG 4"; this estimator has no such
        // number, and the two common conversions from alpha (1/a and
        // (2-a)/a) disagree, so the panel prints the weight the estimator was
        // actually configured with and names the mechanism.
        float emaAlpha = -1.0f;

        // The passband, in VfoBand's panel-width fractions, and it must be
        // the SAME band handed to drawVfoOverlay this frame. The header's
        // "PEAK IN PASSBAND" figure is measured over exactly the bins this
        // band covers (see peakInBand) — a peak taken over the whole visible
        // width and captioned "in passband" would be a different measurement
        // wearing this one's name. nullptr omits the whole peak block.
        const VfoBand* passband = nullptr;

        // Seconds since the data behind the trace was published by the DSP —
        // the age of the figures on this panel, not of the GUI frame. Negative
        // or non-finite means "unknown" and drops the line; the peak figure
        // itself still draws, because how old a reading is and whether there
        // is one are different questions.
        double dataAgeSec = -1.0;

        // The frequency axis. `freqTicks` points at freqTickCount marks the
        // caller has already positioned and labelled; null or 0 draws no axis
        // (rather than an unlabelled ruler, which would imply a scale the
        // panel cannot state).
        const AxisTick* freqTicks = nullptr;
        int freqTickCount = 0;

        // Width of the view in Hz, for the boxed SPAN readout at the foot of
        // the well. Zero, negative or non-finite omits the box.
        double spanHz = 0.0;
    };

    // Draws the well and its frame, 10 dB gridlines with labels, one polyline
    // vertex per bin with x spread evenly across `width`, and whatever of the
    // chrome above the caller supplied inputs for. Everything is emitted via
    // the current window's ImDrawList, so this must run inside an ImGui
    // window. n == 0 (or a null dbBins) draws the empty panel — frame, grid
    // and any chrome that does not depend on the data. A single bin (n == 1)
    // has no x extent to spread, so it renders as a flat line across the
    // panel at that bin's level.
    //
    // Implemented as the full-range special case of drawBinRange (window
    // [0, n-1]) so the zoomed and unzoomed paths cannot drift apart.
    void draw(const float* dbBins, int n, float width, float height,
              const Chrome* chrome = nullptr);

    // Zoomed variant of draw(): renders only the fractional-bin window
    // [firstBin, lastBin] across the full panel width, with the trace value
    // linearly interpolated where the window cuts between two bins. Panel
    // furniture (frame, background, grid, labels, chrome, clipping, layout
    // advance) is identical to draw().
    //
    // The window arrives as plain bin numbers — no frequency-scale type —
    // because the zoom controller owns the frequency<->bin mapping and this
    // widget must stay reusable under any axis convention.
    //
    // Defined edge behavior (all headless-testable via binToXFrac):
    //   - Window edges between bins: the edge vertex sits at x = 0 (or
    //     x = width) with the value lerped between the two straddled bins.
    //   - Window partly outside [0, n-1]: only the intersection carries
    //     data; it lands at the sub-span binToXFrac maps it to, and the
    //     out-of-data remainder of the panel shows background/grid only
    //     (no data is invented beyond the first/last bin).
    //   - firstBin == lastBin (finite): a zero-width window still has one
    //     interpolated level, drawn as a flat full-width line — the windowed
    //     analogue of draw()'s n == 1 case.
    //   - Inverted or NaN window: no defined sample positions, so the trace
    //     is omitted (grid-only panel) rather than guessing an ordering.
    void drawBinRange(const float* dbBins, int n, double firstBin, double lastBin,
                      float width, float height, const Chrome* chrome = nullptr);

    // Translucent band fill + edge lines + center line, painted over the
    // panel rectangle recorded by the most recent draw()/drawBinRange() call
    // (that call's layout advance moved the ImGui cursor past the panel, so
    // "current cursor" would point below it). If nothing has been drawn yet
    // it falls back to the current cursor position. Never advances the
    // layout cursor itself: the spectrum item already occupies the space.
    // Band fractions are clamped to [0, 1]; a band entirely outside the
    // panel (or NaN) draws nothing.
    void drawVfoOverlay(const VfoBand& band, float width, float height);

    enum class VfoHit { None, Center, EdgeLow, EdgeHigh };

    // Pure mouse classification for VFO dragging; static so tests can pin
    // the whole decision matrix without a GL context. All inputs share the
    // panel-width-fraction space of VfoBand.
    //
    // Documented rules (the tests rely on these exactly):
    //   - The band is normalized first: lo = min(x0Frac, x1Frac),
    //     hi = max(...), so an inverted band behaves like the ordered one.
    //   - A NaN or negative tolerance is treated as 0 (exact hits only).
    //   - Within tolerance of an edge (inclusive), that edge wins — even
    //     when the mouse is also inside the band, because a narrow band
    //     would otherwise become impossible to resize.
    //   - Within tolerance of BOTH edges (band narrower than 2*tolerance,
    //     including the degenerate zero-width band), the nearer edge wins;
    //     an exact distance tie resolves by side: mouse at or left of the
    //     low edge grabs EdgeLow, otherwise EdgeHigh — so a collapsed band
    //     is pulled open in the direction the mouse approaches from.
    //   - Otherwise inside [lo, hi] is Center (whole-band drag).
    //   - Everything else — including any NaN input — is None.
    static VfoHit hitTest(double mouseXFrac, const VfoBand& band,
                          double edgeToleranceFrac);

    // The bin -> panel-x mapping used by drawBinRange, exposed as a pure
    // static so the windowing math is testable headless: returns
    // (bin - firstBin) / (lastBin - firstBin) as a fraction of the panel
    // width. Deliberately UNclamped — a result outside [0, 1] tells the
    // caller the bin lies outside the visible window. A degenerate window
    // (lastBin <= firstBin, or NaN bounds) returns 0: no span means no
    // ordering, and 0 keeps every vertex at the panel's left edge instead
    // of dividing by zero. draw()'s classic layout is the identity case:
    // binToXFrac(i, 0, n-1) == i / (n-1).
    static float binToXFrac(double bin, double firstBin, double lastBin);

    // THE FIGURE BEHIND "PEAK IN PASSBAND", as a pure static so the claim in
    // that caption can be pinned without a graphics context.
    //
    // Writes the largest dB value among the bins the band covers and returns
    // true; returns false, leaving peakDb untouched, when there is no such
    // measurement. It is the inverse of binToXFrac applied to the band's two
    // fractions, then a max over every WHOLE bin the resulting bin interval
    // touches — floor(lo) through ceil(hi) — so a band narrower than one bin
    // still reports the bins it straddles rather than nothing.
    //
    // Deliberate properties:
    //   - The band is normalized first (order does not matter), exactly as
    //     hitTest and drawVfoOverlay normalize it.
    //   - Bins are clamped to [0, n-1] but the band is NOT clamped to the
    //     visible window: the peak in the passband is a fact about the
    //     signal, not about the zoom, so a VFO parked off-screen still
    //     reports its own peak. A band entirely outside the data returns
    //     false.
    //   - A degenerate view window (lastBin <= firstBin) has no bin ordering
    //     to invert; the panel is then showing the single bin at firstBin, so
    //     that bin's value is reported if the band overlaps the panel at all,
    //     and false otherwise.
    //   - NaN band edges, NaN window bounds, a null/empty bin array, and a
    //     band whose bins are all NaN all return false. A poisoned bin is
    //     skipped rather than compared: NaN loses every comparison, so an
    //     unguarded max would silently report the last non-NaN bin's value
    //     or the NaN itself depending on operand order.
    static bool peakInBand(const float* dbBins, int n, double firstBin, double lastBin,
                           const VfoBand& band, float& peakDb);

    // Display range in dB. Stored verbatim: a degenerate/inverted pair is not
    // swapped or rejected here because dbToY and gridlineDbs already define
    // safe behavior for it, and silently reordering would hide the caller's
    // bug instead of degrading visibly (flat-lined plot, no grid).
    void setRange(float dbMin, float dbMax);

    // HOW MANY 10 dB GRIDLINES TO STEP BETWEEN LABELS on the dB axis, so the
    // ladder cannot collide with itself.
    //
    // THIS EXISTS BECAUSE THE LABELS ARE THE ONLY THING ON THIS PANEL THAT
    // OVERLAPS ITSELF WHEN THE TYPE GROWS. gridlineDbs draws a line every
    // 10 dB whatever the panel is worth; the labels beside them are a fixed
    // number of pixels tall, so their spacing is set by the panel height and
    // the dB range and by nothing else. A 150 px well showing -100..0 dB puts
    // its eleven lines 15 px apart, which at the old 12 px lettering left
    // three pixels of air and at 14 px leaves none — and two dB figures
    // printed through each other are not a coarse scale, they are a smear.
    //
    // The answer is a stride and not a smaller face: the ladder steps to
    // 20 dB, or 50, and stays legible. Labels are drawn where the decade
    // index (db / 10) divides by the stride, so the ladder is anchored on
    // 0 dB and steps in round numbers rather than sliding as the panel is
    // resized.
    //
    // Returns the smallest stride that leaves at least THREE PIXELS of clear
    // air between one label and the next — stride * (pixels per 10 dB) >=
    // labelHeight + 3 — rounded UP to one of {1, 2, 5, 10, 20, 50}, so the
    // figures that survive step by 10, 20, 50, 100, 200 or 500 dB rather than
    // by some arithmetic remainder like 70. Rounding up only widens the gap,
    // so the spacing guarantee holds either way. Nothing on that ladder
    // reaching it means the well is far too short for its range; the answer
    // is then 64, the gridline cap, which leaves at most one figure.
    //
    // A degenerate input — dbMin >= dbMax, a non-positive panel height or
    // label height, any NaN — returns 1, which labels every line exactly as
    // this panel did before the stride existed; the draw loop's own
    // header/axis guards then drop what will not fit.
    static int dbLabelStride(float dbMin, float dbMax, float panelHeight,
                             float labelHeight);

    // Gridline generator, exposed as a pure static so tests can pin down the
    // grid without a GL context. Writes the multiples of 10 dB inside
    // [dbMin, dbMax] — boundary-INCLUSIVE on both ends, so a range of exactly
    // [-100, 0] yields both -100 and 0 — in ascending order, at most `cap`
    // values. Returns the number written. A null `out`, cap <= 0, or a
    // degenerate range (dbMin > dbMax, NaN) returns 0; dbMin == dbMax on an
    // exact multiple of 10 yields that single line.
    static int gridlineDbs(float dbMin, float dbMax, float* out, int cap);

private:
    // Defaults match the estimator's dBFS scaling (0 dBFS full-scale tone,
    // ~-100 dB visible noise floor) so an unconfigured view is already usable.
    float dbMin_ = -100.0f;
    float dbMax_ = 0.0f;

    // Screen-space top-left of the most recently drawn panel, recorded so
    // drawVfoOverlay can paint over it after the layout cursor has moved on.
    // Plain floats (not ImVec2) keep this header free of ImGui types.
    float panelX_ = 0.0f;
    float panelY_ = 0.0f;
    bool panelValid_ = false;
};

}  // namespace cascade::gui
