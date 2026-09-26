// basemap_stand_in.hpp - a basemap that needs no plugin and no network, for
// bounded test runs only (0.99.41).
//
// WHY IT EXISTS. What a map does WITH tiles - letter the attribution the
// basemap supplies, keep the tile cache's frame count moving so eviction and
// the retry of a missing tile happen - can only be seen with a basemap
// attached, and the only real one is a plugin that fetches from a tile server.
// A test must not depend on either. FOXSDR_FORCE_BASEMAP=1 in a --frames run
// attaches this instead when no plugin supplies a basemap: flat tiles of one
// colour, served at once, with an attribution line of its own that a test can
// look for. Never in an interactive run, never beside a real basemap.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_GUI_BASEMAP_STAND_IN_HPP
#define CASCADE_GUI_BASEMAP_STAND_IN_HPP

#include "core/plugin_abi.h"

namespace cascade::gui {

// The attribution the stand-in supplies - what a test expects to see lettered.
inline constexpr const char* kStandInBasemapAttribution = "Stand-in tiles (FOXSDR_FORCE_BASEMAP)";

// The stand-in's table. Every READY tile it serves notes "basemap:tile" in the
// interface census (gui/ui_census.hpp), so a test can tell a map that asked
// for tiles from one that did not.
const CascadeBasemapApi* standInBasemap();

// Whether FOXSDR_FORCE_BASEMAP asks for it: set, non-empty, and a bounded run.
bool standInBasemapWanted(const char* value, bool boundedRun);

}  // namespace cascade::gui

#endif  // CASCADE_GUI_BASEMAP_STAND_IN_HPP
