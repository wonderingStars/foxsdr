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
#include <cstddef>
#include <cstdint>
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

// The key a table column sorts by. ONE mapping, so the column order and the
// window's remembered sort state cannot drift apart; an index outside the
// table's columns answers with the default rather than with whatever the last
// case happened to be.
TrackSortKey trackSortKeyForColumn(int columnIndex);

// The column the table opens sorted by. It is the CALLSIGN column on purpose:
// distance is the more useful order but it is empty until a receiver position
// has been set, so a table that opened on it would open sorted by nothing at
// all for every new user. This constant is the single statement of that
// choice - the window seeds its remembered sort key through
// trackSortKeyForColumn(kTrackSortDefaultColumn), and a static_assert beside
// the column that carries ImGuiTableColumnFlags_DefaultSort pins the other
// half, because when the two were written independently they disagreed and the
// table silently opened in an order the code said it would not.
inline constexpr int kTrackSortDefaultColumn = 0;

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

template <typename MeasureTextWidth>
inline float altLegendWidth(MeasureTextWidth&& measureTextWidth) {
    // The LONGEST label decides the width, so the panel is sized by the one
    // that would otherwise overflow rather than by the one that happened to be
    // measured. All six are measured every call: this runs once per frame and
    // measuring six short strings is nothing beside the drawing it precedes.
    float longest = 0.0f;
    for (int i = 0; i < kAltBandCount; ++i) {
        const float w = measureTextWidth(altBandStyle(i).label);
        if (w > longest) { longest = w; }
    }
    return kAltLegendPad + kAltLegendSwatch + kAltLegendPad + longest + kAltLegendPad;
}

inline TrackSortKey trackSortKeyForColumn(int columnIndex) {
    switch (columnIndex) {
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

}  // namespace cascade::gui
