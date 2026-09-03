// Tests for gui/scope_view.hpp - the pure half of the ADS-B radar scope: the
// ladder of range settings, the range and bearing from the receiver to a
// target, the polar-to-screen mapping every mark on the face goes through, the
// in-range test that decides what is drawn at all, and every string a readout
// or a panel row prints.
//
// WHY THIS IS THE HALF WORTH TESTING. A scope is a picture made entirely of
// ranges and bearings. Its failure modes are an aircraft in the wrong place and
// a field that reads wrong - and neither is observable once the arithmetic has
// gone into an ImDrawList, which is exactly why every decision lives in free
// functions here rather than inside the draw loop. There is no ImGui in this
// file and none is needed.
//
// HOW THE GEOMETRY IS CHECKED, AND WHAT EACH CHECK CAN ACTUALLY CATCH. The
// axis-aligned pairs have analytic answers - one degree of latitude is the
// sphere's radius times one degree in radians, and the bearing due north is
// exactly 0 - so those are asserted against arithmetic worked out here rather
// than against a number somebody remembered, and kDegreeNm spells 1.852 out
// independently. That independent spelling is what pins the unit conversion: a
// scope built on statute miles (1.609) or on the old "1.15 miles" rule of
// thumb would place every target at the wrong radius, and every ring label
// would agree with the error.
//
// The oblique cross-check against greatCircleKm is a DIFFERENT assertion and
// deliberately not that one: multiplying scopeRelative's answer back by the
// same constant is self-consistent by construction, so it proves only that the
// wrapper delegates to the tested geodesy instead of growing a private
// haversine. Both are worth having; neither substitutes for the other, and
// this note exists because a mutation run showed the cross-check staying green
// against a deliberately wrong constant.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cmath>
#include <cstddef>
#include <limits>
#include <cstdio>
#include <string>
#include <vector>

#include "gui/scope_view.hpp"
#include "test_check.hpp"

using cascade::gui::buildScopeDetailLines;
using cascade::gui::clampScopeRangeNm;
using cascade::gui::greatCircleKm;
using cascade::gui::initialBearingDeg;
using cascade::gui::kKmPerNm;
using cascade::gui::kScopeDefaultRangeNm;
using cascade::gui::kScopeRangeCount;
using cascade::gui::kScopeRangesNm;
using cascade::gui::kScopeRingCount;
using cascade::gui::ScopeDetailInput;
using cascade::gui::ScopeDetailLine;
using cascade::gui::ScopePoint;
using cascade::gui::ScopePolar;
using cascade::gui::scopeBearingLabel;
using cascade::gui::scopeInRange;
using cascade::gui::scopeProject;
using cascade::gui::scopeRangeIndex;
using cascade::gui::scopeRangeNmAt;
using cascade::gui::scopeRangeReadout;
using cascade::gui::scopeRangeStepped;
using cascade::gui::scopeRelative;
using cascade::gui::scopeRingLabel;
using cascade::gui::scopeRingNm;
using cascade::gui::scopeTracksReadout;
using cascade::gui::scopeUnavailableNote;
using cascade::gui::ScopeLatLon;
using cascade::gui::scopeViewCentre;

