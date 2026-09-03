// scope_view.hpp - the ADS-B radar scope: a plan-position indicator with the
// receiver at the middle of it, and the arithmetic that decides where every
// mark on it goes.
//
// WHAT THIS IS, AND WHY IT IS NOT THE MAP. A beta tester sent a photograph of
// a dedicated ADS-B receiver - a circular scope, range rings, a track count in
// one corner and a range readout in the other, and a panel down the side
// describing whichever aircraft was selected. That layout is not one product's
// idea: the plan-position indicator has been how a radar picture is presented
// since the 1940s, and its whole argument is that RANGE AND BEARING FROM ONE
// PLACE are what an operator actually reads. The map answers "where is
// everything"; the scope answers "how far away, and which way do I look". They
// are different questions and they want different pictures, which is why this
// is a second renderer rather than a mode bolted onto MapView.
//
// It is a HOST mode, not a plugin, and that is forced rather than chosen: the
// plugin ABI declares nine capabilities and not one of them is a canvas, so
// nothing on the plugin side of the boundary can draw a pixel. The scope is
// fed by the tracks the ADS-B plugin already publishes through
// PluginUi::tracks(), exactly as the map pages are.
//
// WHAT IS PURE AND WHY. Everything below the class - the ladder of range
// steps, the range and bearing to a target, the polar-to-screen mapping, the
// in-range test and every string a readout or a panel row prints - is a
// function of its arguments and lives in this header. That is the only half a
// test can reach: what a user complains about is a target in the wrong place
// or a field that reads wrong, and neither is observable once it has gone into
// an ImDrawList. The drawing in scope_view.cpp therefore contains no
// arithmetic decisions of its own.
//
// GUI THREAD ONLY, like everything else in this directory.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#ifndef CASCADE_GUI_SCOPE_VIEW_HPP
#define CASCADE_GUI_SCOPE_VIEW_HPP

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <map>
#include <string>
#include <vector>

#include "core/plugin_ui.hpp"
#include "gui/track_metrics.hpp"

