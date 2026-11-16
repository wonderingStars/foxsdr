// mapicons_logic.hpp - which aircraft silhouette a track draws on the Slint maps (window-gaps lane 4, row 493).
//
// The classic's rule (gui/aircraft_icons.cpp aircraftIconForCategory / aircraftIconFor), repeated here because that file also draws with ImGui and is not linked into this window; the
// order of the indexes is gui/aircraft_icons.hpp's AircraftIcon and the pictures are gui/aircraft_icon_pixels.hpp's. The test pins every category of the ABI.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstdint>

namespace foxsdr::slinthost {

constexpr int kAircraftIconCount = 11;   // airliner, heavy, light, glider, microlight, uav, balloon, surface, helicopter, unknown, helicopterlight

// The silhouette of a broadcast category (CASCADE_AIRCRAFT_*); a value this build has never heard of draws "no type stated" (index 9). `icaoType` may be null: a single-engine helicopter
// type (Doc 8643 H1P / H1T) draws the light helicopter (index 10), every other rotorcraft the five-blade one (index 8).
int aircraftIconIndex(std::uint32_t category, const char* icaoType);

}  // namespace foxsdr::slinthost