namespace {

const double kNaN = std::numeric_limits<double>::quiet_NaN();

// Bounds-safe row access. CHECK records and CONTINUES, so `lines[i]` guarded
// only by a preceding size check is an out-of-bounds read in exactly the run
// that has something to report - the test would crash instead of naming the
// broken expectation. A default-constructed row fails every assertion below it
// instead.
ScopeDetailLine at(const std::vector<ScopeDetailLine>& v, std::size_t i) {
    return i < v.size() ? v[i] : ScopeDetailLine{};
}

// The whole label list as one comparable value, so the field ORDER is asserted
// in a single container compare rather than by indexing ten times.
std::vector<std::string> labelsOf(const std::vector<ScopeDetailLine>& v) {
    std::vector<std::string> out;
    out.reserve(v.size());
    for (const ScopeDetailLine& l : v) { out.push_back(l.label); }
    return out;
}

// Distance from the scope's centre to a projected point. The property every
// mark on a plan-position indicator must have: the radius IS the range.
double radiusOf(const ScopePoint& p, double cx, double cy) {
    const double dx = p.x - cx;
    const double dy = p.y - cy;
    return std::sqrt(dx * dx + dy * dy);
}

// An identity, and the reason it exists is a compiler warning rather than a
// test: several assertions below legitimately compare a compile-time constant
// against the value the rest of the application assumes, and MSVC calls a
// constant condition C4127. Reading the constant through a function keeps the
// assertion exactly as written, and keeps the warning out of a build that has
// none.
template <typename T>
T asValue(T v) {
    return v;
}

// One degree of arc on the sphere greatCircleKm measures on, in nautical miles.
// Derived rather than remembered, so this is a second expression of the same
// geometry and not a copy of the answer.
const double kDegreeNm = 6371.0 * 3.14159265358979323846 / 180.0 / 1.852;


// THE VIEW CENTRE, AND THE BUG IT EXISTS TO STOP.
//
// Reported as "the planes disappear when you try and zoom on them". The face
// had been made pannable, and the rings, the map and the centre dot all moved
// with the view - but the target cull did not. It still asked "is this
// aircraft within RANGE of the ANTENNA", so after dragging the view somewhere
// and zooming in, a contact sitting in the middle of a 10 NM picture that
// happened to be 50 NM from the aerial failed the test and was never drawn.
// The picture emptied exactly when it was looked at closely.
//
// These checks pin the arithmetic the fix rests on: where the middle of the
// glass is after a drag, and that a contact there is in range whatever its
// distance from the antenna.
void testViewCentreAndTargetCull() {
    std::printf("  the middle of a dragged view, and what stays visible in it\n");

    // A scale of 100000 px per world width: one pixel is 0.0036 degrees of
    // longitude, which keeps the numbers below readable.
    const double ppw = 100000.0;

    // NO PAN IS THE ANTENNA, exactly - not nearly.
    {
        const ScopeLatLon v = scopeViewCentre(51.5, -2.1, 0.0, 0.0, ppw);
        CHECK(v.latDeg == 51.5);
        CHECK(v.lonDeg == -2.1);
    }

    // NO USABLE SCALE FALLS BACK TO THE ANTENNA rather than dividing by zero.
    {
        const ScopeLatLon v = scopeViewCentre(51.5, -2.1, 300.0, 200.0, 0.0);
        CHECK(v.latDeg == 51.5 && v.lonDeg == -2.1);
    }

    // THE SIGNS. Dragging right carries the map right, so the middle of the
    // glass moves WEST; dragging down carries it down, so the middle moves
    // NORTH. Getting either backwards looks plausible in code and is instantly
    // wrong on screen.
    {
        const ScopeLatLon r = scopeViewCentre(51.5, -2.1, 1000.0, 0.0, ppw);
        CHECK(r.lonDeg < -2.1);
        const ScopeLatLon d = scopeViewCentre(51.5, -2.1, 0.0, 1000.0, ppw);
        CHECK(d.latDeg > 51.5);
        const ScopeLatLon u = scopeViewCentre(51.5, -2.1, 0.0, -1000.0, ppw);
        CHECK(u.latDeg < 51.5);
    }

    // THE EXACT OFFSET, so a future change cannot quietly rescale the pan.
    {
        const ScopeLatLon v = scopeViewCentre(0.0, 0.0, 1000.0, 0.0, ppw);
        // 1000 px of a 100000 px world is a hundredth of it: 3.6 degrees.
        CHECK(std::fabs(v.lonDeg + 3.6) < 1e-9);
    }

    // AND THE BUG ITSELF. An aircraft placed at the middle of a view that has
    // been dragged well away from the antenna must be IN range at a short
    // setting, even though it is far outside that range from the aerial.
    {
        const double rxLat = 51.5, rxLon = -2.1;
        // Drag far enough that the view centre is about a degree east.
        const ScopeLatLon v = scopeViewCentre(rxLat, rxLon, -27777.8, 0.0, ppw);
        CHECK(v.lonDeg > rxLon + 0.9);

        // The contact sits exactly at the middle of that view.
        const ScopePolar fromView = scopeRelative(v.latDeg, v.lonDeg, v.latDeg, v.lonDeg);
        const ScopePolar fromRx = scopeRelative(rxLat, rxLon, v.latDeg, v.lonDeg);
        CHECK(fromView.rangeNm < 0.001);
        CHECK(fromRx.rangeNm > 30.0);

        // THIS is the pair of answers that differed before the fix.
        CHECK(scopeInRange(fromView.rangeNm, 10.0));
        CHECK(!scopeInRange(fromRx.rangeNm, 10.0));
    }
}

}  // namespace