namespace cascade::gui {

class BasemapCache;
class TrackInfoCache;

// --- the range ladder --------------------------------------------------------

// THE RANGE STEPS, in nautical miles. Discrete rather than continuous because
// a scope's range is a SETTING an operator states and returns to ("I am on the
// hundred-mile scale"), not a zoom they scrub - and because every ring, every
// label and the corner readout are derived from it, so a free-running value
// would print ranges like "173 NM" on rings nobody chose.
//
// The ladder itself is the one every ADS-B receiver of this shape offers:
// roughly doubling steps from a circuit-sized 10 NM out to 400, which is past
// the horizon for a ground station at any sane antenna height and therefore
// past anything this radio can hear.
inline constexpr int kScopeRangesNm[] = {10, 25, 50, 100, 200, 400};
inline constexpr int kScopeRangeCount =
    static_cast<int>(sizeof(kScopeRangesNm) / sizeof(kScopeRangesNm[0]));

// The range a scope nobody has configured opens on. 200 NM is the one that
// shows a receiver's whole realistic catchment without the picture being
// mostly empty ring: a good ADS-B site hears 200-250 NM, so this is "show me
// everything I can hear" rather than a value chosen for a screenshot.
inline constexpr int kScopeDefaultRangeNm = 200;

// How many range rings are drawn, and therefore what fraction of full scale
// each one sits at: quarter, half, three-quarter, full.
inline constexpr int kScopeRingCount = 4;

// A nautical mile is 1852 metres EXACTLY - it has been a defined quantity
// since 1929, not a measured one - so this conversion is not an approximation
// and needs no tolerance anywhere it is used.
inline constexpr double kKmPerNm = 1.852;

// The index on the ladder of the value closest to `nm`. Total by construction:
// every integer has a nearest entry, so there is no "not on the ladder" answer
// for a caller to forget to handle.
//
// TIES GO TO THE SMALLER RANGE. 150 NM is exactly between 100 and 200, and the
// tighter of the two is the safer reading of an ambiguous instruction: a scope
// set shorter than asked still draws everything inside it correctly and says
// so in its own corner, where one set longer quietly claims reach the user did
// not ask for.
inline int scopeRangeIndex(int nm) {
    int best = 0;
    // long long throughout: `nm` arrives from a hand-edited config and may be
    // INT_MIN, where `nm - 10` in int arithmetic is undefined behaviour rather
    // than a large number.
    long long bestDist = std::llabs(static_cast<long long>(nm) -
                                    static_cast<long long>(kScopeRangesNm[0]));
    for (int i = 1; i < kScopeRangeCount; ++i) {
        const long long d =
            std::llabs(static_cast<long long>(nm) - static_cast<long long>(kScopeRangesNm[i]));
        if (d < bestDist) {  // strict, so a tie keeps the earlier - smaller - entry
            bestDist = d;
            best = i;
        }
    }
    return best;
}

// The ladder value at `index`, bounds-safe: an index outside the ladder is
// clamped rather than read past the end.
inline int scopeRangeNmAt(int index) {
    if (index < 0) { index = 0; }
    if (index >= kScopeRangeCount) { index = kScopeRangeCount - 1; }
    return kScopeRangesNm[index];
}

// The nearest legal range to `nm`. This is what the config sanitizer applies on
// load, and it is not tidiness: the renderer derives four ring radii, four ring
// labels and the corner readout from this number, so a hand-edited 173 would
// reach the drawing code as a scale with no rings anybody chose and a readout
// nobody could reproduce from the ladder. Snapping to the nearest keeps what
// the edit was reaching for; discarding it back to the default would throw the
// user's intent away for the sake of a number that was almost right.
inline int clampScopeRangeNm(int nm) { return scopeRangeNmAt(scopeRangeIndex(nm)); }

// One step up or down the ladder from `currentNm`, which is snapped to the
// ladder first so a config-edited value cannot make a step land nowhere.
//
// CLAMPED AT BOTH ENDS, NEVER WRAPPED. The mouse wheel drives this, and a
// wheel that turns 400 NM into 10 NM on one accidental notch throws the entire
// picture away and leaves the user to work out what happened. A control that
// stops at the end of its travel is a control that can be leaned on.
inline int scopeRangeStepped(int currentNm, int steps) {
    const long long target = static_cast<long long>(scopeRangeIndex(currentNm)) +
                             static_cast<long long>(steps);
    if (target < 0) { return kScopeRangesNm[0]; }
    if (target >= kScopeRangeCount) { return kScopeRangesNm[kScopeRangeCount - 1]; }
    return kScopeRangesNm[static_cast<int>(target)];
}

// --- where a target sits ------------------------------------------------------

// A target as the scope thinks of it: how far, and which way.
struct ScopePolar {
    double rangeNm = std::numeric_limits<double>::quiet_NaN();
    // NaN when the target is on top of the receiver. There is no direction
    // from a point to itself, and the scope must not print "000" - which a
    // user reads as due north - for an aircraft directly overhead. See
    // initialBearingDeg, which is where that decision is made and tested.
    double bearingDeg = std::numeric_limits<double>::quiet_NaN();
};

// Range and bearing from the receiver to a target, THROUGH THE TESTED PAIR IN
// track_metrics.hpp rather than through a second copy of the geodesy. That
// matters more here than anywhere else in the application: the scope is a
// picture made entirely of ranges and bearings, so a private haversine with a
// sign wrong would not be a wrong column, it would be a wrong picture - and
// the hover readout beside it would disagree with the marks on it.
inline ScopePolar scopeRelative(double rxLatDeg, double rxLonDeg, double latDeg,
                                double lonDeg) {
    ScopePolar p;
    p.rangeNm = greatCircleKm(rxLatDeg, rxLonDeg, latDeg, lonDeg) / kKmPerNm;
    p.bearingDeg = initialBearingDeg(rxLatDeg, rxLonDeg, latDeg, lonDeg);
    return p;
}

// A place on the scope face, in screen pixels.
struct ScopePoint {
    double x = 0.0;
    double y = 0.0;
};

// THE POLAR MAPPING, which is the whole geometry of a plan-position indicator:
// distance from the middle is proportional to range, and the angle round is
// the true bearing with north straight up. Screen y grows DOWNWARDS, which is
// why north subtracts.
//
// EVERY DEGENERATE INPUT LANDS AT THE CENTRE, deliberately and not as a
// fallback nobody thought about:
//   - a zero range IS the centre;
//   - a NaN bearing means the target is on top of the receiver (see
//     ScopePolar), which is also the centre, and is the one case where a
//     bearing genuinely does not exist;
//   - a NaN range, or a full scale of zero, cannot be drawn at all - and
//     scopeInRange rejects both before anything asks for a position, so this
//     branch exists to keep a NaN out of the draw list rather than to place a
//     mark.
inline ScopePoint scopeProject(double centreX, double centreY, double radiusPx,
                               double rangeNm, double bearingDeg, double fullScaleNm) {
    ScopePoint p{centreX, centreY};
    if (!(fullScaleNm > 0.0) || !(radiusPx > 0.0)) { return p; }
    if (!std::isfinite(rangeNm) || !std::isfinite(bearingDeg)) { return p; }
    const double r = radiusPx * (rangeNm / fullScaleNm);
    const double a = bearingDeg * 3.14159265358979323846 / 180.0;
    p.x = centreX + r * std::sin(a);
    p.y = centreY - r * std::cos(a);
    return p;
}

// Whether a target belongs on a scope set to `fullScaleNm`.
//
// THE OUTERMOST RING COUNTS AS INSIDE. A target at exactly full scale is ON
// the ring the scope draws, and a ring the user can see with nothing allowed
// to sit on it would be a boundary that lies about itself. Written as a
// positive finiteness test so a NaN range - which is what a broken position
// produces - falls out as "not on this scope" rather than being compared.
inline bool scopeInRange(double rangeNm, double fullScaleNm) {
    if (!(fullScaleNm > 0.0)) { return false; }
    if (!(rangeNm >= 0.0)) { return false; }  // rejects NaN too
    return rangeNm <= fullScaleNm;
}

// The range one ring sits at, ring 1 being the innermost. Bounds-safe: a ring
// index outside 1..kScopeRingCount is clamped, so a loop that is off by one
// draws a duplicate ring instead of a radius outside the scope face.
inline double scopeRingNm(int fullScaleNm, int ring) {
    if (ring < 1) { ring = 1; }
    if (ring > kScopeRingCount) { ring = kScopeRingCount; }
    return static_cast<double>(fullScaleNm) * static_cast<double>(ring) /
           static_cast<double>(kScopeRingCount);
}

// --- Mercator ----------------------------------------------------------------
//
// Normalised Web Mercator: the whole world is 0..1 in y, north at the top.
// Duplicated from the drawing side deliberately - this half of the scope has
// to stay free of ImGui and of the tile cache, and a projection is arithmetic.
inline double scopeMercY(double latDeg) {
    const double clamped = (latDeg > 85.05112878)    ? 85.05112878
                           : (latDeg < -85.05112878) ? -85.05112878
                                                     : latDeg;
    const double s = std::sin(clamped * 3.14159265358979323846 / 180.0);
    return 0.5 - std::log((1.0 + s) / (1.0 - s)) / (4.0 * 3.14159265358979323846);
}

inline double scopeMercLat(double y) {
    const double n = 3.14159265358979323846 * (1.0 - 2.0 * y);
    return 180.0 / 3.14159265358979323846 * std::atan(std::sinh(n));
}

// --- the view centre ---------------------------------------------------------

// A latitude and longitude, in degrees.
struct ScopeLatLon {
    double latDeg = 0.0;
    double lonDeg = 0.0;
};

// WHERE THE MIDDLE OF THE GLASS IS after the view has been dragged by
// (panXpx, panYpx) screen pixels.
//
// This is the number every other thing on the face is measured from, and
// getting it wrong is not a small visual error - it silently moves the whole
// picture relative to the map under it. It is here, in the ImGui-free half,
// precisely so it can be checked without a graphics context.
//
// SIGNS. Dragging RIGHT (panXpx > 0) carries the map right, so the point in
// the middle of the glass moves WEST - a smaller longitude. Dragging DOWN
// (panYpx > 0) carries the map down, so the middle moves NORTH, which in
// normalised Mercator is a SMALLER y. Both are the opposite of the pan's own
// sign, which is exactly the kind of thing that reads as correct in code and
// is wrong on screen.
//
// pixPerWorld is how many screen pixels one whole world-width spans. Zero or
// negative means there is no usable scale - the receiver is at a pole, or the
// face has not been laid out yet - and the answer is then the receiver itself
// rather than a coordinate derived by dividing by it.
inline ScopeLatLon scopeViewCentre(double rxLatDeg, double rxLonDeg, double panXpx,
                                   double panYpx, double pixPerWorld) {
    ScopeLatLon out{rxLatDeg, rxLonDeg};
    if (!(pixPerWorld > 0.0) || !std::isfinite(panXpx) || !std::isfinite(panYpx)) {
        return out;
    }
    // AN UNPANNED VIEW IS THE ANTENNA EXACTLY, not a value that has been
    // through a Mercator round trip. scopeMercLat(scopeMercY(lat)) is a log,
    // an atan and a sinh, and it does not come back bit-identical - so without
    // this the centre of an untouched scope drifts a few billionths of a
    // degree off the aerial it is supposed to BE. Harmless on screen and
    // exactly the kind of thing that makes an equality check downstream fail
    // for a reason nobody can see. The test caught it on the first run.
    if (panXpx != 0.0) {
        out.lonDeg = rxLonDeg - panXpx / pixPerWorld * 360.0;
    }
    if (panYpx != 0.0) {
        out.latDeg = scopeMercLat(scopeMercY(rxLatDeg) - panYpx / pixPerWorld);
    }
    return out;
}

// --- what the readouts say ----------------------------------------------------

namespace detail {

// One short formatted string. Bounded, and truncating rather than trusting:
// everything reaching it is a number or a field the info cache has already
// capped at 128 bytes.
// A compass value that will be printed to whole degrees. "%03.0f" rounds
// 359.6 up to 360, and 360 DEG is not a bearing - the same direction is 000.
// Wrapping BEFORE the rounding is what keeps the printed value a compass one.
inline double scopeWrapBearing(double deg) {
    if (!std::isfinite(deg)) { return deg; }
    // ROUND FIRST, THEN WRAP, and that order is the whole function. Wrapping
    // first cannot work: 359.5 - 360 is -0.5, which wraps straight back to
    // 359.5 and prints 360 anyway. It is the ROUNDED value that can land on
    // 360, so it is the rounded value that has to be brought back into range.
    double r = std::fmod(std::round(deg), 360.0);
    if (r < 0.0) { r += 360.0; }
    return r;
}

inline std::string scopePrintf(const char* fmt, ...) {
    char buf[192];
    va_list ap;
    va_start(ap, fmt);
    const int n = std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) { return std::string(); }
    buf[sizeof buf - 1] = '\0';
    return std::string(buf);
}

}  // namespace detail

