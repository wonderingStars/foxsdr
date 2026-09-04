// track_metrics.hpp - the arithmetic behind the track table, the altitude
// colouring and the coverage map, with no ImGui and no application state in it.
//
// WHY THESE THREE LIVE TOGETHER AND WHY THEY LIVE HERE. All three are host
// work by necessity: CascadeHostApi hands a plugin the centre frequency, the
// sample rate, a tune request and the time, and NOTHING about where the
// receiver is - so nothing on the plugin side of the ABI can compute a range or
// a bearing at all. Putting them in the host also means they work for every
// track source at once: an aircraft, a ship and an APRS station all arrive as
// CascadeTrack, and a range column that only understood ADS-B would be a
// column that is blank for two of the three.
//
// AND WHY THEY ARE PURE. Everything here is a function of its arguments, so
// each of the three - the geodesy, the band that decides a colour, and the
// per-bearing accumulator - can be tested against known pairs and known
// sequences without a window, a GL context or a plugin. The drawing code that
// uses them is the part a test cannot reach; keeping the arithmetic out of it
// is what leaves anything testable at all.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

namespace cascade::gui {

// --- geodesy -----------------------------------------------------------------

// Great-circle distance in kilometres on a sphere of radius 6371 km, by the
// haversine formula. Spherical rather than ellipsoidal on purpose: the error
// against WGS-84 is a few parts in a thousand, which is far below what a
// receiver's own position is known to when it was typed into a text box, and
// the ellipsoidal form (Vincenty) fails to converge for near-antipodal pairs -
// a failure mode a range column must never have.
double greatCircleKm(double lat1Deg, double lon1Deg, double lat2Deg, double lon2Deg);

// Initial bearing in degrees true, 0..360, from point 1 towards point 2. It is
// the bearing to POINT AT, which is what an antenna wants; it is not constant
// along the path, and over a few hundred kilometres the difference does not
// reach a degree.
//
// IDENTICAL POINTS RETURN NaN - explicitly undefined rather than the zero that
// atan2(0, 0) would hand back. There is no direction from a point to itself,
// and "000 deg" is a fabricated fact that a user would read as north.
double initialBearingDeg(double lat1Deg, double lon1Deg, double lat2Deg, double lon2Deg);

// A place on the sphere.
struct LatLon {
    double latDeg;
    double lonDeg;  // normalised to -180..180
};

// THE DIRECT PROBLEM, and the exact inverse of the two functions above: set off
// from (latDeg, lonDeg) on `bearingDeg` true, travel `km` along a great circle
// on the SAME 6371 km sphere greatCircleKm measures on, and this is where you
// end up. greatCircleKm(start, destinationPoint(start, b, d)) == d and
// initialBearingDeg(start, destinationPoint(start, b, d)) == b, to floating
// point, which is the property the tests lean on hardest.
//
// IT EXISTS BECAUSE THE PLATE-CARREE SHORTCUT IS NOT GOOD ENOUGH. Offsetting a
// latitude by km/111.32*cos(bearing) and a longitude by
// km/(111.32*cos(lat))*sin(bearing) is a flat-earth step: it treats the meridian
// convergence as constant over the whole leg and ignores that a great circle
// changes bearing as it goes. From 51.5N the error against this function is
// 1.1 km at 100 km, 9.9 km at 300 km, 27.4 km at 500 km and 110 km at 1000 km -
// and 300-500 km is ordinary reach for an ADS-B or APRS receiver, so the
// shortcut is wrong exactly where the picture is being looked at.
//
// The longitude comes back NORMALISED to -180..180, because it is a place and
// that is what a place's longitude is. Code that needs a CONTINUOUS offset -
// anything drawing a connected shape - wants lonDeltaDeg below instead.
LatLon destinationPoint(double latDeg, double lonDeg, double bearingDeg, double km);

// The signed longitude difference from `fromLonDeg` to `toLonDeg`, taking the
// SHORT way round: always in -180..180, so +179.5 to -179.5 is +1, not -359.
//
// This is the piece that keeps a shape drawn around a receiver near the
// antimeridian in one piece. A vertex one degree east of 179.5 is at -179.5,
// and a plotter that subtracts raw longitudes puts it most of a world away -
// which is a line straight across the map, the classic naive ground-track bug.
double lonDeltaDeg(double fromLonDeg, double toLonDeg);

// One vertex of the coverage outline, expressed the only way a connected shape
// can be: a latitude, and a longitude offset from the receiver that is
// CONTINUOUS rather than wrapped into a range. The drawing code turns the
// offset into pixels by scaling it - both projections this view offers are
// linear in longitude across x - so the outline never has a seam in it no
// matter where the receiver sits or where the view happens to be centred.
struct CoverageVertex {
    double latDeg;
    double dLonDeg;  // -180..180, measured from the receiver
};
CoverageVertex coverageVertex(double homeLatDeg, double homeLonDeg, double bearingDeg,
                              double km);

// --- altitude bands ----------------------------------------------------------

// The bands, in feet, because every altitude a track source reports is derived
// from an aviation or marine source that thinks in feet and because the table
// prints feet. Metres in, feet inside: the ABI says altM is metres.
//
// SIX BANDS, and the boundaries are the ones that separate the phases of flight
// a watcher actually distinguishes: circuit and final approach below 1 kft,
// departure and arrival below 5, low-level and turboprop cruise below 10,
// climb and descent below 20 and 30, and jet cruise above. An approach and a
// cruise land four bands apart, which is the whole point.
inline constexpr int kAltBandCount = 6;

struct AltBandStyle {
    std::uint8_t r, g, b;
    const char* label;
};

// -1 when the altitude is unknown - NaN by ABI contract, and also whatever
// infinity a broken plugin might send. Every other value falls in a band,
// including a negative one: below sea level is a real place and a Dead Sea
// approach is not a decode error.
int altitudeBandIndex(double altM);

// Bounds-safe: an index outside 0..kAltBandCount-1 is clamped rather than read
// past the end, so a caller that passes -1 by accident gets a colour instead of
// undefined behaviour.
const AltBandStyle& altBandStyle(int index);

// --- orbital altitude bands ---------------------------------------------------

// THE SAME IDEA IN THE OTHER UNIT, and it exists because the ladder above is an
// AVIATION ladder wearing a general name. Its top band opens at 30 kft and the
// ISS at 421 km is 1,381,000 ft: EVERY satellite this application has ever
// tracked falls in that one band, draws in the same indigo, and sits under a
// legend reading "> 30 kft" - a unit nothing in orbit is anywhere near. A 421 km
// orbit and an 861 km orbit have therefore never been told apart on the map.
//
// THE AVIATION LADDER IS NOT CHANGED, and must not be: it is correct for its
// own subject, its boundaries are the phases of flight a watcher distinguishes,
// and the tests pin every one of them. Two subjects, two ladders; the call site
// picks one by track kind (CASCADE_TRACK_SATELLITE against the rest).
//
// FIVE BANDS, IN KILOMETRES, and the boundaries are where the populations
// actually sit rather than where round numbers fall: the station and the very
// low orbits under 500, the amateur and rideshare orbits from 500 to 750, the
// sun-synchronous weather belt from 750 to 850 that NOAA 15 and the three
// Meteor-M2s share, 850 to 1200 for the higher sun-synchronous orbits NOAA 18
// and 19 fly, and everything above that. Against the eight targets a stock
// installation follows - ISS 421, SO-50 692, NOAA 15 807, Meteor-M2 4 814,
// Meteor-M2 2 818, Meteor-M2 3 822, NOAA 18 854, NOAA 19 861 - that puts real
// traffic in four of the five bands, where the aviation ladder put all eight in
// one.
inline constexpr int kOrbitBandCount = 5;

// -1 when the altitude is unknown, exactly as altitudeBandIndex does: NaN by ABI
// contract, and also whatever infinity a broken plugin might send. Metres in,
// kilometres inside - the ABI says altM is metres for every track kind, orbital
// ones included.
int orbitBandIndex(double altM);

// Bounds-safe on the same terms as altBandStyle: an index outside
// 0..kOrbitBandCount-1 is clamped rather than read past the end, so the -1 that
// orbitBandIndex itself returns for an unknown altitude yields a colour rather
// than undefined behaviour.
const AltBandStyle& orbitBandStyle(int index);

// --- altitude legend layout --------------------------------------------------

// The legend's fixed parts: a square swatch, and the same padding either side
// of it and after the label. Named rather than repeated as literals because the
// width below and the drawing code have to agree about them, and when they were
// written separately they did not.
inline constexpr float kAltLegendSwatch = 10.0f;
inline constexpr float kAltLegendPad = 5.0f;

// Panel width for the altitude legend, MEASURED rather than guessed.
//
// A hard-coded width is a promise about a font, and the font is the host's
// choice at runtime: the shipped constant was 74 px, which held "< 1 kft" and
// clipped "20-30 kft" and "10-20 kft" to "20-30 kf" and "10-20 kf" and took the
// final t off "> 30 kft" - a legend that is itself unreadable, which is worse
// than no legend because the user cannot tell it is truncated. The measuring
// function is passed in so this stays free of ImGui and testable; the caller
// hands it ImGui::CalcTextSize with the font actually in use.
template <typename MeasureTextWidth>
float altLegendWidth(MeasureTextWidth&& measureTextWidth);

// The same measurement for the ORBITAL ladder, and it is not the same number:
// "850-1200 km" is eleven characters where the widest aviation label,
// "20-30 kft", is nine. A satellite legend laid out at altLegendWidth would clip
// its own longest label - which is the shipped defect described directly above,
// re-committed in a second place - so the two ladders each measure their own.
template <typename MeasureTextWidth>
float orbitLegendWidth(MeasureTextWidth&& measureTextWidth);

// --- coverage accumulation ---------------------------------------------------

// The greatest distance anything has been heard at, per compass bearing. It is
// the cheapest antenna-tuning instrument there is: a lobe pattern measured from
// the traffic that was going to be decoded anyway, with no external service and
// no reference transmitter.
//
// BOUNDED BY CONSTRUCTION. Seventy-two five-degree buckets, one double each -
// 576 bytes for the whole session, whatever the traffic. Five degrees is
// roughly the width of a real pattern's null; finer buckets would mostly record
// how many aircraft happened to fly down each one.
//
// NOT PERSISTED, deliberately. The accumulation measures ONE antenna in ONE
// place: carrying it across restarts would blend last month's dipole with
// today's collinear and there would be no way to tell what a change did. A
// session that starts empty is the honest baseline, and reset() is there for a
// bad hour inside a good session.
class CoverageMap {
public:
    static constexpr int kBuckets = 72;
    static constexpr double kBucketDeg = 360.0 / static_cast<double>(kBuckets);

