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
#include <functional>
#include <string>
#include <vector>

#include "core/plugin_ui.hpp"
#include "gui/track_metrics.hpp"

namespace cascade::gui {

class BasemapCache;
class TrackInfoCache;

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
    //
    // `info` may be null too; when a track-info plugin is active it feeds the
    // hover tooltip's registration/type/operator lines.
    void draw(float width, float height, const std::vector<cascade::core::HostTrack>& tracks,
              const std::vector<cascade::core::HostPath>& paths,
              BasemapCache* tiles = nullptr, TrackInfoCache* info = nullptr);

    // The receiver's own position, used for range rings and for the range and
    // bearing readout. Unset until the user provides one - guessing it from a
    // decoded track would be wrong the moment a plugin reports an aircraft.
    void setHome(double latDeg, double lonDeg);
    void clearHome() { hasHome_ = false; }
    // THE COVERAGE OVERLAY, borrowed for the next draw() and null when the user
    // has it switched off. A pointer rather than a copy because the accumulator
    // is fed every frame by the window that owns it, and rather than a second
    // boolean parameter because "no coverage to draw" and "coverage hidden" are
    // the same instruction to this class. Drawn only when a home position is
    // set: every wedge in it is a distance FROM somewhere, and without that
    // somewhere there is nothing to centre it on.
    void setCoverage(const CoverageMap* coverage) { coverage_ = coverage; }

    // HOW A TRAIL VERTEX GETS AN ALTITUDE. The arguments are the owning
    // plugin, the track id, and the vertex's position; the answer is true with
    // the altitude in metres filled in, or false for "nothing was observed
    // here" - which is a real answer and must be drawn as unknown rather than
    // guessed at (see cascade::core::PluginUi::altitudeNear, which is what the
    // application installs here).
    //
    // A FUNCTION AND NOT A POINTER TO THE HOST, so this view keeps knowing
    // nothing about who is answering: the map draws what it is told and does
    // not acquire an opinion about the plugin system to do it. Unset by
    // default, which is exactly the state a test or a caller with no store
    // needs - every trail then falls back to its owner's single colour.
    using AltitudeAt = std::function<bool(const std::string& plugin, const std::string& id,
                                          double latDeg, double lonDeg, double& outAltM)>;
    void setAltitudeLookup(AltitudeAt fn) { altitudeAt_ = std::move(fn); }

    // THE TWO TRAIL SWITCHES, and they are two because the request behind them
    // was ambiguous and both readings deserve an answer: some users do not
    // want the colouring, and some do not want trails at all. `drawTrails`
    // false draws none of the path layer; `altitudeColours` false keeps the
    // trails and gives each its owner's single colour, exactly as they were
    // drawn before this existed. Persisted by the caller (AppConfig::mapTrails
    // and AppConfig::mapTrailAltitudeColours); defaulted ON here so a view
    // nobody configures behaves the way the application does.
    // 0 = line, 1 = ribbon. See AppConfig::mapTrailStyle.
    void setTrailStyle(int style) { trailStyle_ = style; }

    void setTrailOptions(bool drawTrails, bool altitudeColours) {
        drawTrails_ = drawTrails;
        trailAltitudeColours_ = altitudeColours;
    }

    bool hasHome() const { return hasHome_; }
    double homeLatDeg() const { return homeLat_; }
    double homeLonDeg() const { return homeLon_; }

    // Centres the view on everything currently plotted. A map that opens on
    // the Atlantic while every target is over England is a map the user has to
    // fight before it is useful.
    void requestFitToTracks() { fitRequested_ = true; }

    // GO TO ONE TARGET: centre on it and, if the view is wider than `spanDeg`,
    // zoom in to that. Used by the flight list beside the map - clicking a
    // callsign should take you to the aircraft, which is the one thing a list
    // of callsigns is for.
    //
    // The zoom is only ever TIGHTENED, never loosened: someone who has zoomed
    // right into an approach path and then clicks a flight in that same area
    // wants to go to it, not to be yanked back out to a county view.
    void goTo(double latDeg, double lonDeg, double spanDeg = 2.0);

    // The track the list has asked to follow, empty when none. While set, the
    // view re-centres on that target every frame, so an aircraft being watched
    // stays put instead of flying off the edge.
    void setFollowed(const std::string& id) { followId_ = id; }
    const std::string& followedId() const { return followId_; }
    void clearFollow() { followId_.clear(); }

    // The track the user last clicked in the list, so the caller can show it
    // as selected. Distinct from following: selecting is a highlight, and
    // following moves the map.
    void setSelected(const std::string& id) { selectedId_ = id; }
    const std::string& selectedId() const { return selectedId_; }

    // Latched when the user tried to DRAG the map while a target was being
    // followed. Follow re-centres every frame, so applying the drag just made
    // the view fight the hand and snap back; the drag is now IGNORED while
    // following and the attempt recorded here instead, so the caller can ask
    // "stop following?" once per gesture rather than letting the map stutter.
    // The caller clears it after acting (it is not cleared by draw()).
    bool followInterruptRequested() const { return followInterrupt_; }
    void clearFollowInterruptRequest() { followInterrupt_ = false; }

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
    const CoverageMap* coverage_ = nullptr;  // borrowed, may be null

    AltitudeAt altitudeAt_;  // empty until the host installs one
    bool drawTrails_ = true;
    bool trailAltitudeColours_ = true;
    // One trail's per-vertex altitudes, NaN where nothing was observed. A
    // MEMBER because the path layer runs every frame for every path, and the
    // coverage polygon's fixed-size array is not an option here - a path's
    // length is the plugin's choice, up to PluginUi::kMaxPathPoints. Reused
    // rather than reallocated, so the steady state allocates nothing; assign()
    // keeps the capacity a previous frame grew.
    std::vector<double> trailAltScratch_;

    // Set by the path loop when it draws a banded trail, read by the target
    // loop when it decides whether the altitude legend is information or
    // clutter. A frame member rather than a local because the two loops are
    // far apart and the path one runs first.
    int trailStyle_ = 0;
    bool anyBandedTrail_ = false;
    bool fitRequested_ = true;  // fit once, the first time there is anything
    bool fittedOnce_ = false;

    std::string hoveredId_;
    std::string followId_;
    std::string selectedId_;
    bool followInterrupt_ = false;
};

}  // namespace cascade::gui
