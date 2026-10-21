// passband_slide.hpp - SHIFT + WHEEL OVER THE SPECTRUM OR THE WATERFALL: where the tuned
// frequency goes. No ImGui.
//
// WHY IT EXISTS (0.99.82). A Linux user asked for the wheel, with a modifier, to slide the
// demodulator's passband along the band the way a tuning knob does - without a second gesture
// for "tune" and a third for "scroll". The plain wheel over the picture zooms and the pinch
// (Ctrl + wheel) zooms; the left button tunes and drags; the right opens the display menu.
// Shift is free on the wheel, and it already means "off the raster" on a click, which is the
// nearest thing in this window to an "I am being fine about it" key.
//
// WHAT A NOTCH IS. One step of the MODE'S RASTER - the same table a click and a drag snap to
// (kModeSnapHz in app_window.cpp: NFM 12.5 kHz, WFM 100 kHz, AM 9 kHz, the SSB and CW modes
// 1 kHz) - so a notch from a signal tuned by a click lands on the next channel, and a hand that
// has been Shift-clicking off the raster is brought back onto it by its first notch rather than
// moved a whole step from where it was. Up (a wheel turned away from the user) is up in
// frequency, as on the counter's digits.
//
// WHERE IT GOES. Inside the span the source delivers, the VFO moves and the source does not:
// the same move a click makes. Past the end of the VFO's reach - the span less half the filter
// at each end, the limit every VFO move is held to - the PASSBAND STAYS AT THE END and the
// SOURCE is carried the rest of the way, through the same retune the counter's wheel and the
// tuning keys use, so a wheel turned for long enough walks across the whole band. The
// alternative, putting the passband back in the middle of the new span at the first notch past
// the end, was rejected: it jumps the picture by half a span on one notch and makes the
// source retune for a move of one raster step; this retunes by exactly the step.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <cmath>

namespace cascade::gui {

struct PassbandSlide {
    bool moved = false;          // anything to do
    double wantAbsHz = 0.0;      // where the tuned frequency ends up
    double offsetHz = 0.0;       // the VFO's offset from the source's centre afterwards
    bool retuneSource = false;   // the source has to move to get there
    double centreHz = 0.0;       // ...to this centre (valid when retuneSource)
};

// The raster point `notches` steps from `tunedHz`: a tuned frequency already on the raster moves
// by exactly `notches` steps, one off it lands on the nearest point in the direction of the first
// notch (so up from 100.0004 MHz on a 1 kHz raster is 100.001, and down from it is 100.000).
// Non-finite input, a non-positive step or no notches answer `tunedHz` unchanged.
inline double passbandRasterStep(double tunedHz, double stepHz, double notches) {
    if (!std::isfinite(tunedHz) || !(stepHz > 0.0) || !std::isfinite(notches) || notches == 0.0) {
        return tunedHz;
    }
    // A tolerance of a millionth of a step: a frequency that is on the raster up to the noise a
    // round trip through a double leaves must not count as off it.
    constexpr double kEps = 1e-6;
    const double q = tunedHz / stepHz;
    const double whole = std::trunc(notches);
    const double base = notches > 0.0 ? std::floor(q + kEps) : std::ceil(q - kEps);
    return (base + whole) * stepHz;
}

// Plans a slide of `notches` raster steps from the tuned frequency `tunedHz` (= centreHz + the
// VFO's offset). `rateHz` and `bandwidthHz` are the span the source delivers and the filter's
// width, which fix the VFO's reach; `minTunedHz` is the lowest tuned frequency the tuning
// controls may reach (core::minTunedAirHz) - a slide is stopped there, not wrapped.
inline PassbandSlide planPassbandSlide(double tunedHz, double centreHz, double rateHz,
                                       double bandwidthHz, double stepHz, double notches,
                                       double minTunedHz) {
    PassbandSlide r;
    if (!std::isfinite(tunedHz) || !std::isfinite(centreHz) || !std::isfinite(rateHz) ||
        !(rateHz > 0.0) || !std::isfinite(bandwidthHz)) {
        return r;
    }
    const double want =
        std::max(minTunedHz, passbandRasterStep(tunedHz, stepHz, notches));
    if (!std::isfinite(want) || want == tunedHz) { return r; }
    r.moved = true;
    r.wantAbsHz = want;
    const double reach = 0.5 * rateHz - 0.5 * bandwidthHz;
    const double off = want - centreHz;
    if (reach > 0.0 && off >= -reach && off <= reach) {
        r.offsetHz = off;
        return r;
    }
    // Past the end. The filter at the very end of its reach; a filter wider than the span has
    // none and sits in the middle. Either way the source takes up the rest.
    r.retuneSource = true;
    r.offsetHz = (reach > 0.0) ? (off < 0.0 ? -reach : reach) : 0.0;
    r.centreHz = want - r.offsetHz;
    return r;
}

}  // namespace cascade::gui
