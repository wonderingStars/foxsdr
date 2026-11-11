// web_viewport.hpp - the maths of the browser waterfall's zoom and pan (feat/webmodels).
//
// A STANDALONE MODEL. The web page's waterfall shows `totalBins` spectrum bins that cover [centreHz - spanHz/2, centreHz + spanHz/2]. A VIEW is
// the window of that row the browser draws: `first` (a fractional bin, so a wheel step can land between bins) and `count` (how many bins fit
// the canvas). Everything here is pure arithmetic on doubles and ints: no clock, no socket, no allocation, nothing printed, so the page's
// JavaScript can be written from this file's rules and the tests drive every edge (a NaN from a bad pointer event, a zero-width canvas, a
// view pushed past either end).
//
// RULES (each has a test in tests/test_web_viewport.cpp):
//   1. A view is always inside [0, totalBins]: `count` is between min(kMinVisibleBins, totalBins) and totalBins, and `first` is between 0 and
//      totalBins - count. clampView() enforces both, and every function below returns a clamped view.
//   2. A view with a non-finite member, or on an empty row (totalBins <= 0), becomes the full view (or an empty one for no bins): a hostile or
//      broken pointer event can never produce a NaN that reaches the canvas.
//   3. zoomAt() keeps the bin under the pointer where it was (the desktop's behaviour): zooming in then out at the same pointer returns the
//      same view (to rounding) while neither end clamps.
//   4. panByPixels() moves the picture WITH the finger: a drag to the right shows lower frequencies.
//   5. pointerToHz() and hzToPointer() are inverses of each other on the view, and the pointer is clamped to the canvas first.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstdint>

namespace cascade::net {

// The narrowest view the browser may zoom to, in bins. Below this the picture is a few fat blocks and the wheel feels stuck.
inline constexpr double kMinVisibleBins = 16.0;
// One wheel notch (a deltaY of 100 pixels) zooms by this factor; the factor for a larger or smaller delta is this to the power of delta/100.
inline constexpr double kWheelZoomPerNotch = 1.25;
// A single wheel event may not zoom by more than this factor either way (a trackpad flick reports thousands of pixels).
inline constexpr double kMaxZoomStep = 4.0;

struct SpectrumSpan {
    std::int64_t totalBins = 0;
    double centreHz = 0.0;
    double spanHz = 0.0;  // the whole row
};

struct View {
    double first = 0.0;
    double count = 0.0;
    bool operator==(const View& o) const { return first == o.first && count == o.count; }
};

// The whole row.
View fullView(std::int64_t totalBins);

// Rules 1 and 2.
View clampView(View v, std::int64_t totalBins);

// Zoom by `factor` (> 1 zooms IN, < 1 OUT) keeping the bin under `anchor` (0 = the left edge of the view, 1 = the right edge) fixed.
// A factor that is not finite or not positive leaves the view as it was (clamped).
View zoomAt(View v, std::int64_t totalBins, double anchor, double factor);

// The zoom factor for a wheel event of `deltaY` pixels (up, negative, zooms in). Zero, non-finite, or a delta whose factor rounds to 1 gives 1.
double wheelFactor(double deltaY);

// Drag by `dxPixels` on a canvas `widthPixels` wide. A zero or negative width, or a non-finite delta, leaves the view as it was.
View panByPixels(View v, std::int64_t totalBins, double dxPixels, double widthPixels);

// Move by whole bins (the keyboard's arrow keys).
View panByBins(View v, std::int64_t totalBins, double deltaBins);

// The frequency under a pointer `x` pixels from the canvas's left edge on a canvas `widthPixels` wide. `x` is clamped to the canvas.
// Returns the span's low edge when the canvas has no width or the span is empty.
double pointerToHz(View v, const SpectrumSpan& span, double x, double widthPixels);

// The pointer position (pixels, may be outside [0, width] when the frequency is outside the view) of `hz`.
double hzToPointer(View v, const SpectrumSpan& span, double hz, double widthPixels);

// The view that shows [loHz, hiHz] (swapped if reversed, clamped to the row).
View viewForRange(const SpectrumSpan& span, double loHz, double hiHz);

// The integer bins [firstBin, endBin) a view touches, for the server side of a "send only what is visible" reply: always inside
// [0, totalBins], endBin >= firstBin.
struct BinRange {
    std::int64_t firstBin = 0;
    std::int64_t endBin = 0;
};
BinRange visibleBins(View v, std::int64_t totalBins);

}  // namespace cascade::net