// The label on one range ring. The unit is repeated on every ring rather than
// stated once, because a ring label is read on its own - the eye goes to the
// ring it is measuring against, not to the corner - and "100" beside a circle
// is a number without a dimension.
//
// "%.4g" rather than a fixed precision because the ladder's short end does not
// divide into four whole numbers: a 10 NM scope's rings are at 2.5, 5, 7.5 and
// 10, and "2 NM" on a ring at two and a half would be a wrong label rather
// than a rounded one.
inline std::string scopeRingLabel(int fullScaleNm, int ring) {
    return detail::scopePrintf("%.4g NM", scopeRingNm(fullScaleNm, ring));
}

// The top-left readout: how many aircraft are actually PLOTTED. Not how many
// the plugin reported - the scope drops stale targets and everything outside
// the current range, and a count that included either would contradict the
// picture it sits on, which is the exact complaint the map's own target count
// was rewritten to answer.
//
// "TRACKS" DOES NOT CONJUGATE. It is a legend on an instrument face, a fixed
// field of fixed width, and "1 TRACK" would change the readout's shape as the
// last aircraft leaves. A negative count cannot arise from the draw loop; it
// is floored anyway, because a readout is the wrong place to discover one.
inline std::string scopeTracksReadout(int count) {
    return detail::scopePrintf("%d TRACKS", count < 0 ? 0 : count);
}

