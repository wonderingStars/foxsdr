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
// ...UNLESS A PAGE PINS IT. That reasoning is right for an aircraft page and
// backwards for a satellite one, so a page can now say which it is - see
// MapProjection below. The satellite window pins equirectangular and forgoes
// tiles; every other page keeps the rule above, unchanged.
//
// AND ONE PAGE IS A WHOLE INSTRUMENT. drawSatellitePanel draws the SATELLITES
// MAP window's entire content - the control deck, the target register, the
// selected target's card and the map - so that everything for satellites is in
// one window rather than scattered across the main window's rail.
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

// --- which projection a page draws in ----------------------------------------
//
// THE SMALLEST HONEST WAY TO LET A PAGE CHOOSE, and it is small because the
// choice already existed - it was simply not addressable. This view has always
// drawn equirectangular UNLESS a basemap plugin was supplying tiles, in which
// case it switched to Web Mercator because the tile grid IS Mercator. So there
// is no second projection to write and no coordinate maths to convert: the
// only thing missing was a way for a page to say "I am the one that must not
// tear a polar orbit apart", and to accept the consequence.
//
// AND THE CONSEQUENCE IS REAL, so it is stated here rather than buried:
// Equirectangular means NO BASEMAP TILES on that page. Drawing a Mercator
// raster under an equirectangular graticule would put every coastline in the
// wrong place, which is worse than having no imagery; the built-in Natural
// Earth outline is drawn instead. That trade is right for satellites - street
// imagery is least useful exactly when a whole hemisphere is on screen - and
// wrong for aircraft, which is why Automatic stays the default and the ADS-B
// page is untouched.
enum class MapProjection {
    // Web Mercator while a basemap plugin supplies tiles, equirectangular
    // otherwise. What every page did before this existed.
    Automatic = 0,
    // Always equirectangular; basemap tiles are not asked for and not drawn.
    Equirectangular,
};

// --- the satellite window's controls -----------------------------------------
//
// THE SETTINGS THE SATELLITES MAP WINDOW OPERATES, owned by the CALLER because
// every one of them is application state that outlives this view: the trail
// switches and the coverage overlay are already AppConfig fields shared by
// every map page, and two pages disagreeing about how a trail is drawn would
// be two answers to one question. The panel edits them in place and never
// stores a second copy.
//
// The receiver's position is NOT here: it lives where it always did, applied
// through MapView::setHome by whoever owns it, because it is a fact about the
// antenna rather than a setting of this window. What the panel does instead is
// ASK for a new one - see receiverPositionRequested() - so the one caller that
// knows about every page, the radar scope and the coverage accumulator is the
// one that applies it.
struct SatelliteDeck {
    // Draw the coverage overlay. Inert without a receiver position, and the
    // panel says so rather than offering a switch that does nothing.
    bool coverage = false;
    // Draw the path layer at all - ground tracks included. The OFF segment of
    // the trail-style selector and the GROUND TRACKS rocker are two views of
    // this one flag, exactly as the design's mock has them.
    bool groundTracks = true;
    // Colour trails and markers by altitude band. False gives each its owner's
    // single colour.
    bool altitudeColours = true;
    // 0 = line, 1 = ribbon. Matches AppConfig::mapTrailStyle, which is what
    // the existing toolbar's style list already sets.
    int trailStyle = 0;
    // Which of the four visible sort keys the register is ordered by, and
    // which way. An INDEX rather than a TrackSortKey so the caller can persist
    // it as a number without knowing the enum's values; see satelliteSortKey.
    int sortKey = 0;
    bool sortAscending = true;
    // The two coordinate cells while they are being TYPED. Separate from the
    // applied position for the same reason the toolbar's fields always were:
    // an intermediate keystroke must not move the range rings. Committed by
    // Enter, and only then does the panel raise a request.
    double latInput = 0.0;
    double lonInput = 0.0;
};

// How many sort keys the register shows, and which they are. FOUR, VISIBLE, as
// buttons - the design's point being that the current sort is legible without
// opening anything. They are a SUBSET of TrackSortKey rather than a new set,
// so the register orders its rows with the same tested comparator the flight
// list uses (sortTrackRows) instead of a second one that could disagree.
//
// The four are the ones a satellite register can actually answer: a callsign,
// a NORAD number, an orbital altitude and the age of the fix. Distance and
// bearing are deliberately absent - they are empty until a receiver position
// is set, and a sort key that silently orders by nothing is the failure this
// window exists to remove.
inline constexpr int kSatelliteSortKeyCount = 4;

// The key at position `index`, and the engraved word over its button. An index
// outside the range answers with the first key rather than with whatever the
// last case happened to be.
TrackSortKey satelliteSortKey(int index);
const char* satelliteSortKeyLabel(int index);

// --- the three figures the no-position page is built out of -------------------
//
// All three are PURE and declared here so they can be pinned by a test. Every
// one of them is read on the frame a brand-new install draws its first
// satellite window, which is the state nothing else in this file can be tested
// in: there is no receiver position, nothing is selected, and the page has to
// be right anyway.

