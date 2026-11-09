// view_follow.hpp - THE PICTURE FOLLOWS A RETUNE THAT LEAVES IT. The one rule both windows apply to their own view state.
//
// THE REPORT (a beta tester, an HF operator, 2026-11-09): "if I am on the 7 MHz band and switch to the 3.5 MHz band, the
// waterfall display remains on 7 MHz - I am forced to move the waterfall with the mouse; the transition should happen
// automatically." What happened: a zoomed picture is a window in absolute Hz, and a retune moves the BASEBAND (the source's
// centre follows the tuned frequency, the VFO offset kept). The ImGui window's FreqScale keeps a zoom that still fits the new
// baseband - the right thing for a small retune or a rate change - and the Slint window's ViewWindow is never touched by a
// retune at all; so with a wide baseband (an SDRplay at 10 MS/s holds both 7 and 3.5 MHz) the picture stayed on the old band
// while the receiver listened on the new one, off the picture's edge.
//
// THE RULE: when the TUNED FREQUENCY MOVES (a retune: typed, clicked, a bookmark, a key, the web remote, a plugin) and lands
// OUTSIDE the picture as it was drawn, the picture is recentred on the new frequency AT THE SAME WIDTH, held inside the
// baseband the source now delivers (brought as near an edge as the baseband allows, never past it). Nothing moves when the
// frequency did not change, when it lands inside the picture (the hand's zoom is left alone), or when the picture is the full
// span or wider (the baseband itself follows the tune, both windows show the whole baseband for such a picture, and this rule
// never invents a zoom). The windows add their own truces on top - not while the hand drags the band or pans the waterfall,
// not while the passband slide's own follow is in force (app_window.cpp).
//
// Pure arithmetic, no window types: tests/test_view_follow.cpp has every branch with the numbers written out. Both windows
// convert to and from their own representation (low/high for FreqScale, centre/span for ViewWindow) at the call.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_VIEW_FOLLOW_HPP
#define CASCADE_CORE_VIEW_FOLLOW_HPP

#include <algorithm>
#include <cmath>

namespace cascade::core {

struct ViewFollow {
    bool moved = false;      // whether the picture moves at all
    double lowHz = 0.0;      // the picture to draw, when it does
    double highHz = 0.0;
};

// `viewLowHz`/`viewHighHz`: the picture as it was last drawn. `fullLowHz`/`fullHighHz`: the baseband the source delivers
// NOW (after the retune). `wasTunedHz`/`nowTunedHz`: the tuned frequency before and after. Any non-finite input, an
// empty or inverted picture or baseband, or a first frame with no previous tune (NaN) moves nothing.
inline ViewFollow recentreOnRetune(double viewLowHz, double viewHighHz, double fullLowHz, double fullHighHz,
                                   double wasTunedHz, double nowTunedHz) {
    ViewFollow out;
    if (!std::isfinite(viewLowHz) || !std::isfinite(viewHighHz) || !std::isfinite(fullLowHz) ||
        !std::isfinite(fullHighHz) || !std::isfinite(wasTunedHz) || !std::isfinite(nowTunedHz)) {
        return out;
    }
    const double viewSpan = viewHighHz - viewLowHz;
    const double fullSpan = fullHighHz - fullLowHz;
    if (!(viewSpan > 0.0) || !(fullSpan > 0.0)) { return out; }
    if (nowTunedHz == wasTunedHz) { return out; }                           // no retune
    if (nowTunedHz >= viewLowHz && nowTunedHz <= viewHighHz) { return out; }  // landed inside the picture
    if (viewSpan >= fullSpan) { return out; }   // the full span (or wider: both windows show the full span for it) follows the tune by itself
    const double span = viewSpan;
    const double lo = fullLowHz + span / 2.0;
    const double hi = fullHighHz - span / 2.0;
    const double centre = std::clamp(nowTunedHz, std::min(lo, hi), std::max(lo, hi));
    out.moved = true;
    out.lowHz = centre - span / 2.0;
    out.highHz = centre + span / 2.0;
    return out;
}

}  // namespace cascade::core

#endif  // CASCADE_CORE_VIEW_FOLLOW_HPP
