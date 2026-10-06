// airband_marker_geometry.hpp - where an Airband channel's mark lies on the
// spectrum, pulled out of AppWindow::drawAirbandMarkers so the one rule in it
// can be checked without an ImGui frame or a spectrum to draw on.
//
// WHY THE RULE EXISTS (0.99.66). The monitor plays channels 10 kHz wide, and
// the receiver's span is megahertz: a channel's true width is a fraction of a
// pixel, and a mark that honest would not show at all. So a mark is never
// narrower than kAirbandMarkMinPx, centred on the channel - wider only where
// the span is zoomed in far enough for the channel to be wider than that.
//
// Same split as gui/bookmark_marker_geometry.hpp: WHAT the geometry is, here,
// with no ImGui in it; HOW it is painted stays in AppWindow.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_GUI_AIRBAND_MARKER_GEOMETRY_HPP
#define CASCADE_GUI_AIRBAND_MARKER_GEOMETRY_HPP

#include <algorithm>

#include "gui/freq_scale.hpp"

namespace cascade::gui {

// The narrowest a channel's mark is drawn, in pixels.
inline constexpr float kAirbandMarkMinPx = 3.0f;

struct AirbandMarkExtent {
    float xLo = 0.0f;       // left edge, in the same pixels as panelX0
    float xHi = 0.0f;       // right edge
    float xCentre = 0.0f;   // the channel's own frequency
};

// The extent of the mark for a channel at freqHz, bandwidthHz wide, on a panel
// that starts at panelX0 and is panelWidth wide and shows `scale`'s view.
// False - nothing to draw - when the channel's CENTRE is outside the view (a
// channel half in view is a mark for a channel the picture does not show) or
// the panel or the scale is unusable.
inline bool airbandMarkExtent(const FreqScale& scale, double freqHz, double bandwidthHz, float panelX0,
                              float panelWidth, AirbandMarkExtent& out) {
    const double lowHz = scale.viewLowHz();
    const double highHz = scale.viewHighHz();
    if (!(panelWidth > 0.0f) || !(highHz > lowHz)) { return false; }
    if (!(freqHz >= lowHz && freqHz <= highHz)) { return false; }   // also false for NaN
    const double half = bandwidthHz > 0.0 ? 0.5 * bandwidthHz : 0.0;
    const float xc = panelX0 + static_cast<float>(scale.hzToX(freqHz)) * panelWidth;
    float lo = panelX0 + static_cast<float>(scale.hzToX(freqHz - half)) * panelWidth;
    float hi = panelX0 + static_cast<float>(scale.hzToX(freqHz + half)) * panelWidth;
    lo = std::min(lo, xc - 0.5f * kAirbandMarkMinPx);
    hi = std::max(hi, xc + 0.5f * kAirbandMarkMinPx);
    out.xLo = lo;
    out.xHi = hi;
    out.xCentre = xc;
    return true;
}

}  // namespace cascade::gui

#endif  // CASCADE_GUI_AIRBAND_MARKER_GEOMETRY_HPP