// The top-right readout: the scale the scope is set to, which is what makes
// every ring on it readable. Always a ladder value, because that is the only
// thing the view accepts.
inline std::string scopeRangeReadout(int fullScaleNm) {
    return detail::scopePrintf("%d NM", fullScaleNm);
}

// The label on one bearing tick. THE FOUR CARDINALS GET LETTERS and everything
// else gets three digits, which is the compass-rose convention and the reason
// a scope can be read without counting round from the top: "N" is found
// instantly, "030" is then unambiguous. Three digits, because a bearing is
// always spoken and written as three ("zero three zero") and a bare "30" reads
// as a distance on a face covered in distances.
//
// Anything not a multiple of 30 still formats, so a future finer tick spacing
// needs no change here; anything outside 0..359 is folded into it rather than
// printed, because a bearing of 370 is not a direction.
inline std::string scopeBearingLabel(int deg) {
    int d = deg % 360;
    if (d < 0) { d += 360; }
    switch (d) {
        case 0: return "N";
        case 90: return "E";
        case 180: return "S";
        case 270: return "W";
        default: break;
    }
    return detail::scopePrintf("%03d", d);
}

// --- the detail panel ---------------------------------------------------------

// Everything the panel can say about one aircraft, already extracted from the
// track and from the track-info cache. Plain values rather than pointers into
// either, so the builder has nothing to dereference and a test can state a
// case in one initialiser - the same shape, and for the same reasons, as
// TrackDetailInput next door.
struct ScopeDetailInput {
    // The callsign where one has been decoded, and the id when it has not. The
    // panel's first row is an identity and must never be blank: an aircraft
    // with no callsign is still a specific aircraft, and its ICAO address is
    // what identifies it.
    std::string flight;
    std::string operatorName;
    std::string typeName;      // the spelled-out type where known, the code otherwise
    std::string registration;

