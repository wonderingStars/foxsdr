// Spectrum line-plot widget: dB bins -> ImDrawList polyline over a dB grid.
//
// SPDX-License-Identifier: MIT
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
    void draw(const float* dbBins, int n, float width, float height);

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
};

}  // namespace cascade::gui