// THE WHOLE PLANET, in degrees of longitude across a viewport of this shape,
// and the view a page that pins equirectangular opens at. 360 across is the
// obvious half of it; the other half is that a viewport WIDER than two to one
// would then be showing less than 180 degrees of latitude and cropping the
// poles off - which on a satellite page is cropping off the part a polar orbit
// spends its time in. Whichever of the two demands more zoom wins.
double wholeWorldSpanDeg(float widthPx, float heightPx);

// WHAT ONE APERTURE OF THE RECEIVER'S COORDINATE COUNTER SHOWS.
//
// `shape` is the character the formatter produced for that aperture and
// `known` says whether there is a position behind it. With a position the
// character is shown as it stands. WITHOUT ONE THE FIGURES ARE NEVER SHOWN,
// and that is the whole point of this function: the formatter is handed 0.0
// so that the counter keeps its shape, and a row of apertures reading
// 00.00000 / 000.00000 is not a blank counter - it is a receiver claiming to
// stand at 0N 0E, which is a real place in the Gulf of Guinea and the exact
// conflation this application has been bitten by before. A figure becomes a
// dash; the SIGN is blanked rather than dashed, because a '-' in that cell
// reads as a southern latitude; the decimal point stays, so the counter still
// reads as a coordinate waiting for one.
//
// The SHAPE is what decides the widths (see the cells' layout), so it is the
// unchanged text that is measured and only the glyph that is replaced - the
// well therefore does not resize the moment a position arrives.
char coordApertureGlyph(char shape, bool known);

// HOW WIDE AND HOW TALL ONE OF THOSE APERTURES IS, measured in the counter
// face at the size it is lettered at rather than typed as a literal.
//
// WHY THIS IS NOT A CONSTANT ANY MORE. It was three of them - 13 px for a
// figure's cell, 8 for a sign or a decimal point, 26 for the height - fitted
// by eye around a 15 px counter. At 17 the counter face advances 6.85 px, so
// the narrow cells were down to about half a pixel of metal either side of
// their glyph while the figure cells still had two: the same control, drawn
// with and without a shoulder, because the two widths were typed rather than
// derived.
//
// AND THE NEXT POINT WOULD NOT HAVE CLIPPED, IT WOULD HAVE SPILLED.
// drawFreqDrumCell CENTRES its glyph in whatever cell it is handed and clips
// nothing, so a cell narrower than its figure lets that figure cross the
// machined edge into the cell beside it - which reads as a smeared counter
// rather than as a cut one, and gets taken for a rendering fault.
//
// `shape` is the character the formatter produced for that aperture - the
// SHAPE, not the glyph finally drawn, which is what keeps the well from
// resizing the moment a receiver position arrives (see coordApertureGlyph).
// A sign and a decimal point are narrower than a figure, because they are.
//
// Both must be called inside a frame: they ask the atlas to measure.
float coordCellWidth(char shape);
float coordCellHeight();