    // Whether a track-info plugin is installed at all, and whether it has
    // answered yet. "No plugin" and "the plugin has no entry for this
    // aircraft" are different facts about different things, and a panel that
    // showed them identically would send the user looking for a fault in the
    // wrong place.
    bool infoActive = false;
    bool infoPending = false;

    // CASCADE_TRACK_FLAG_EMERGENCY. It is a flag and not a squawk - see
    // scopeUnavailableNote - so this is the whole of what the panel can
    // honestly say about the state a squawk would carry.
    bool emergency = false;

    // NaN means the source does not report it, exactly as the ABI says.
    double altM = std::numeric_limits<double>::quiet_NaN();
    double speedMps = std::numeric_limits<double>::quiet_NaN();
    double courseDeg = std::numeric_limits<double>::quiet_NaN();

    // Range and bearing are measured FROM somewhere, and without a receiver
    // position there is no somewhere. Distinct from a NaN range: one is the
    // host not knowing where it is, the other is a target position that made
    // no sense.
    bool hasRx = false;
    double rangeNm = std::numeric_limits<double>::quiet_NaN();
    double bearingDeg = std::numeric_limits<double>::quiet_NaN();
};

// One row of the panel: a fixed label in a column, and a value beside it.
//
// SPLIT, unlike TrackDetailLine, which is one padded string. The scope's panel
// is a two-column instrument readout - labels dim and left, values bright and
// aligned - and pre-padding them into one string would tie the layout to a
// monospaced font this application does not ship.
struct ScopeDetailLine {
    std::string label;
    std::string value;
    // false = the value is not a measurement (nothing reported it, or nothing
    // could have). Drawn dimmed: "0 KT" and "NO DATA" are different facts and
    // must not look the same.
    bool known = true;
    // The one row that outranks the rest of the panel's styling. See
    // buildScopeDetailLines.
    bool alert = false;
};

// The panel's field list, in the order it is read: who it is, what the registry
// knows, what state it is in, and then what the radio heard about where it is
// and where it is going.
std::vector<ScopeDetailLine> buildScopeDetailLines(const ScopeDetailInput& in);

// THE ONE LINE AT THE FOOT OF THE PANEL, and the reason it exists.
//
// The photographed receiver shows three things this one cannot: the squawk,
// the flight's route, and the operator's logo. None of them is a bug and none
// of them may be faked:
//
//   - SQUAWK. The ADS-B plugin decodes it. CascadeTrack has nowhere to put it:
//     the struct carries an id, a label, a position, an altitude, a course, a
//     speed, an age, a kind and a flags word, and the only thing the flags
//     word says about a squawk is CASCADE_TRACK_FLAG_EMERGENCY - one bit that
//     is set for 7500, 7600 and 7700 alike. Getting the four digits onto this
//     panel is A NEW ABI CAPABILITY (a per-track string, or a transponder
//     table alongside the track one), not a fix to anything here, and it would
//     have to be versioned so an older plugin against a newer host stays
//     loadable. Until then the STATUS row is the honest whole of it.
//   - ROUTE. Origin and destination are not broadcast by ADS-B at all; every
//     display that shows them has joined the callsign against a schedule
//     database over the network. This product ships no such database and makes
//     no such call.
//   - LOGO. An airline's mark is its trademark. Shipping a set of them is a
//     licensing question, not a drawing one.
//
// The line is SHOWN rather than the rows being left blank, because a blank row
// under a label reads as "we tried to read this and failed", which is a claim
// about the receiver's decoding that would not be true.
const char* scopeUnavailableNote();