int main() {
    // --- the ladder itself ---------------------------------------------------
    // RED WHEN a value is added, removed or reordered without the tests and the
    // config documentation being brought with it. The ladder is not a private
    // detail: config.cpp snaps a hand-edited range onto it, the view derives
    // four ring radii and four labels from whichever entry is selected, and the
    // stepper's two buttons ARE the ladder.
    {
        CHECK(asValue(kScopeRangeCount) == 6);
        const int expect[] = {10, 25, 50, 100, 200, 400};
        for (int i = 0; i < kScopeRangeCount; ++i) {
            CHECK(scopeRangeNmAt(i) == expect[i]);
        }
        // Strictly ascending, which every piece of arithmetic below assumes:
        // scopeRangeIndex's nearest-match scan, the stepper's "the next index
        // is a longer range", and the disabled state of the two buttons.
        for (int i = 1; i < kScopeRangeCount; ++i) {
            CHECK(kScopeRangesNm[i] > kScopeRangesNm[i - 1]);
        }
        // Bounds-safe rather than reading past either end. RED WHEN the clamp
        // in scopeRangeNmAt is dropped - which, in a release build, is an
        // out-of-bounds read that would usually still "pass".
        CHECK(scopeRangeNmAt(-1) == 10);
        CHECK(scopeRangeNmAt(-99999) == 10);
        CHECK(scopeRangeNmAt(kScopeRangeCount) == 400);
        CHECK(scopeRangeNmAt(99999) == 400);
        // The default has to BE on the ladder, or the very first frame after a
        // fresh install would draw a scale the view has no rings for.
        CHECK(asValue(kScopeDefaultRangeNm) == 200);
        CHECK(clampScopeRangeNm(kScopeDefaultRangeNm) == kScopeDefaultRangeNm);
        // Four rings: quarter, half, three-quarter, full.
        CHECK(asValue(kScopeRingCount) == 4);
    }

    // --- snapping a configured range onto the ladder --------------------------
    // This is what a hand-edited config.json meets on the way in. RED WHEN the
    // clamp is dropped from ConfigStore::load or from ScopeView::setRangeNm, in
    // which case a range like 173 reaches the renderer and it draws rings at
    // 43.25, 86.5, 129.75 and 173 NM - a scale nobody chose and nobody can
    // reproduce.
    {
        // Every legal value is its own answer: snapping must never move a
        // range the user actually selected.
        for (int i = 0; i < kScopeRangeCount; ++i) {
            CHECK(clampScopeRangeNm(kScopeRangesNm[i]) == kScopeRangesNm[i]);
        }
        // Below and above the ladder, which is the "clamped at the ends"
        // half of nearest-match.
        CHECK(clampScopeRangeNm(0) == 10);
        CHECK(clampScopeRangeNm(1) == 10);
        CHECK(clampScopeRangeNm(-5) == 10);
        CHECK(clampScopeRangeNm(999) == 400);
        CHECK(clampScopeRangeNm(100000) == 400);
        // NEAREST, not "next one up" and not "back to the default". A user who
        // typed 173 was reaching for the 200 NM scale, and a reset to the
        // default would throw that away in the one case it happens to agree.
        CHECK(clampScopeRangeNm(173) == 200);
        CHECK(clampScopeRangeNm(12) == 10);
        CHECK(clampScopeRangeNm(60) == 50);
        CHECK(clampScopeRangeNm(90) == 100);
        // The midpoints, either side. 17.5 is halfway between 10 and 25, so 17
        // belongs to 10 and 18 to 25 - the pair that proves the scan compares
        // distances rather than testing "less than the next entry".
        CHECK(clampScopeRangeNm(17) == 10);
        CHECK(clampScopeRangeNm(18) == 25);
        CHECK(clampScopeRangeNm(37) == 25);
        CHECK(clampScopeRangeNm(38) == 50);
        // EXACT TIES GO TO THE SMALLER RANGE, which is the documented rule: a
        // scope set tighter than an ambiguous instruction still draws
        // everything inside it correctly and says its own scale in the corner,
        // where one set longer quietly claims reach nobody asked for. RED WHEN
        // the scan uses <= instead of <.
        CHECK(clampScopeRangeNm(75) == 50);
        CHECK(clampScopeRangeNm(150) == 100);
        CHECK(clampScopeRangeNm(300) == 200);
        // A hand-edit big enough to overflow int arithmetic. `nm - 10` in int
        // is undefined behaviour near the bottom of the range, which is why the
        // scan works in long long. RED WHEN it goes back to int - and on this
        // compiler that failure is a wrong answer rather than a crash, so the
        // assertion is the only thing that would catch it.
        CHECK(clampScopeRangeNm(2000000000) == 400);
        CHECK(clampScopeRangeNm(-2000000000) == 10);
        CHECK(scopeRangeIndex(200) == 4);
        CHECK(scopeRangeIndex(-2000000000) == 0);
    }

    // --- stepping the ladder --------------------------------------------------
    // The mouse wheel over the face and the two buttons on the bar both come
    // through here.
    {
        // One notch at a time, up the whole ladder and back down it.
        for (int i = 0; i + 1 < kScopeRangeCount; ++i) {
            CHECK(scopeRangeStepped(kScopeRangesNm[i], +1) == kScopeRangesNm[i + 1]);
            CHECK(scopeRangeStepped(kScopeRangesNm[i + 1], -1) == kScopeRangesNm[i]);
        }
        // CLAMPED AT BOTH ENDS, NEVER WRAPPED. RED WHEN someone makes the step
        // cycle round: a wheel that turns the 400 NM scale into the 10 NM scale
        // on one accidental notch throws the whole picture away and leaves the
        // user with no idea what happened.
        CHECK(scopeRangeStepped(10, -1) == 10);
        CHECK(scopeRangeStepped(10, -1000) == 10);
        CHECK(scopeRangeStepped(400, +1) == 400);
        CHECK(scopeRangeStepped(400, +1000) == 400);
        // A step of nothing moves nothing.
        for (int i = 0; i < kScopeRangeCount; ++i) {
            CHECK(scopeRangeStepped(kScopeRangesNm[i], 0) == kScopeRangesNm[i]);
        }
        // Several notches at once, which is what a fast wheel produces.
        CHECK(scopeRangeStepped(10, +5) == 400);
        CHECK(scopeRangeStepped(400, -5) == 10);
        CHECK(scopeRangeStepped(50, +2) == 200);
        // A CURRENT VALUE OFF THE LADDER still steps somewhere sensible,
        // because the step snaps before it counts. Without that, a config-edited
        // 173 would step from an index nothing defines.
        CHECK(scopeRangeStepped(173, 0) == 200);
        CHECK(scopeRangeStepped(173, +1) == 400);
        CHECK(scopeRangeStepped(173, -1) == 100);
        // ...including one that would overflow the index arithmetic.
        CHECK(scopeRangeStepped(200, 2000000000) == 400);
        CHECK(scopeRangeStepped(200, -2000000000) == 10);
    }

    // --- range and bearing from the receiver ----------------------------------
    {
        // DUE NORTH, one degree. The bearing is exactly 0 and the range is the
        // sphere's radius times one degree in radians, converted at 1852 m to
        // the nautical mile - a defined quantity since 1929, so this needs no
        // tolerance beyond floating point.
        const ScopePolar north = scopeRelative(51.5, -0.5, 52.5, -0.5);
        CHECK_NEAR(north.rangeNm, kDegreeNm, 1e-9);
        CHECK_NEAR(north.bearingDeg, 0.0, 1e-9);

        // DUE SOUTH, DUE EAST, DUE WEST - the other three axis-aligned cases,
        // which is what catches a sign or a swapped argument in the wrapper.
        const ScopePolar south = scopeRelative(51.5, -0.5, 50.5, -0.5);
        CHECK_NEAR(south.rangeNm, kDegreeNm, 1e-9);
        CHECK_NEAR(south.bearingDeg, 180.0, 1e-9);
        const ScopePolar east = scopeRelative(0.0, 0.0, 0.0, 1.0);
        CHECK_NEAR(east.rangeNm, kDegreeNm, 1e-9);
        CHECK_NEAR(east.bearingDeg, 90.0, 1e-9);
        const ScopePolar west = scopeRelative(0.0, 0.0, 0.0, -1.0);
        CHECK_NEAR(west.rangeNm, kDegreeNm, 1e-9);
        CHECK_NEAR(west.bearingDeg, 270.0, 1e-9);

        // THE ANTIMERIDIAN. A receiver at 179.9 E and an aircraft at 179.9 W
        // are two tenths of a degree apart, not three hundred and fifty-nine
        // and eight tenths - and a scope that believed the second would refuse
        // to draw an aircraft twelve miles away on every range setting it has.
        // RED WHEN the conversion is applied to a naive longitude difference
        // rather than to greatCircleKm's answer.
        const ScopePolar seam = scopeRelative(0.0, 179.9, 0.0, -179.9);
        CHECK_NEAR(seam.rangeNm, 0.2 * kDegreeNm, 1e-9);
        CHECK_NEAR(seam.bearingDeg, 90.0, 1e-9);
        // ...and the same pair the other way round is due west, not a different
        // distance.
        const ScopePolar seamBack = scopeRelative(0.0, -179.9, 0.0, 179.9);
        CHECK_NEAR(seamBack.rangeNm, 0.2 * kDegreeNm, 1e-9);
        CHECK_NEAR(seamBack.bearingDeg, 270.0, 1e-9);

        // DIRECTLY OVERHEAD. The range is zero and THERE IS NO BEARING: an
        // aircraft at the receiver's own coordinates has no direction from it,
        // and "000" would be read as due north. scopeRelative returns NaN,
        // which is what the panel turns into the word OVERHEAD and what
        // scopeProject turns into the centre of the face. RED WHEN the NaN is
        // replaced by a zero anywhere along that path.
        const ScopePolar over = scopeRelative(51.5, -0.5, 51.5, -0.5);
        CHECK(over.rangeNm == 0.0);
        CHECK(std::isnan(over.bearingDeg));

        // THE WRAPPER DELEGATES, and this is the assertion that says so: an
        // oblique pair with no closed form, checked against the tested geodesy
        // rather than against a remembered number. RED WHEN scopeRelative grows
        // a private haversine or a flat-earth shortcut of its own.
        //
        // It CANNOT catch a wrong nautical mile - multiplying the answer back
        // by the same constant is self-consistent whatever that constant is -
        // so the conversion is pinned separately, by the analytic cases above
        // (which spell 1.852 out independently in kDegreeNm) and by the plain
        // comparison at the end of this block.
        const double oblique = greatCircleKm(-33.87, 151.21, -37.81, 144.96);
        const ScopePolar obliquePolar = scopeRelative(-33.87, 151.21, -37.81, 144.96);
        CHECK_NEAR(obliquePolar.rangeNm * kKmPerNm, oblique, 1e-9);
        CHECK_NEAR(obliquePolar.bearingDeg,
                   initialBearingDeg(-33.87, 151.21, -37.81, 144.96), 1e-12);
        CHECK(asValue(kKmPerNm) == 1.852);
    }

    // --- what is on the scope at all ------------------------------------------
    {
        CHECK(scopeInRange(0.0, 200.0));
        CHECK(scopeInRange(199.9, 200.0));
        // THE OUTERMOST RING COUNTS AS INSIDE. The scope draws a ring at full
        // scale, and a ring nothing is allowed to sit on is a boundary that
        // lies about itself. RED WHEN the comparison becomes strict.
        CHECK(scopeInRange(200.0, 200.0));
        CHECK(!scopeInRange(200.1, 200.0));
        CHECK(!scopeInRange(400.0, 200.0));
        // A NaN range is a position that made no sense, and it must fall out as
        // "not on this scope" rather than being compared - a NaN passed through
        // to scopeProject scatters NaN vertices through the draw list. RED WHEN
        // the test is written as the negation of an out-of-range test, which
        // accepts NaN.
        CHECK(!scopeInRange(kNaN, 200.0));
        CHECK(!scopeInRange(-1.0, 200.0));
        CHECK(!scopeInRange(50.0, kNaN));
        CHECK(!scopeInRange(50.0, 0.0));
        CHECK(!scopeInRange(50.0, -200.0));
    }

    // --- the polar-to-screen mapping ------------------------------------------
    // North is up, bearing runs clockwise, and screen y grows DOWNWARDS - which
    // is the one thing a plan-position indicator gets wrong by a sign and then
    // draws every aircraft mirrored through the horizontal.
    {
        const double cx = 500.0;
        const double cy = 400.0;
        const double rad = 200.0;
        const double full = 200.0;

        const ScopePoint n = scopeProject(cx, cy, rad, 100.0, 0.0, full);
        CHECK_NEAR(n.x, 500.0, 1e-9);
        CHECK_NEAR(n.y, 300.0, 1e-9);  // half scale, straight UP
        const ScopePoint e = scopeProject(cx, cy, rad, 100.0, 90.0, full);
        CHECK_NEAR(e.x, 600.0, 1e-9);
        CHECK_NEAR(e.y, 400.0, 1e-9);
        const ScopePoint s = scopeProject(cx, cy, rad, 100.0, 180.0, full);
        CHECK_NEAR(s.x, 500.0, 1e-9);
        CHECK_NEAR(s.y, 500.0, 1e-9);
        const ScopePoint w = scopeProject(cx, cy, rad, 100.0, 270.0, full);
        CHECK_NEAR(w.x, 400.0, 1e-9);
        CHECK_NEAR(w.y, 400.0, 1e-9);

        // A target at full scale lands ON the rim, and one at a quarter scale a
        // quarter of the way out. RED WHEN the radius stops being linear in
        // range - the ring labels are computed the same way, so a scope with a
        // non-linear radius would have rings that disagreed with the marks
        // between them.
        for (int b = 0; b < 360; b += 15) {
            const ScopePoint p =
                scopeProject(cx, cy, rad, full, static_cast<double>(b), full);
            CHECK_NEAR(radiusOf(p, cx, cy), rad, 1e-9);
            const ScopePoint q =
                scopeProject(cx, cy, rad, full * 0.25, static_cast<double>(b), full);
            CHECK_NEAR(radiusOf(q, cx, cy), rad * 0.25, 1e-9);
        }

        // The intercardinal, worked out here rather than remembered: it is what
        // catches sine and cosine being swapped, which the four axis-aligned
        // cases above cannot see on their own.
        const ScopePoint ne = scopeProject(cx, cy, rad, full, 45.0, full);
        CHECK_NEAR(ne.x, cx + rad * std::sin(45.0 * 3.14159265358979323846 / 180.0), 1e-9);
        CHECK_NEAR(ne.y, cy - rad * std::cos(45.0 * 3.14159265358979323846 / 180.0), 1e-9);
        CHECK(ne.x > cx);   // north-east is right...
        CHECK(ne.y < cy);   // ...and up

        // EVERY DEGENERATE INPUT LANDS AT THE CENTRE. Zero range IS the centre;
        // a NaN bearing is the aircraft on top of the receiver, which is also
        // the centre and the one case where a bearing genuinely does not exist;
        // and a NaN range or a zero scale cannot be drawn at all. RED WHEN any
        // of them is allowed to reach the arithmetic - the result is a NaN
        // vertex, and one of those takes the whole draw list with it.
        const ScopePoint zero = scopeProject(cx, cy, rad, 0.0, 123.0, full);
        CHECK(zero.x == cx && zero.y == cy);
        const ScopePoint overhead = scopeProject(cx, cy, rad, 0.0, kNaN, full);
        CHECK(overhead.x == cx && overhead.y == cy);
        const ScopePoint nanRange = scopeProject(cx, cy, rad, kNaN, 90.0, full);
        CHECK(nanRange.x == cx && nanRange.y == cy);
        const ScopePoint noScale = scopeProject(cx, cy, rad, 50.0, 90.0, 0.0);
        CHECK(noScale.x == cx && noScale.y == cy);
        const ScopePoint noRadius = scopeProject(cx, cy, 0.0, 50.0, 90.0, full);
        CHECK(noRadius.x == cx && noRadius.y == cy);

        // A bearing past a full turn is still a direction; the sine and cosine
        // are periodic, so this must agree with its folded equivalent rather
        // than being rejected.
        const ScopePoint wrapped = scopeProject(cx, cy, rad, 100.0, 450.0, full);
        CHECK_NEAR(wrapped.x, e.x, 1e-9);
        CHECK_NEAR(wrapped.y, e.y, 1e-9);
    }

    // --- the rings and their labels -------------------------------------------
    {
        CHECK(scopeRingNm(200, 1) == 50.0);
        CHECK(scopeRingNm(200, 2) == 100.0);
        CHECK(scopeRingNm(200, 3) == 150.0);
        CHECK(scopeRingNm(200, 4) == 200.0);
        // THE SHORT END OF THE LADDER DOES NOT DIVIDE INTO WHOLE NUMBERS, which
        // is the case an integer ring radius would silently round: the 10 NM
        // scope's rings are at 2.5, 5, 7.5 and 10, and "2 NM" beside a ring at
        // two and a half is a wrong label rather than a rounded one. RED WHEN
        // scopeRingNm returns an int or the label uses %.0f.
        CHECK(scopeRingNm(10, 1) == 2.5);
        CHECK(scopeRingLabel(10, 1) == "2.5 NM");
        CHECK(scopeRingLabel(10, 3) == "7.5 NM");
        CHECK(scopeRingLabel(200, 1) == "50 NM");
        CHECK(scopeRingLabel(200, 4) == "200 NM");
        CHECK(scopeRingLabel(400, 3) == "300 NM");
        CHECK(scopeRingLabel(25, 1) == "6.25 NM");
        // Bounds-safe: a ring index outside the set is clamped, so a loop that
        // is off by one draws a duplicate ring instead of a radius outside the
        // face (or, at the other end, inside the receiver marker).
        CHECK(scopeRingNm(200, 0) == scopeRingNm(200, 1));
        CHECK(scopeRingNm(200, -7) == scopeRingNm(200, 1));
        CHECK(scopeRingNm(200, 5) == scopeRingNm(200, kScopeRingCount));
        CHECK(scopeRingNm(200, 9999) == scopeRingNm(200, kScopeRingCount));
        // The outermost ring is the scale itself, which is what lets the corner
        // readout and the rim label be read as one statement.
        for (int i = 0; i < kScopeRangeCount; ++i) {
            CHECK(scopeRingNm(kScopeRangesNm[i], kScopeRingCount) ==
                  static_cast<double>(kScopeRangesNm[i]));
        }
    }

    // --- the corner readouts and the bearing ticks ----------------------------
    {
        CHECK(scopeTracksReadout(0) == "0 TRACKS");
        CHECK(scopeTracksReadout(12) == "12 TRACKS");
        // IT DOES NOT CONJUGATE. The readout is a legend on an instrument face,
        // a fixed field whose width must not change as the last aircraft
        // leaves. Stated as a test because it looks like a bug otherwise, and
        // the next reader deserves to find the decision rather than "fix" it.
        CHECK(scopeTracksReadout(1) == "1 TRACKS");
        // A negative count cannot come out of the draw loop, and a readout is
        // the wrong place to discover one.
        CHECK(scopeTracksReadout(-4) == "0 TRACKS");

        CHECK(scopeRangeReadout(10) == "10 NM");
        CHECK(scopeRangeReadout(200) == "200 NM");
        CHECK(scopeRangeReadout(400) == "400 NM");

        // THE CARDINALS ARE LETTERS AND EVERYTHING ELSE IS THREE DIGITS. That
        // is the compass-rose convention and the reason a scope can be read
        // without counting round from the top. Three digits because a bearing
        // is always written as three, and a bare "30" on a face covered in
        // distances reads as one of them.
        CHECK(scopeBearingLabel(0) == "N");
        CHECK(scopeBearingLabel(90) == "E");
        CHECK(scopeBearingLabel(180) == "S");
        CHECK(scopeBearingLabel(270) == "W");
        CHECK(scopeBearingLabel(30) == "030");
        CHECK(scopeBearingLabel(60) == "060");
        CHECK(scopeBearingLabel(120) == "120");
        CHECK(scopeBearingLabel(330) == "330");
        // Folded rather than printed: 360 is north and -30 is 330. A tick loop
        // that ran past a full turn would otherwise label a direction that does
        // not exist.
        CHECK(scopeBearingLabel(360) == "N");
        CHECK(scopeBearingLabel(720) == "N");
        CHECK(scopeBearingLabel(-30) == "330");
        CHECK(scopeBearingLabel(-90) == "W");
    }

    // --- the detail panel's field list ----------------------------------------
    {
        // The field list the request fixed, in order. RED WHEN a row is added,
        // removed or moved - the panel is read top to bottom and its order is
        // part of what it says.
        const std::vector<std::string> expected = {"FLIGHT",  "OPERATOR", "TYPE",
                                                   "REG",     "STATUS",   "ALTITUDE",
                                                   "SPEED",   "HEADING",  "RANGE",
                                                   "BEARING"};

        ScopeDetailInput in;
        in.flight = "BAW123";
        in.operatorName = "British Airways";
        in.typeName = "Airbus A320";
        in.registration = "G-EUUU";
        in.infoActive = true;
        in.altM = 11277.6;    // 37 000 ft
        in.speedMps = 231.5;  // 450 kt
        in.courseDeg = 37.0;
        in.hasRx = true;
        in.rangeNm = 42.5;
        in.bearingDeg = 273.4;

        std::vector<ScopeDetailLine> lines = buildScopeDetailLines(in);
        CHECK(labelsOf(lines) == expected);
        CHECK(at(lines, 0).value == "BAW123");
        CHECK(at(lines, 0).known);
        CHECK(at(lines, 1).value == "British Airways");
        CHECK(at(lines, 2).value == "Airbus A320");
        CHECK(at(lines, 3).value == "G-EUUU");
        CHECK(at(lines, 4).value == "NORMAL");
        CHECK(!at(lines, 4).alert);
        // FEET AND KNOTS, not the ABI's metres and metres per second. Every
        // figure on this panel is compared against something a controller or a
        // pilot said, and both of them speak in feet and knots. RED WHEN a
        // conversion factor is dropped or inverted.
        CHECK(at(lines, 5).value == "37000 FT");
        CHECK(at(lines, 6).value == "450 KT");
        // THREE DIGITS for a heading, for the same reason a bearing tick has
        // three: "37 DEG" and "037 DEG" are the same number and only one of
        // them reads as a direction.
        CHECK(at(lines, 7).value == "037 DEG");
        CHECK(at(lines, 8).value == "42.5 NM");
        CHECK(at(lines, 9).value == "273 DEG");
        for (const ScopeDetailLine& l : lines) { CHECK(l.known); }

        // AN EMERGENCY OUTRANKS EVERY OTHER STYLING RULE, which is the same
        // precedence the map's colour rule states: it is the one thing on the
        // picture that must never be mistaken for anything else. RED WHEN the
        // alert flag stops being set, which would leave a 7700 squawk drawn in
        // the same colour as the aircraft beside it.
        in.emergency = true;
        lines = buildScopeDetailLines(in);
        CHECK(labelsOf(lines) == expected);  // the row set does not change
        CHECK(at(lines, 4).value == "EMERGENCY");
        CHECK(at(lines, 4).alert);
        CHECK(at(lines, 4).known);
        in.emergency = false;

        // NO TRACK-INFO PLUGIN IS NOT "UNKNOWN". Nothing was ever asked, and
        // telling a user their receiver failed to read an operator name it had
        // no source for would send them debugging the radio. RED WHEN the three
        // registry states collapse into two.
        in.infoActive = false;
        lines = buildScopeDetailLines(in);
        CHECK(labelsOf(lines) == expected);
        CHECK(at(lines, 1).value == "NO REGISTRY PLUGIN");
        CHECK(!at(lines, 1).known);
        CHECK(at(lines, 2).value == "NO REGISTRY PLUGIN");
        CHECK(at(lines, 3).value == "NO REGISTRY PLUGIN");
        // The flight and everything the radio heard are untouched by the
        // registry's absence - they came off the air, not out of a database.
        CHECK(at(lines, 0).value == "BAW123");
        CHECK(at(lines, 5).value == "37000 FT");

        // ASKED, NOTHING BACK YET. Distinct from both of the above: the lookup
        // is in flight, and the row will fill in on its own.
        in.infoActive = true;
        in.infoPending = true;
        lines = buildScopeDetailLines(in);
        CHECK(at(lines, 1).value == "LOOKING UP");
        CHECK(!at(lines, 1).known);
        CHECK(at(lines, 3).value == "LOOKING UP");

        // ANSWERED, AND THE ANSWER WAS "NOT IN MY DATA" - the plugin is
        // installed, it replied, and it has no entry for this aircraft.
        in.infoPending = false;
        in.operatorName.clear();
        in.typeName.clear();
        in.registration.clear();
        lines = buildScopeDetailLines(in);
        CHECK(at(lines, 1).value == "NO DATA");
        CHECK(!at(lines, 1).known);
        CHECK(at(lines, 2).value == "NO DATA");
        CHECK(at(lines, 3).value == "NO DATA");

        // A SOURCE THAT REPORTS NO ALTITUDE, SPEED OR COURSE. NaN by ABI
        // contract, and "NO DATA" dimmed rather than a zero: "0 KT" and
        // "nothing reported it" are different facts about the aircraft and must
        // not look the same. RED WHEN a NaN is formatted instead of tested,
        // which prints "nan FT".
        ScopeDetailInput quiet;
        quiet.flight = "4CA2D1";
        quiet.hasRx = true;
        quiet.rangeNm = 12.0;
        quiet.bearingDeg = 5.0;
        lines = buildScopeDetailLines(quiet);
        CHECK(labelsOf(lines) == expected);
        CHECK(at(lines, 5).value == "NO DATA");
        CHECK(!at(lines, 5).known);
        CHECK(at(lines, 6).value == "NO DATA");
        CHECK(!at(lines, 6).known);
        CHECK(at(lines, 7).value == "NO DATA");
        CHECK(!at(lines, 7).known);
        // The heading pads to three digits at the low end too.
        CHECK(at(lines, 9).value == "005 DEG");

        // DIRECTLY OVERHEAD IS AN ANSWER, NOT A GAP. There is no bearing to an
        // aircraft at the receiver's own coordinates, and OVERHEAD is the fact -
        // so it is a KNOWN value and is not dimmed. RED WHEN a NaN bearing is
        // printed (a fabricated "000 DEG", which reads as due north) or dimmed
        // (which calls a certainty a doubt).
        ScopeDetailInput above;
        above.flight = "OVERHEAD1";
        above.hasRx = true;
        above.rangeNm = 0.0;
        above.bearingDeg = kNaN;
        lines = buildScopeDetailLines(above);
        CHECK(at(lines, 8).value == "0.0 NM");
        CHECK(at(lines, 8).known);
        CHECK(at(lines, 9).value == "OVERHEAD");
        CHECK(at(lines, 9).known);

        // NO RECEIVER POSITION IS NOT "UNKNOWN" EITHER: range and bearing are
        // measured FROM somewhere, and without that somewhere they are not
        // unmeasured, they are unmeasurable. The scope refuses to draw at all
        // in this state, so this is a contract the builder keeps rather than a
        // state the panel reaches - but a builder that printed "NO DATA" here
        // would be blaming the aircraft for the host's missing setting.
        ScopeDetailInput noRx;
        noRx.flight = "NORX1";
        lines = buildScopeDetailLines(noRx);
        CHECK(labelsOf(lines) == expected);
        CHECK(at(lines, 8).value == "NO RX POSITION");
        CHECK(!at(lines, 8).known);
        CHECK(at(lines, 9).value == "NO RX POSITION");
        CHECK(!at(lines, 9).known);

        // A POSITION THAT MADE NO SENSE, with a receiver position present. This
        // is the third state again: the host knows where it is, and the range
        // still could not be computed.
        ScopeDetailInput bad;
        bad.flight = "BAD1";
        bad.hasRx = true;
        bad.rangeNm = kNaN;
        bad.bearingDeg = kNaN;
        lines = buildScopeDetailLines(bad);
        CHECK(at(lines, 8).value == "NO DATA");
        CHECK(!at(lines, 8).known);
        CHECK(at(lines, 9).value == "NO DATA");
        CHECK(!at(lines, 9).known);
    }

    // --- what the scope cannot show, and why ----------------------------------
    // ONE SHORT LINE at the foot of the panel. It exists because three fields
    // on the photographed instrument - the squawk, the route and the airline's
    // mark - are not available to this product, and leaving their rows blank
    // would read as "we tried to decode these and failed", which is a claim
    // about the receiver that is not true. RED WHEN the note is emptied or
    // grows into a paragraph the panel has no room for.
    {
        const std::string note = scopeUnavailableNote();
        CHECK(!note.empty());
        CHECK(note.find("squawk") != std::string::npos);
        CHECK(note.find("route") != std::string::npos);
        // One line: the panel reserves a fixed number of them at its foot, and
        // an embedded newline would push the last of it out of the window.
        CHECK(note.find('\n') == std::string::npos);
        CHECK(note.size() < 120u);
    }

    // --- a compass value is never 360 --------------------------------------
    // "%03.0f" rounds 359.6 UP, and the panel then printed "360 DEG" for a
    // direction whose name is 000. Reachable from ordinary geometry - a target
    // slightly west of due north bears 359.65 - and from ADS-B's quantised
    // heading steps, so it is everyday output rather than an edge case. Both
    // the BEARING and HEADING rows go through the same wrapper.
    //
    // RED WHEN: the wrap is removed, or applied after the rounding.
    {
        const double cases[] = {359.5, 359.6, 359.9, 359.999, 360.0, 0.0, 0.4};
        for (double d : cases) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%03.0f", cascade::gui::detail::scopeWrapBearing(d));
            CHECK(std::string(buf) != std::string("360"));
        }
        // It returns the value to PRINT, so it is rounded: 359.4 is 359, not
        // 359.4. What matters is that the middle of the range is untouched in
        // direction and that nothing lands on 360.
        CHECK(cascade::gui::detail::scopeWrapBearing(180.0) == 180.0);
        CHECK(cascade::gui::detail::scopeWrapBearing(359.4) == 359.0);
        CHECK(cascade::gui::detail::scopeWrapBearing(359.6) == 0.0);
        CHECK(cascade::gui::detail::scopeWrapBearing(0.4) == 0.0);
        // a non-finite value passes straight through, for the OVERHEAD path
        CHECK(!std::isfinite(
            cascade::gui::detail::scopeWrapBearing(std::numeric_limits<double>::quiet_NaN())));
    }

    testViewCentreAndTargetCull();

    return testSummary("test_scope_view");
}