    // Records one sighting. Silently ignores anything that is not a measurement:
    // a NaN bearing (the target is on top of the receiver, see
    // initialBearingDeg), a NaN or non-positive distance, and anything past
    // half the earth's circumference, which no terrestrial receiver has heard
    // and which a broken plugin can produce.
    void record(double bearingDeg, double km);

    // Bounds-safe, and 0 means "nothing heard this way" rather than "heard at
    // zero range" - the accumulator only ever takes positive distances, so the
    // two cannot be confused.
    double maxKm(int bucket) const;

    void reset();

    bool empty() const { return filled_ == 0; }
    int filledBuckets() const { return filled_; }
    double peakKm() const;

private:
    double maxKm_[kBuckets] = {};
    int filled_ = 0;
};

// --- the sortable track table ------------------------------------------------

// One row, already reduced to numbers. The table sorts THIS rather than the
// host's track vector, because that vector is the plugins' output and is
// rebuilt every poll: sorting it in place would fight the next frame.
enum class TrackSortKey {
    Label = 0,
    Id,
    Altitude,
    Speed,
    Course,
    Distance,
    Bearing,
    Age,
};

struct TrackRow {
    std::string label;
    std::string id;
    // NaN for every one of these means the source does not know, exactly as the
    // ABI says. distanceKm and bearingDeg are additionally NaN when the
    // receiver's own position has never been set, which is a different fact and
    // is what the table prints differently.
    double altM = std::numeric_limits<double>::quiet_NaN();
    double speedMps = std::numeric_limits<double>::quiet_NaN();
    double courseDeg = std::numeric_limits<double>::quiet_NaN();
    double distanceKm = std::numeric_limits<double>::quiet_NaN();
    double bearingDeg = std::numeric_limits<double>::quiet_NaN();
    std::uint64_t ageMs = 0;
    // Index back into the host's track vector, so a clicked row can still do
    // what clicking a row has always done - select the target and fly the map
    // to it - after the rows have been reordered under it.
    std::size_t source = 0;
};

// How many keys there are. Pinned so the sort menu cannot list a subset of
// them by accident - dropping a key from that menu is dropping the ability to
// ask its question, which is exactly what happened when the table lost six of
// its eight columns.
inline constexpr int kTrackSortKeyCount = 8;

// The key at position `index` of the sort menu. ONE mapping, so the menu's
// order and the window's remembered sort state cannot drift apart; an index
// outside the menu answers with the default rather than with whatever the last
// case happened to be.
//
// IT USED TO BE trackSortKeyForColumn, one key per table column, back when the
// list was an eight-column sortable table. Eight columns do not fit the width
// the list actually gets and their headings were unreadable, so the table is
// now three things - callsign, id and a details button - and the eight keys
// live in a labelled menu above it instead. The mapping itself is unchanged:
// the menu lists them in exactly the order the columns did.
TrackSortKey trackSortKeyForMenuIndex(int index);

// The menu's own label for a key, which is the whole reason the menu can carry
// eight keys where the headings could not: one spelled-out word at a time.
// Never empty and never shared between two keys - a menu with two entries
// reading the same thing is a menu that cannot be used.
const char* trackSortKeyName(TrackSortKey key);

// The key the list opens sorted by. It is the CALLSIGN on purpose: distance is
// the more useful order but it is empty until a receiver position has been
// set, so a list that opened on it would open sorted by nothing at all for
// every new user. This constant is the single statement of that choice - the
// window seeds its remembered sort key through
// trackSortKeyForMenuIndex(kTrackSortDefaultIndex).
inline constexpr int kTrackSortDefaultIndex = 0;

// --- how wide the three columns come out --------------------------------------
//
// WHY THIS IS A FUNCTION AND NOT TWO MAGIC WEIGHTS. The three-column list
// replaced eight truncated headings, and it was still truncating one of its
// own: at a 620 px map window the list gets about 173 px of table, "Callsign"
// wants 51 px of that and a six-character ID wants 42 px, and with four pixels
// of cell padding on every side of every cell the two of them were three
// pixels short. The heading came out "Callsi..." - a smaller version of the
// complaint that started the change.
//
// So the widths are DERIVED FROM WHAT THE TEXT ACTUALLY MEASURES, and the
// question "do the headings fit" has an answer the caller can act on rather
// than a look. Everything is passed in, so this is pure and testable; the
// caller measures with ImGui and does the drawing.
// The three widths are CONTENT widths, excluding cell padding, which is
// ImGui's own convention for a column width ("specify 100 and the column covers
// 100 + padding * 2"). Keeping the same convention is what lets these go
// straight into TableSetupColumn without a correction term nobody would
// remember to keep in step.
struct TrackListFit {
    float callsignW = 0.0f;
    float idW = 0.0f;
    float detailsW = 0.0f;
    // True when callsignW and idW are at least what their text needs. False is
    // not a failure to render - the columns are still laid out, just scaled
    // down - it is the caller's cue to spend less width on something else.
    bool headingsFit = false;
};

// `availW`     total width the table's columns have to share, cell padding
//              included (excluding scrollbar and borders).
// `callsignTextW` width of the WIDEST thing the callsign column must show
//              without truncating, which is the heading "Callsign" itself.
// `idTextW`    likewise for the ID column, which is the heading or a sample
//              identifier, whichever is wider.
// `detailsButtonW` width of the details button, which never shrinks: a button
//              squeezed to nothing cannot be pressed.
// `cellPadX`   ImGui's per-side cell padding.
TrackListFit trackListFit(float availW, float callsignTextW, float idTextW,
                          float detailsButtonW, float cellPadX);

// Height to give a scrolling table so that a line of text drawn AFTER it is
// still on screen.
//
// An ImGui table with ScrollY and the default outer_size takes all the height
// its parent has left, so anything drawn under it lands below the fold - which
// is where the track table's "no receiver position" note went: present, correct
// and invisible until the pane was scrolled ten notches. Reserving the line
// before the table is drawn is the only thing that fixes it, because by the
// time the text is emitted the height is already spent.
//
// NEVER RETURNS ZERO FOR A RESERVED LINE: zero is ImGui's own "use everything
// left", so a pane too short to hold both would silently reintroduce the bug
// instead of producing a squeezed table.
float tableHeightReservingLines(float availY, float lineHeight, int reservedLines);

// Stable, so rows that tie keep the order the previous sort left them in.
//
// UNKNOWNS SORT LAST IN BOTH DIRECTIONS. Sorting by altitude to find the high
// traffic and getting a screenful of aircraft whose altitude is unknown is not
// a sort, and reversing it to escape them would only move the same useless
// block to the other end.
void sortTrackRows(std::vector<TrackRow>& rows, TrackSortKey key, bool ascending);

// --- the target detail block --------------------------------------------------
//
// ONE BLOCK, THREE PLACES. The map's hover tooltip, the list row's hover
// tooltip and the details window all show the same thing about a target, and
// they showed it from two hand-written copies until the altitude band, the
// units and the registry fields each changed in one copy and not the other.
// The text is BUILT HERE, as data, and drawn by one renderer; nothing that
// decides what a line says lives in a drawing function any more.
//
// The strings are built rather than drawn so the block is testable at all:
// what a user complains about is the wording and the units, and neither is
// reachable from a test once it has gone into ImGui.

// One line of the block. `text` is the whole line, label and value together,
// because the label is padded to a fixed width to make a column and splitting
// it would let the two halves disagree about that width.
struct TrackDetailLine {
    std::string text;
    // false = the source does not know this, drawn dimmed. "0 kt" and "no
    // speed reported" are different facts and must not look the same.
    bool known = true;
    bool separatorAfter = false;
    // The callsign at the top, drawn without a label.
    bool heading = false;
};

// Everything the block can say about one target, already extracted from the
// host's track and from the track-info cache. A struct rather than fourteen
// parameters, and plain values rather than pointers into either, so the
// builder has nothing to dereference and a test can state a case in one
// initialiser.
struct TrackDetailInput {
    std::string label;   // callsign where one is decoded, id otherwise
    std::string id;
    std::string source;  // the plugin that reported it
    double latDeg = 0.0;
    double lonDeg = 0.0;
    // NaN means the source does not know, exactly as the ABI says.
    double altM = std::numeric_limits<double>::quiet_NaN();
    double speedMps = std::numeric_limits<double>::quiet_NaN();
    double courseDeg = std::numeric_limits<double>::quiet_NaN();
    std::uint64_t ageMs = 0;