// --- the renderer -------------------------------------------------------------

class ScopeView {
public:
    // Draws into the CURRENT ImGui window, filling `width` x `height`: the
    // scope square on the left, the detail panel down the right.
    //
    // ONLY AIRCRAFT ARE PLOTTED. This is the ADS-B scope, and its whole
    // coordinate system is range and bearing from a ground station; a
    // satellite at 500 km up or a ship two thousand miles away would be a mark
    // that meant something different from every mark beside it. The kind
    // filter is in the draw loop rather than in the caller so no caller can
    // forget it.
    //
    // `tiles` may be null, and is whenever no basemap plugin is installed -
    // which is the shipped configuration. With one, its tiles are drawn under
    // the scope face; without one, the compiled-in coastline is.
    //
    // `info` may be null too; when a track-info plugin is active it fills the
    // panel's OPERATOR, TYPE and REG rows.
    void draw(float width, float height,
              const std::vector<cascade::core::HostTrack>& tracks,
              BasemapCache* tiles = nullptr, TrackInfoCache* info = nullptr);

    // The receiver's own position - the centre of the scope, and the origin of
    // every range and bearing on it. Unset until the user provides one; see
    // hasReceiver, and see the caller, which draws an honest empty state
    // rather than a scope centred on 0N 0E (a real place, in the Gulf of
    // Guinea, about which this receiver knows nothing).
    void setReceiver(double latDeg, double lonDeg) {
        rxLat_ = latDeg;
        rxLon_ = lonDeg;
        hasRx_ = true;
    }
    void clearReceiver() { hasRx_ = false; }
    bool hasReceiver() const { return hasRx_; }

    // The scale, always a ladder value: setRangeNm snaps, so no path into this
    // class can install a range the renderer has no rings for.
    int rangeNm() const { return rangeNm_; }
    void setRangeNm(int nm) { rangeNm_ = clampScopeRangeNm(nm); }

    // The aircraft the panel is describing, empty when none. An id rather than
    // a pointer or an index, for the reason every other selection in this
    // application is an id: the host's track vector is rebuilt on every poll,
    // so an index would name a different aircraft a frame later and a pointer
    // would dangle.
    const std::string& selectedId() const { return selectedId_; }
    void setSelected(const std::string& id) { selectedId_ = id; }
    void clearSelection() { selectedId_.clear(); }

    // How many aircraft the last draw actually PLOTTED, which is the number
    // the corner readout printed. Exposed so the caller can say the same thing
    // elsewhere without running the filter a second time and risking a
    // different answer.
    int plottedCount() const { return plotted_; }

    // Whether the last draw asked the basemap for a tile. The caller needs
    // this to decide whether to run BasemapCache::endFrame(), whose eviction
    // pass must happen ONCE per frame and only after every surface that wanted
    // tiles has asked - see the note at its call site in app_window.cpp.
    bool askedForTiles() const { return askedTiles_; }

