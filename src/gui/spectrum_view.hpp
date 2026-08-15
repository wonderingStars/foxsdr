// Spectrum line-plot widget: dB bins -> ImDrawList polyline over a dB grid.
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
    // Draws background, 10 dB gridlines with labels, and one polyline vertex
    // per bin with x spread evenly across `width`. Everything is emitted via
    // the current window's ImDrawList, so this must run inside an ImGui
    // window. n == 0 (or a null dbBins) draws the empty panel — background
    // and grid only. A single bin (n == 1) has no x extent to spread, so it
    // renders as a flat line across the panel at that bin's level.
    //
    // Implemented as the full-range special case of drawBinRange (window
    // [0, n-1]) so the zoomed and unzoomed paths cannot drift apart.
    void draw(const float* dbBins, int n, float width, float height);

    // Zoomed variant of draw(): renders only the fractional-bin window
    // [firstBin, lastBin] across the full panel width, with the trace value
    // linearly interpolated where the window cuts between two bins. Panel
    // furniture (background, grid, labels, clipping, layout advance) is
    // identical to draw().
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
                      float width, float height);

    // The VFO band as the caller computed it: fractions of the panel width
    // (0 = left edge, 1 = right edge). Pixel fractions rather than bins or
    // Hz so the overlay and hit test are independent of whatever zoom window
    // the spectrum was drawn with — the caller already solved that mapping.
    struct VfoBand {
        double x0Frac, x1Frac;  // band edges; order does not matter
        bool dragging;          // caller's drag state; brightens the fill
    };

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

    // Display range in dB. Stored verbatim: a degenerate/inverted pair is not
    // swapped or rejected here because dbToY and gridlineDbs already define
    // safe behavior for it, and silently reordering would hide the caller's
    // bug instead of degrading visibly (flat-lined plot, no grid).
    void setRange(float dbMin, float dbMax);

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