    // The receiver's own position, which is what range and bearing are
    // measured from and which the host may simply not have.
    bool hasHome = false;
    double homeLatDeg = 0.0;
    double homeLonDeg = 0.0;

    // The track-info plugin's answer. `infoActive` is whether such a plugin is
    // installed at all - with none, the registry lines are absent rather than
    // empty, because "no plugin" and "plugin says unknown" are different facts.
    bool infoActive = false;
    bool infoPending = false;  // asked, nothing back yet
    bool infoKnown = false;    // answered, and it had an entry
    std::string registration;
    std::string typeCode;
    std::string typeName;
    std::string operatorName;
    std::string country;
};

// The block, in the order it is read: who, then what the registry knows, then
// what the radio heard.
std::vector<TrackDetailLine> buildTrackDetailLines(const TrackDetailInput& in);

// ============================================================================
// Implementation. Header-only because every piece of it is a handful of lines
// with no state to hide, and because a .cpp added to src/gui would have to be
// picked up by a CONFIGURE_DEPENDS glob that needs two build passes to see it.
// ============================================================================

namespace detail {
inline constexpr double kPi = 3.14159265358979323846;
inline constexpr double kEarthRadiusKm = 6371.0;
inline constexpr double kMetresToFeet = 3.28084;
inline double rad(double deg) { return deg * kPi / 180.0; }
inline double nan() { return std::numeric_limits<double>::quiet_NaN(); }

// Any longitude, or any DIFFERENCE of longitudes, folded into -180..180.
//
// Written as fmod-then-lift rather than as a pair of if statements because the
// input is not always within one turn of the range: a destination past the
// antipode leaves atan2 having added up to 180 degrees to a starting longitude
// that was already 180, and a difference of two longitudes spans -360..360.
// A single "if (x > 180) x -= 360" silently leaves those cases out of range.
//
// +180 comes back as -180, which is the same meridian; callers that care about
// the sign at that one seam should compare its magnitude.
inline double normaliseLonDeg(double lonDeg) {
    double l = std::fmod(lonDeg + 180.0, 360.0);
    if (l < 0.0) { l += 360.0; }
    return l - 180.0;
}
}  // namespace detail

inline double greatCircleKm(double lat1Deg, double lon1Deg, double lat2Deg,
                            double lon2Deg) {
    const double p1 = detail::rad(lat1Deg);
    const double p2 = detail::rad(lat2Deg);
    const double dp = detail::rad(lat2Deg - lat1Deg);
    const double dl = detail::rad(lon2Deg - lon1Deg);
    // No unwrapping of dl is needed for the antimeridian: only sin(dl/2) is
    // used, and sin is periodic, so +179.5 to -179.5 gives the same term as
    // -0.5 to +0.5 does. The naive "difference of longitudes times 111 km"
    // version is the one that calls that pair 359 degrees apart.
    double a = std::sin(dp * 0.5) * std::sin(dp * 0.5) +
               std::cos(p1) * std::cos(p2) * std::sin(dl * 0.5) * std::sin(dl * 0.5);
    // CLAMPED, because `a` is a mathematically-in-range quantity that floating
    // point can push out of range: an exactly antipodal pair - (45,10) against
    // (-45,-170), say - sums to 1.0000000000000002, and sqrt(1 - a) of that is
    // NaN. A range column that reads "nan" for the far side of the world is a
    // range column with a domain error in it.
    if (a > 1.0) { a = 1.0; }
    if (a < 0.0) { a = 0.0; }
    return 2.0 * detail::kEarthRadiusKm * std::atan2(std::sqrt(a), std::sqrt(1.0 - a));
}

inline double initialBearingDeg(double lat1Deg, double lon1Deg, double lat2Deg,
                                double lon2Deg) {
    // The one case the formula cannot answer, taken out before it runs. See
    // the declaration: atan2(0, 0) is 0.0, which would read as due north.
    if (lat1Deg == lat2Deg && lon1Deg == lon2Deg) { return detail::nan(); }
    const double p1 = detail::rad(lat1Deg);
    const double p2 = detail::rad(lat2Deg);
    const double dl = detail::rad(lon2Deg - lon1Deg);
    const double y = std::sin(dl) * std::cos(p2);
    const double x = std::cos(p1) * std::sin(p2) - std::sin(p1) * std::cos(p2) * std::cos(dl);
    double b = std::atan2(y, x) * 180.0 / detail::kPi;
    // atan2 answers in -180..180 and a compass rose does not.
    if (b < 0.0) { b += 360.0; }
    if (b >= 360.0) { b -= 360.0; }
    return b;
}

inline LatLon destinationPoint(double latDeg, double lonDeg, double bearingDeg, double km) {
    const double p1 = detail::rad(latDeg);
    const double b = detail::rad(bearingDeg);
    // The distance enters as an ANGLE - the arc divided by the radius - which
    // is what makes the whole thing exact instead of a small-offset
    // approximation, and what lets it stay right at a thousand kilometres and
    // past the pole.
    const double d = km / detail::kEarthRadiusKm;
    double sinP2 = std::sin(p1) * std::cos(d) + std::cos(p1) * std::sin(d) * std::cos(b);
    // CLAMPED for the same reason greatCircleKm clamps: this is a
    // mathematically-in-range quantity that rounding can push a few ulps
    // outside it - a departure from a pole, or a distance of exactly half the
    // circumference - and asin of 1.0000000000000002 is NaN. A NaN latitude
    // would put a coverage vertex nowhere and take the polygon with it.
    if (sinP2 > 1.0) { sinP2 = 1.0; }
    if (sinP2 < -1.0) { sinP2 = -1.0; }
    const double p2 = std::asin(sinP2);
    // atan2 rather than asin for the longitude change, so the quadrant is
    // right for every bearing and a path that crosses the pole flips the
    // meridian by 180 instead of folding back on itself.
    const double y = std::sin(b) * std::sin(d) * std::cos(p1);
    const double x = std::cos(d) - std::sin(p1) * sinP2;
    const double l2 = detail::rad(lonDeg) + std::atan2(y, x);
    return LatLon{p2 * 180.0 / detail::kPi, detail::normaliseLonDeg(l2 * 180.0 / detail::kPi)};
}

inline double lonDeltaDeg(double fromLonDeg, double toLonDeg) {
    // The SHORT way round, which is the whole point: 179.5 E to 179.5 W is one
    // degree east, not 359 degrees west. A raw subtraction is what drew a
    // coverage outline straight across the world map for a receiver near the
    // antimeridian, because each vertex then sat a full turn away from its
    // neighbour. Reusing the same fmod-based normaliser the destination point
    // already applies, so the two cannot disagree about where +/-180 is.
    return detail::normaliseLonDeg(toLonDeg - fromLonDeg);
}

inline CoverageVertex coverageVertex(double homeLatDeg, double homeLonDeg, double bearingDeg,
                                     double km) {
    const LatLon d = destinationPoint(homeLatDeg, homeLonDeg, bearingDeg, km);
    return CoverageVertex{d.latDeg, lonDeltaDeg(homeLonDeg, d.lonDeg)};
}

inline int altitudeBandIndex(double altM) {
    if (!std::isfinite(altM)) { return -1; }
    const double ft = altM * detail::kMetresToFeet;
    if (ft < 1000.0) { return 0; }
    if (ft < 5000.0) { return 1; }
    if (ft < 10000.0) { return 2; }
    if (ft < 20000.0) { return 3; }
    if (ft < 30000.0) { return 4; }
    return 5;
}

inline const AltBandStyle& altBandStyle(int index) {
    // THE PALETTE. It walks warm to cool - orange, amber, lime, green, cyan,
    // indigo - which is the convention every altitude-coloured traffic display
    // uses and therefore the one a user arrives already able to read.
    //
    // CHOSEN TO SURVIVE BOTH BACKGROUNDS, which is the actual constraint here:
    // the map is either a near-black empty field or a pale, busy OSM raster,
    // and a palette tuned for one is invisible on the other. Every entry is a
    // SATURATED MID-LUMINANCE colour - none of them near white, none near
    // black - so each has contrast against both. That rules out the pale
    // yellows and dark navies an evenly-spaced hue ramp would otherwise
    // include, and the test pins the luminance window so a later "nicer"
    // palette cannot quietly drift out of it.
    //
    // NO RED ANYWHERE IN IT, deliberately: red is the aircraft KIND colour
    // (which an unknown-altitude target keeps) and the emergency colour, and
    // both of those have to win. A band that was also red would put an
    // ordinary aircraft at circuit height in the same colour as one squawking
    // 7700.
    static const AltBandStyle kBands[kAltBandCount] = {
        {255, 122, 20, "< 1 kft"},     // circuit, final approach
        {255, 190, 30, "1-5 kft"},     // departure, arrival
        {156, 220, 40, "5-10 kft"},    // low level, light aircraft cruise
        {0, 205, 130, "10-20 kft"},    // climb, descent, turboprop cruise
        {40, 190, 240, "20-30 kft"},   // upper climb and descent
        {130, 140, 255, "> 30 kft"},   // jet cruise
    };
    if (index < 0) { index = 0; }
    if (index >= kAltBandCount) { index = kAltBandCount - 1; }
    return kBands[index];
}

inline int orbitBandIndex(double altM) {
    if (!std::isfinite(altM)) { return -1; }
    // Metres in, kilometres inside - the mirror of altitudeBandIndex's metres
    // in, feet inside. A satellite plugin reports altM in metres like every
    // other track source does, so the conversion belongs here and not at the
    // eight call sites that would each have to remember it.
    const double km = altM / 1000.0;
    if (km < 500.0) { return 0; }
    if (km < 750.0) { return 1; }
    if (km < 850.0) { return 2; }
    if (km < 1200.0) { return 3; }
    return 4;
}

inline const AltBandStyle& orbitBandStyle(int index) {
    // THE PALETTE, from the design handoff's own BANDS array, walking warm to
    // cool exactly as the aviation one does so a user reads both the same way
    // round. Two of the five ARE theme constants rather than colours invented
    // here: the < 500 km band is theme::kGold (D9B23C) and the 500-750 km band
    // is theme::kPhosphor (8FD9A0). They are written out as channels because
    // this header is deliberately free of ImGui - AltBandStyle is three bytes
    // and a label, theme.hpp's constants are ImU32 - and this is the only place
    // the hex is repeated.
    //
    // THE SAME TWO CONSTRAINTS AS THE AVIATION PALETTE, and they were MEASURED
    // against it rather than judged by eye, because "distinguishable" is not an
    // opinion when the aviation palette already fixes the standard.
    //
    //   READABLE OVER BOTH GROUNDS. The map is either a near-black empty field
    //   or a pale, busy raster. Every entry is mid-luminance - the five sit at
    //   176, 188, 175, 154 and 158 on the 601 weighting, inside the 90..210
    //   window the aviation palette's test pins - so none is lost to either.
    //
    //   DISTINGUISHABLE FROM EACH OTHER. The closest pair here is 39.4 in CIE
    //   Lab (1976); the aviation palette's own closest pair, its orange against
    //   its amber, is 38.8. So this ladder is no harder to read than the one
    //   that shipped.
    //
    //   AND NEVER THE EMERGENCY COLOUR. Nothing here is red, for the reason the
    //   aviation comment gives: red is the kind colour and the emergency
    //   colour and both have to win. The nearest any band comes to theme
    //   ::kAlarmHot (E07A4E) is 41.7, and to theme::kAlarm (B8552F) 49.1 -
    //   where the aviation palette's own low band comes within 30.8 of the
    //   first.
    //
    // ONE COLOUR WAS MOVED FROM THE HANDOFF, and this is the whole of it. The
    // handoff's high band was B87FD9, which measures 30.3 from the 850-1200 km
    // band beside it - below the 38.8 the shipped aviation palette establishes
    // as this product's floor, and the two are adjacent bands, which is the
    // pair a reader is most often asked to tell apart. Rotating it towards
    // magenta to C97AE8 - a shift of 12.3, still plainly the same violet -
    // opens that pair to 41.3 and makes 39.4 the ladder's floor instead.
    static const AltBandStyle kBands[kOrbitBandCount] = {
        {217, 178, 60, "< 500 km"},      // the station and the very low orbits
        {143, 217, 160, "500-750 km"},   // amateur and rideshare
        {111, 201, 217, "750-850 km"},   // the sun-synchronous weather belt
        {127, 155, 224, "850-1200 km"},  // the higher sun-synchronous orbits
        {201, 122, 232, "> 1200 km"},    // everything above
    };
    if (index < 0) { index = 0; }
    if (index >= kOrbitBandCount) { index = kOrbitBandCount - 1; }
    return kBands[index];
}

namespace detail {
// The legend arithmetic, once, for both ladders. Written as one function
// because the fixed parts - pad, swatch, pad, label, pad - have to agree with
// the drawing code, and when the aviation width and the drawing code were
// written separately they did not.
template <typename BandStyleAt, typename MeasureTextWidth>
inline float legendWidth(int count, BandStyleAt&& bandStyleAt,
                         MeasureTextWidth&& measureTextWidth) {
    // The LONGEST label decides the width, so the panel is sized by the one
    // that would otherwise overflow rather than by the one that happened to be
    // measured. All of them are measured every call: this runs once per frame
    // and measuring a handful of short strings is nothing beside the drawing it
    // precedes.
    float longest = 0.0f;
    for (int i = 0; i < count; ++i) {
        const float w = measureTextWidth(bandStyleAt(i).label);
        if (w > longest) { longest = w; }
    }
    return kAltLegendPad + kAltLegendSwatch + kAltLegendPad + longest + kAltLegendPad;
}
}  // namespace detail

template <typename MeasureTextWidth>
inline float altLegendWidth(MeasureTextWidth&& measureTextWidth) {
    return detail::legendWidth(kAltBandCount, altBandStyle, measureTextWidth);
}

template <typename MeasureTextWidth>
inline float orbitLegendWidth(MeasureTextWidth&& measureTextWidth) {
    return detail::legendWidth(kOrbitBandCount, orbitBandStyle, measureTextWidth);
}

inline TrackSortKey trackSortKeyForMenuIndex(int index) {
    switch (index) {
        case 0: return TrackSortKey::Label;
        case 1: return TrackSortKey::Id;
        case 2: return TrackSortKey::Altitude;
        case 3: return TrackSortKey::Speed;
        case 4: return TrackSortKey::Course;
        case 5: return TrackSortKey::Distance;
        case 6: return TrackSortKey::Bearing;
        case 7: return TrackSortKey::Age;
        default: return TrackSortKey::Label;
    }
}

inline const char* trackSortKeyName(TrackSortKey key) {
    switch (key) {
        case TrackSortKey::Label: return "Callsign";
        case TrackSortKey::Id: return "ID";
        // UNITS IN THE NAME, because the menu is now the only place they are
        // written down: the columns that carried "Alt ft" and "Dist km" are
        // gone, and a distance a user cannot name the unit of is not a
        // distance.
        case TrackSortKey::Altitude: return "Altitude (ft)";
        case TrackSortKey::Speed: return "Speed (kt)";
        case TrackSortKey::Course: return "Course (deg)";
        case TrackSortKey::Distance: return "Distance (km)";
        case TrackSortKey::Bearing: return "Bearing (deg)";
        case TrackSortKey::Age: return "Age (s)";
    }
    return "Callsign";
}

inline TrackListFit trackListFit(float availW, float callsignTextW, float idTextW,
                                 float detailsButtonW, float cellPadX) {
    TrackListFit f;
    // Nothing here may produce a negative width or a NaN: these numbers become
    // ImGui column widths and weights, and a negative weight lays the table out
    // inside out. Every input is clamped at the door rather than trusted.
    const float pad = 2.0f * (cellPadX > 0.0f ? cellPadX : 0.0f);
    const float needC = (callsignTextW > 0.0f ? callsignTextW : 0.0f);
    const float needI = (idTextW > 0.0f ? idTextW : 0.0f);
    f.detailsW = (detailsButtonW > 0.0f ? detailsButtonW : 0.0f);

    // ALL THREE COLUMNS PAY THE PADDING, which is why it is subtracted three
    // times and not once. Twenty-four pixels of a two-hundred-pixel list at
    // ImGui's default four per side is most of the width the headings were
    // missing.
    float rest = availW - f.detailsW - 3.0f * pad;
    if (!(rest > 0.0f)) { rest = 0.0f; }  // written positively so NaN lands here

    const float needTotal = needC + needI;
    if (!(needTotal > 0.0f)) {
        // No text to fit. Split what is left rather than dividing by zero.
        f.callsignW = rest * 0.5f;
        f.idW = rest - f.callsignW;
        f.headingsFit = false;
        return f;
    }
    if (rest >= needTotal) {
        // SURPLUS GOES OUT IN PROPORTION TO NEED, not in equal shares: a wide
        // list should spend its extra pixels on the column whose contents are
        // long, which is the callsign. Splitting evenly would leave a six
        // character ID column as wide as a name. It also means the two widths
        // are always in the ratio of their text, whatever the surplus - which
        // is exactly what makes them usable as ImGui stretch weights.
        const float surplus = rest - needTotal;
        f.callsignW = needC + surplus * (needC / needTotal);
        f.idW = needI + surplus * (needI / needTotal);
        f.headingsFit = true;
    } else {
        // GENUINELY TOO NARROW. Both are scaled by the same factor so the two
        // headings degrade together instead of one column eating the other,
        // and the caller is told so it can free width elsewhere.
        const float k = rest / needTotal;
        f.callsignW = needC * k;
        f.idW = needI * k;
        f.headingsFit = false;
    }
    return f;
}

inline float tableHeightReservingLines(float availY, float lineHeight, int reservedLines) {
    if (reservedLines <= 0) { return 0.0f; }
    const float wanted = availY - lineHeight * static_cast<float>(reservedLines);
    // The floor is one line rather than zero, and the reason is the whole point
    // of the function: zero is ImGui's "use everything left", so clamping a
    // squeezed pane to zero would hand the reserved line's space straight back
    // to the table. One line of table plus one line of text is unusable but
    // honest; a table that has silently eaten the text is not.
    const float floorPx = (lineHeight > 1.0f) ? lineHeight : 1.0f;
    return (wanted > floorPx) ? wanted : floorPx;
}

inline void CoverageMap::record(double bearingDeg, double km) {
    // Everything that is not a measurement, refused in one place. NaN arrives
    // here honestly - it is what initialBearingDeg returns for a target on top
    // of the receiver - and the rest is what a broken plugin can produce.
    if (!std::isfinite(bearingDeg) || !std::isfinite(km)) { return; }
    if (km <= 0.0) { return; }
    // Half the earth's circumference is 20015 km; nothing terrestrial is heard
    // further, and a value past it is arithmetic gone wrong upstream.
    if (km > 20100.0) { return; }

    double b = std::fmod(bearingDeg, 360.0);
    if (b < 0.0) { b += 360.0; }
    int bucket = static_cast<int>(b / kBucketDeg);
    // fmod plus a division can land exactly on the count for a bearing a hair
    // under 360; clamped rather than trusted, because the alternative is a
    // write one past the end of the array.
    if (bucket < 0) { bucket = 0; }
    if (bucket >= kBuckets) { bucket = kBuckets - 1; }

    if (maxKm_[bucket] == 0.0) { ++filled_; }
    if (km > maxKm_[bucket]) { maxKm_[bucket] = km; }
}

inline double CoverageMap::maxKm(int bucket) const {
    if (bucket < 0 || bucket >= kBuckets) { return 0.0; }
    return maxKm_[bucket];
}

inline void CoverageMap::reset() {
    for (int i = 0; i < kBuckets; ++i) { maxKm_[i] = 0.0; }
    filled_ = 0;
}

inline double CoverageMap::peakKm() const {
    double best = 0.0;
    for (int i = 0; i < kBuckets; ++i) {
        if (maxKm_[i] > best) { best = maxKm_[i]; }
    }
    return best;
}

inline void sortTrackRows(std::vector<TrackRow>& rows, TrackSortKey key, bool ascending) {
    // The numeric key for a row, and whether it HAS one. Split apart because
    // the unknown rule is about the second and the ordering is about the first,
    // and folding them together is how a NaN ends up compared with < and
    // sorting into an arbitrary place - or, worse, breaking the strict weak
    // ordering std::sort requires and walking off the end of the vector.
    const auto value = [key](const TrackRow& r) -> double {
        switch (key) {
            case TrackSortKey::Altitude: return r.altM;
            case TrackSortKey::Speed: return r.speedMps;
            case TrackSortKey::Course: return r.courseDeg;
            case TrackSortKey::Distance: return r.distanceKm;
            case TrackSortKey::Bearing: return r.bearingDeg;
            case TrackSortKey::Age: return static_cast<double>(r.ageMs);
            default: return 0.0;
        }
    };

    const bool textual = (key == TrackSortKey::Label || key == TrackSortKey::Id);
    std::stable_sort(rows.begin(), rows.end(),
                     [&](const TrackRow& a, const TrackRow& b) {
                         if (textual) {
                             const std::string& sa =
                                 (key == TrackSortKey::Label) ? a.label : a.id;
                             const std::string& sb =
                                 (key == TrackSortKey::Label) ? b.label : b.id;
                             return ascending ? (sa < sb) : (sb < sa);
                         }
                         const double va = value(a);
                         const double vb = value(b);
                         const bool ka = std::isfinite(va);
                         const bool kb = std::isfinite(vb);
                         // Unknowns sink to the bottom in BOTH directions, so
                         // reversing a sort never brings a block of blanks to
                         // the top. Two unknowns are equal, which is what keeps
                         // the ordering strict and weak and the sort stable.
                         if (ka != kb) { return ka; }
                         if (!ka) { return false; }
                         return ascending ? (va < vb) : (vb < va);
                     });
}

namespace detail {

// One formatted line. Bounded: every string reaching it is either a fixed
// literal or a field the info cache has already capped at 128 bytes, and
// snprintf truncates rather than trusting that.
inline std::string detailPrintf(const char* fmt, ...) {
    char buf[320];
    va_list ap;
    va_start(ap, fmt);
    const int n = std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) { return std::string(); }
    buf[sizeof buf - 1] = '\0';
    return std::string(buf);
}

}  // namespace detail