// WHICH NOTES THE SELECTED TARGET'S CARD DRAWS, from the only two facts that
// decide it. Both the height the card is RESERVED and the card's own drawing
// read this one answer, so the box can never be reserved for a note that is
// not drawn - which is what it was doing on the very first screen a new user
// sees: nothing selected and no position set reserved two notes' worth of
// space for a card whose whole content is the words NO TARGET SELECTED.
struct SatelliteCardNotes {
    // "DISTANCE and BEARING are hatched because no receiver position is set."
    // Recoverable: setting a position fills both in.
    bool noReceiver = false;
    // "INCLINATION, ORBITAL PERIOD and the age of the element set are not part
    // of what a track source reports." Not recoverable by anything the user
    // can click, which is why the two are drawn differently.
    bool notReported = false;
};
SatelliteCardNotes satelliteCardNotes(bool haveTarget, bool haveReceiverPosition);

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

    // See MapProjection. Automatic is what every page did before this existed,
    // and is still the default.
    //
    // AND A PAGE THAT PINS EQUIRECTANGULAR OPENS ON THE WHOLE WORLD. That is
    // not a side effect, it is the answer to what the pin MEANS: a page asks
    // for equirectangular because the poles matter to it and a track may be
    // anywhere on the planet, and the first thing such a page must show is the
    // planet. Left to the ordinary fit-to-content rule it did the opposite -
    // one satellite has no extent, so the fit fell back to a 24-degree span
    // and the satellites window opened on a patch of Indonesia with a single
    // marker in it and no way to tell where on earth that was.
    //
    // ONLY WHEN NOTHING HAS CHOSEN A VIEW YET (see fittedOnce_), so this can
    // never yank the map out from under a user who has already panned, zoomed
    // or gone to a target; and only on the CHANGE, so a page that sets its
    // projection every frame - which the satellite panel does - asks for the
    // whole world once, on the frame it opens.
    void setProjection(MapProjection p) {
        if (p != projection_ && p == MapProjection::Equirectangular && !fittedOnce_) {
            wholeWorldRequested_ = true;
        }
        projection_ = p;
    }
    MapProjection projection() const { return projection_; }

    // Centres on 0,0 and zooms out until the whole planet is inside the
    // viewport. Public because "show me everything" is a thing a caller may
    // legitimately want at any time, and because it is what makes the rule
    // above testable without going through drawSatellitePanel.
    void requestWholeWorld() { wholeWorldRequested_ = true; }

    // --- the satellite instrument -------------------------------------------
    //
    // THE WHOLE CONTENT OF THE SATELLITES MAP WINDOW, drawn into the CURRENT
    // ImGui window: the control deck across the top, the target register down
    // the left with the selected target's card beneath it, and the map with
    // its overlays filling the rest. The window itself - its frame, its place
    // in the Windows menu and the rail switch that opens it - belongs to the
    // caller; this draws what is inside it.
    //
    // ONE WINDOW, AND THAT IS THE POINT. Everything for satellites is in here:
    // receiver position, overlays, trail style, coverage, the register, the
    // selected target's figures and the map. Nothing satellite-shaped is left
    // scattered in the main window's rail.
    //
    // IT FORCES ITS OWN PROJECTION. A satellite window is equirectangular (see
    // MapProjection) - the poles are on screen and a ground track reads as the
    // sinusoid it is - which is why `tiles` is not a parameter: this page does
    // not draw basemap tiles, and offering a pointer it would ignore would be
    // a lie in the signature.
    //
    // `coverage` may be null and is borrowed for the call. It is passed even
    // when the overlay is switched off, because the deck's own note reports
    // what has been accumulated whether or not it is being drawn - "nothing
    // heard yet" and "hidden" are different facts.
    void drawSatellitePanel(SatelliteDeck& deck,
                            const std::vector<cascade::core::HostTrack>& tracks,
                            const std::vector<cascade::core::HostPath>& paths,
                            const CoverageMap* coverage, TrackInfoCache* info = nullptr);

    // SET FROM MAP CLICK, as a latch rather than as a mode the caller has to
    // maintain. While armed, the next click on the map that is not a drag
    // reads its position and raises the request below; a second press of the
    // button, or the click itself, disarms it.
    bool receiverPickArmed() const { return pickHomeArmed_; }
    void armReceiverPick() { pickHomeArmed_ = true; }
    void cancelReceiverPick() { pickHomeArmed_ = false; }

    // THE PANEL ASKS, THE CALLER APPLIES. A receiver position reaches every map
    // page, the radar scope and the coverage accumulator - which is knowledge
    // this view does not have and must not acquire - so the panel raises a
    // request and the owner acts on it, exactly as followInterruptRequested()
    // already does for the follow prompt. Not cleared by draw(): the caller
    // clears it after acting, so a request cannot be missed by a frame that
    // happened to skip.
    bool receiverPositionRequested() const { return homeRequest_; }
    double requestedReceiverLatDeg() const { return homeRequestLat_; }
    double requestedReceiverLonDeg() const { return homeRequestLon_; }
    void clearReceiverPositionRequest() { homeRequest_ = false; }

    // Likewise for RESET COVERAGE: the accumulator belongs to the caller.
    bool coverageResetRequested() const { return coverageResetRequest_; }
    void clearCoverageResetRequest() { coverageResetRequest_ = false; }

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
    // ONE FLAG PER LADDER, because there are two of them and they are read in
    // different units. A page whose trails were banded on the ORBITAL ladder
    // must not put an aviation legend on screen: "> 30 kft" over a picture
    // banded in kilometres is a legend that actively misinforms.
    bool anyBandedTrail_ = false;
    bool anyOrbitTrail_ = false;
    bool fitRequested_ = true;  // fit once, the first time there is anything
    bool fittedOnce_ = false;
    // Applied by the next draw(), which is the first place the viewport's
    // shape is known - "the whole world" is 360 degrees across AND 180 down,
    // and which of those two decides the zoom depends on the aspect ratio.
    bool wholeWorldRequested_ = false;

    MapProjection projection_ = MapProjection::Automatic;

    // SET FROM MAP CLICK, and what it produced. See armReceiverPick and
    // receiverPositionRequested.
    bool pickHomeArmed_ = false;
    bool homeRequest_ = false;
    double homeRequestLat_ = 0.0;
    double homeRequestLon_ = 0.0;
    bool coverageResetRequest_ = false;
    // Which coordinate cell the user is typing into, or -1 for none: 0 is the
    // latitude, 1 the longitude. A cell is a MACHINED APERTURE, not a text
    // box, so it becomes an editable field only while it is being edited -
    // and only one at a time, because two open fields would each need their
    // own commit and the pair is applied together.
    int coordEditing_ = -1;
    // True for the one frame after a cell is opened, which is when the
    // keyboard has to be put into it. Asked as its own flag rather than
    // inferred from "nothing else is active" because that inference is about
    // the PREVIOUS frame's arbitration and would silently stop working the
    // day another control on this deck took focus first.
    bool coordEditFocus_ = false;

    std::string hoveredId_;
    std::string followId_;
    std::string selectedId_;
    bool followInterrupt_ = false;
};

}  // namespace cascade::gui