    // WHERE THE FACE IS LOOKING, as a screen-pixel offset of the receiver from
    // the middle of the tube. Zero puts the antenna at the centre, which is
    // where a radar normally keeps it.
    //
    // THE WHOLE POLAR FRAME MOVES TOGETHER - map, rings, bearing ticks,
    // targets, sweep and the receiver mark - so a ring labelled 200 NM is
    // still 200 NM from the ANTENNA after a pan, not from wherever the view
    // happens to be pointing. Panning a scope by moving only its basemap would
    // leave every ring measuring from a place the aircraft are no longer drawn
    // relative to, which is a lying instrument rather than a moved one.
    void resetPan() { hasView_ = false; }
    bool panned() const { return hasView_; }

private:
    bool hasRx_ = false;
    double rxLat_ = 0.0;
    double rxLon_ = 0.0;
    int rangeNm_ = kScopeDefaultRangeNm;
    std::string selectedId_;
    int plotted_ = 0;
    bool askedTiles_ = false;
    // WHERE THE MIDDLE OF THE GLASS IS, AS A PLACE - not as a pixel offset.
    //
    // Holding it as a pan in pixels meant it had to be divided by the current
    // scale to become a position, so CHANGING THE RANGE MOVED IT: zooming in
    // on something walked the view off it, which is the opposite of what
    // zooming is for. Held as a latitude and longitude, the point under the
    // centre dot is the same point at every range, and the zoom is anchored on
    // it by construction rather than by arithmetic that has to be kept right.
    //
    // hasView_ false means "the antenna", so an untouched scope needs no
    // coordinates and cannot drift from the aerial through rounding.
    bool hasView_ = false;
    double viewLat_ = 0.0;
    double viewLon_ = 0.0;
    // A drag that has moved the view must not also be read as the click that
    // recentres it, or every pan would end by jumping the view again.
    bool dragged_ = false;
    // WHICH SCREEN THE LCD IS SHOWING. 0 = HOME (what the face holds as a
    // whole), 1 = FLIGHT (the selected aircraft), 2 = SYS (what the instrument
    // itself is set to). Three screens rather than one long list because the
    // panel is 330 px wide and a scope is read at a glance: the counts and one
    // aircraft's details answer different questions and crowd each other badly
    // when stacked.
    int lcdScreen_ = 1;

    // WHAT THE INSTRUMENT IS SET TO, all of it reachable from the SYS screen.
    //
    // Every one of these is a real switch on a real thing drawn on the face,
    // because a settings page whose entries do nothing is worse than no
    // settings page. They are deliberately NOT persisted yet - they are view
    // preferences on one mode, and the config already carries more scope state
    // than it has tests for.
    bool optPhosphor_ = true;   // the green wash over the map
    bool optSweep_ = true;      // the turning wedge
    int optFilter_ = 0;         // 0 = all, 1 = alerts only, 2 = named only
    int optTrail_ = 1;          // 0 = off, 1 = line, 2 = ribbon

    // WHERE EACH CONTACT HAS BEEN, and how high it was at each point, so a
    // trail can be coloured by the altitude the aircraft actually had along it
    // rather than by the altitude it has now. Accumulated from what the face
    // already polls; a trail therefore begins when the scope is opened, not
    // when the aircraft was first heard, which is worth saying rather than
    // hiding.
    struct TrailPoint {
        double lat;
        double lon;
        double altM;
    };
    static constexpr std::size_t kTrailMaxPoints = 256;
    static constexpr std::size_t kTrailMaxTracks = 256;
    std::map<std::string, std::vector<TrailPoint>> trails_;
};

// ============================================================================
// Implementation of the pure half. Header-only for the same reason
// track_metrics.hpp is: every piece of it is a handful of lines with no state
// to hide, and it keeps the arithmetic a test reads in the same file as the
// argument for it.
// ============================================================================

inline const char* scopeUnavailableNote() {
    return "No squawk, route or airline mark: the track interface does not carry them.";
}

