// airmarks_logic.hpp - where the Airband monitor's channels lie on the spectrum (window-gaps lane 4, rows 212, 264 to 267).
//
// The classic window's rule (gui/airband_marker_geometry.hpp): the monitor plays channels 10 kHz wide on a span of megahertz, so a mark is never
// narrower than 3 px, centred on the channel; a channel whose CENTRE is outside the view has no mark. Pure, so a test can hold it.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <string>
#include <vector>

namespace foxsdr::slinthost {

struct AirChannelMark {
    double hz = 0.0;
    double bwHz = 0.0;
    bool open = false;      // the squelch is open now
};

struct AirMarkFig {
    float from = 0.0f, to = 0.0f;   // 0..1 of the plot
    bool open = false;
};

constexpr float kAirMarkMinPx = 3.0f;

// The marks of the channels in view of [centreHz - spanHz/2, centreHz + spanHz/2] on a plot `plotPx` wide.
std::vector<AirMarkFig> airbandMarks(const std::vector<AirChannelMark>& channels, double centreHz, double spanHz, double plotPx);

}  // namespace foxsdr::slinthost
