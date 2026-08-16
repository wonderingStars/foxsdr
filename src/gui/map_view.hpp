// map_view.hpp - draws plugin-supplied tracks and paths on a world map.
//
// One map for everything. Aircraft, ships, APRS stations and satellites all
// arrive as CascadeTrack and are drawn here together, which is the point of
// having a single track type in the ABI: a user watching aircraft and ships at
// once should see one picture, not two windows.
//
// PROJECTION: equirectangular by default (longitude maps linearly to x,
// latitude to y). It is the wrong projection for navigation and the right one
// here - it is exactly invertible with two multiplies, which is what makes
// hit-testing the cursor and clamping the view cheap, and at the scales this is
// used for (a few hundred km around a receiver, or a whole orbit) its
// distortion is either irrelevant or unavoidable. Mercator would misplace
// polar orbits, which is precisely the case a satellite tracker cares about.
//
// ...AND WEB MERCATOR while a basemap plugin is supplying tiles, because tiles
// are Mercator by construction. Reprojecting them per frame would cost
// sharpness and time to preserve an advantage - not misplacing polar orbits -
// that matters exactly when a satellite is being tracked, which is when street
// imagery is least useful. Aircraft, ships and APRS stations all sit well
// inside the latitudes where Mercator behaves.
//
// BASEMAP. The built-in one is a Natural Earth coastline compiled in as vector
// data: public domain, offline, and free of the conditions raster imagery
// carries. Real map tiles arrive only through a plugin the user installs and
// points at their own server - which keeps the licence question with the person
// who chose to serve them. On top of either goes a graticule and, when the
// receiver's position is known, range rings, which for local aircraft and ships
// answer the actual question: how far away and which bearing.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "core/plugin_ui.hpp"

namespace cascade::gui {

class BasemapCache;

class MapView {
public:
    // Draws into the CURRENT ImGui window, filling `width` x `height`.
    // `tracks` and `paths` are borrowed for the call only.
    //
    // `tiles` may be null, and is whenever no basemap plugin is installed —
    // which is the shipped configuration. When it is supplied AND active, the
    // view switches to Web Mercator and draws its tiles beneath everything
    // else; the coastline then stays out of the way, because drawing vector
    // land over a rendered map is just a wrong-coloured outline.
    void draw(float width, float height, const std::vector<cascade::core::HostTrack>& tracks,
              const std::vector<cascade::core::HostPath>& paths,
              BasemapCache* tiles = nullptr);

    // The receiver's own position, used for range rings and for the range and
    // bearing readout. Unset until the user provides one - guessing it from a
    // decoded track would be wrong the moment a plugin reports an aircraft.
    void setHome(double latDeg, double lonDeg);
    void clearHome() { hasHome_ = false; }
    bool hasHome() const { return hasHome_; }
    double homeLatDeg() const { return homeLat_; }
    double homeLonDeg() const { return homeLon_; }

    // Centres the view on everything currently plotted. A map that opens on
    // the Atlantic while every target is over England is a map the user has to
    // fight before it is useful.
    void requestFitToTracks() { fitRequested_ = true; }

    // Degrees of longitude across the viewport; smaller is more zoomed in.
    double spanDeg() const { return spanDeg_; }

    // The track the cursor is over, empty when none. Lets the caller show a
    // detail readout without duplicating the hit test.
    const std::string& hoveredId() const { return hoveredId_; }

private:
    // View state, in degrees.
    double centreLat_ = 54.0;   // somewhere sane before anything is plotted
    double centreLon_ = -2.0;
    double spanDeg_ = 20.0;

    bool hasHome_ = false;
    double homeLat_ = 0.0;
    double homeLon_ = 0.0;

    bool fitRequested_ = true;  // fit once, the first time there is anything
    bool fittedOnce_ = false;

    std::string hoveredId_;
};

}  // namespace cascade::gui