inline std::vector<ScopeDetailLine> buildScopeDetailLines(const ScopeDetailInput& in) {
    std::vector<ScopeDetailLine> out;
    out.reserve(10);

    // --- who it is ------------------------------------------------------------
    // Never dimmed and never empty: the caller substitutes the id when there is
    // no callsign, so a row that said nothing would mean the caller was broken
    // rather than that the aircraft was anonymous.
    out.push_back({"FLIGHT", in.flight, true, false});

    // --- what the registry knows ----------------------------------------------
    // THREE STATES, NOT TWO. With no track-info plugin installed these three
    // rows are not "unknown", they are "nothing was asked" - and telling a user
    // their receiver failed to read an operator name it never had a source for
    // would send them debugging the radio. The rows stay present in all three
    // states because their absence would move every row below them as a lookup
    // completed, and a panel that reflows while it is being read is worse than
    // one with a dim row in it.
    const char* registryMiss = in.infoActive ? (in.infoPending ? "LOOKING UP" : "NO DATA")
                                             : "NO REGISTRY PLUGIN";
    const auto registryRow = [&](const char* label, const std::string& value) {
        if (in.infoActive && !in.infoPending && !value.empty()) {
            out.push_back({label, value, true, false});
        } else {
            out.push_back({label, registryMiss, false, false});
        }
    };
    registryRow("OPERATOR", in.operatorName);
    registryRow("TYPE", in.typeName);
    registryRow("REG", in.registration);

    // --- the state a squawk would have carried ---------------------------------
    // WHERE THE SQUAWK WOULD SIT, which is with the identity block and not down
    // among the measurements: on the instrument this pattern comes from, the
    // transponder code is part of who the aircraft is declaring itself to be.
    //
    // AN EMERGENCY OUTRANKS EVERY OTHER STYLING RULE IN THIS FILE - the same
    // rule the map's colour precedence states and for the same reason. It is
    // the one thing on the picture that must never be mistaken for anything
    // else, so it is the one row that carries `alert` and is drawn in a hue
    // nothing else on the scope uses. "NORMAL" is not a claim that a squawk was
    // read: it is the honest reading of a flag that is not set.
    out.push_back({"STATUS", in.emergency ? "EMERGENCY" : "NORMAL", true, in.emergency});

    // --- what the radio heard ---------------------------------------------------
    // FEET AND KNOTS, not metres and metres per second. Every altitude and
    // speed on this panel came from an aviation source that thinks in feet and
    // knots, every controller and every pilot the user might compare a figure
    // against speaks in them, and the ABI's metric units are an internal
    // convention rather than a thing to show anybody.
    if (std::isfinite(in.altM)) {
        out.push_back({"ALTITUDE", detail::scopePrintf("%.0f FT", in.altM * 3.28084), true,
                       false});
    } else {
        out.push_back({"ALTITUDE", "NO DATA", false, false});
    }

    if (std::isfinite(in.speedMps)) {
        out.push_back(
            {"SPEED", detail::scopePrintf("%.0f KT", in.speedMps * 1.94384), true, false});
    } else {
        out.push_back({"SPEED", "NO DATA", false, false});
    }

    if (std::isfinite(in.courseDeg)) {
        out.push_back(
            {"HEADING",
             detail::scopePrintf("%03.0f DEG", detail::scopeWrapBearing(in.courseDeg)), true,
             false});
    } else {
        out.push_back({"HEADING", "NO DATA", false, false});
    }

    // --- where it is, from here -------------------------------------------------
    // WITHOUT A RECEIVER POSITION THESE TWO ARE NOT UNKNOWN, THEY ARE
    // UNMEASURABLE, and the row says which. In practice the scope refuses to
    // draw at all without one (there would be nothing to centre it on), so this
    // branch is a contract the builder keeps rather than a state the panel
    // reaches - but a builder that quietly printed "NO DATA" here would be
    // blaming the aircraft for the host's missing setting.
    if (!in.hasRx) {
        out.push_back({"RANGE", "NO RX POSITION", false, false});
        out.push_back({"BEARING", "NO RX POSITION", false, false});
        return out;
    }

    if (std::isfinite(in.rangeNm)) {
        out.push_back({"RANGE", detail::scopePrintf("%.1f NM", in.rangeNm), true, false});
    } else {
        out.push_back({"RANGE", "NO DATA", false, false});
    }

    // OVERHEAD IS AN ANSWER, NOT A GAP. A bearing is undefined for a target at
    // the receiver's own coordinates (see initialBearingDeg, which returns NaN
    // there rather than the 000 atan2 would hand back), and "OVERHEAD" is the
    // fact - so it is a KNOWN value and is not dimmed. Printing "000 DEG" would
    // be a fabricated direction, and dimming it would call a certainty a doubt.
    if (std::isfinite(in.bearingDeg)) {
        // WRAPPED BEFORE ROUNDING. "%03.0f" rounds 359.6 to 360, and 360 DEG
        // is not a compass bearing - the same direction is 000. Ordinary
        // inputs reach it: a target a little west of due north from the
        // receiver bears 359.65, and an ADS-B heading is quantised in steps
        // that land there routinely.
        out.push_back({"BEARING",
                       detail::scopePrintf("%03.0f DEG",
                                           detail::scopeWrapBearing(in.bearingDeg)),
                       true, false});
    } else if (std::isfinite(in.rangeNm)) {
        out.push_back({"BEARING", "OVERHEAD", true, false});
    } else {
        out.push_back({"BEARING", "NO DATA", false, false});
    }

    return out;
}

}  // namespace cascade::gui

#endif  // CASCADE_GUI_SCOPE_VIEW_HPP