inline std::vector<TrackDetailLine> buildTrackDetailLines(const TrackDetailInput& in) {
    std::vector<TrackDetailLine> out;
    out.reserve(16);

    TrackDetailLine head;
    head.text = in.label;
    head.heading = true;
    head.separatorAfter = true;
    out.push_back(std::move(head));

    // --- what the registry knows ---------------------------------------------
    // Absent entirely when no track-info plugin is installed. An empty "reg"
    // line would say the aircraft has no registration, which is a claim about
    // the aircraft rather than about the host.
    if (in.infoActive) {
        const std::size_t before = out.size();
        if (in.infoKnown) {
            if (!in.registration.empty()) {
                out.push_back({detail::detailPrintf("reg     %s", in.registration.c_str()),
                               true, false, false});
            }
            // The spelled-out type where the source has one, the code
            // otherwise: "737NG 8K5/W" tells a user what is overhead and
            // "B738" does not.
            const std::string& type = !in.typeName.empty() ? in.typeName : in.typeCode;
            if (!type.empty()) {
                out.push_back(
                    {detail::detailPrintf("type    %s", type.c_str()), true, false, false});
            }
            if (!in.operatorName.empty()) {
                out.push_back({detail::detailPrintf("oper    %s", in.operatorName.c_str()),
                               true, false, false});
            }
            if (!in.country.empty()) {
                out.push_back({detail::detailPrintf("reg'd   %s", in.country.c_str()), true,
                               false, false});
            }
        } else if (in.infoPending) {
            out.push_back({"looking up...", false, false, false});
        }
        // The separator belongs to the block, so a plugin that answered "not in
        // my data" - which emits no lines at all - does not leave a rule
        // floating with nothing above it.
        if (out.size() > before) { out.back().separatorAfter = true; }
    }

    // --- what the radio heard -------------------------------------------------
    out.push_back({detail::detailPrintf("id      %s", in.id.c_str()), true, false, false});
    out.push_back(
        {detail::detailPrintf("from    %s", in.source.c_str()), true, false, false});
    out.push_back({detail::detailPrintf("pos     %.5f, %.5f", in.latDeg, in.lonDeg), true,
                   false, false});

    if (!std::isnan(in.altM)) {
        // The band is NAMED as well as measured, so the colour on the map and
        // the figure here can be tied together without counting swatches in
        // the legend.
        out.push_back({detail::detailPrintf("alt     %.0f m (%.0f ft, %s)", in.altM,
                                            in.altM * 3.28084,
                                            altBandStyle(altitudeBandIndex(in.altM)).label),
                       true, false, false});
    } else {
        out.push_back({"alt     unknown", false, false, false});
    }

    if (!std::isnan(in.speedMps)) {
        out.push_back({detail::detailPrintf("speed   %.0f kt", in.speedMps * 1.94384), true,
                       false, false});
    } else {
        out.push_back({"speed   unknown", false, false, false});
    }

    if (!std::isnan(in.courseDeg)) {
        out.push_back(
            {detail::detailPrintf("course  %.0f deg", in.courseDeg), true, false, false});
    } else {
        out.push_back({"course  unknown", false, false, false});
    }

    // RANGE AND BEARING SURVIVED THE COLUMNS BEING CUT, and this is where they
    // live now: the list has no room for them, so every place that shows a
    // target in detail shows them.
    if (in.hasHome) {
        const double km =
            greatCircleKm(in.homeLatDeg, in.homeLonDeg, in.latDeg, in.lonDeg);
        const double brg =
            initialBearingDeg(in.homeLatDeg, in.homeLonDeg, in.latDeg, in.lonDeg);
        // A target at the receiver's own coordinates has no bearing from it,
        // and printing "nan deg" would be worse than saying so.
        if (std::isnan(brg)) {
            out.push_back({detail::detailPrintf("range   %.1f km, bearing undefined", km),
                           true, false, false});
        } else {
            out.push_back({detail::detailPrintf("range   %.1f km at %.0f deg", km, brg),
                           true, false, false});
        }
    }

    out.push_back({detail::detailPrintf("age     %.1f s",
                                        static_cast<double>(in.ageMs) / 1000.0),
                   true, false, false});
    return out;
}

}  // namespace cascade::gui
