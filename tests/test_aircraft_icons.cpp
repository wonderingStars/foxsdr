// Tests for gui/aircraft_icons.hpp - the shaded aircraft icons (0.99.25).
//
// Drawing needs a GL context and is exercised by the rendered check, not here.
// What CAN be wrong quietly is everything that decides: which icon a category
// draws, that the pictures line up with the names, how big, which way round,
// and the pixel work that makes them look right when small. Each is pinned.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/aircraft_icons.hpp"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "gui/aircraft_icon_pixels.hpp"
#include "test_check.hpp"

using cascade::gui::AircraftIcon;
using cascade::gui::aircraftIconForCategory;
using cascade::gui::aircraftIconQuad;
using cascade::gui::clampAircraftIconPx;
using cascade::gui::halveRgba;
using cascade::gui::IconQuad;
using cascade::gui::kAircraftIconCount;
using cascade::gui::shadowFromAlpha;
namespace px = cascade::gui::aircraft_pixels;

namespace {

bool near(float a, float b) { return std::fabs(a - b) < 1e-3f; }

std::vector<std::uint8_t> pixelsOf(AircraftIcon icon) {
    const std::uint8_t* p = px::kRgba[static_cast<int>(icon)];
    return std::vector<std::uint8_t>(p, p + static_cast<std::size_t>(px::kSide) * px::kSide * 4u);
}

// The icon as it reaches the screen at `side` pixels: halved down the same
// mip chain the application uploads.
std::vector<std::uint8_t> atSize(AircraftIcon icon, int side) {
    std::vector<std::uint8_t> v = pixelsOf(icon);
    int s = px::kSide;
    while (s > side) {
        v = halveRgba(v.data(), s);
        s /= 2;
    }
    return v;
}

// Fraction of the pixels covered by either icon that only ONE of them covers
// - 0 for identical outlines, 1 for outlines that never overlap.
double outlineDifference(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b) {
    int either = 0, onlyOne = 0;
    for (std::size_t i = 3; i < a.size(); i += 4) {
        const bool ia = a[i] >= 128, ib = b[i] >= 128;
        if (ia || ib) { ++either; }
        if (ia != ib) { ++onlyOne; }
    }
    return either == 0 ? 0.0 : static_cast<double>(onlyOne) / either;
}

}  // namespace

int main() {
    // --- the pictures and the names are in the same order -------------------
    //
    // The generator writes the icons in its own list's order and the enum
    // indexes them. RED WHEN either list is reordered alone: the glider would
    // draw the helicopter's pixels and every other check here would still pass.
    CHECK(px::kCount == kAircraftIconCount);
    for (int i = 0; i < kAircraftIconCount && i < px::kCount; ++i) {
        if (std::strcmp(px::kNames[i], cascade::gui::kAircraftIconNames[i]) != 0) {
            std::printf("  (icon %d: pixels are '%s', enum says '%s')\n", i, px::kNames[i],
                        cascade::gui::kAircraftIconNames[i]);
        }
        CHECK(std::strcmp(px::kNames[i], cascade::gui::kAircraftIconNames[i]) == 0);
    }
    CHECK(px::kSide == 128);

    // Every icon has a picture in it, and it sits inside its square with a
    // clear border, so the shadow blur and the rotation never clip an edge.
    for (int i = 0; i < kAircraftIconCount; ++i) {
        const std::uint8_t* p = px::kRgba[i];
        int visible = 0, onBorder = 0;
        for (int y = 0; y < px::kSide; ++y) {
            for (int x = 0; x < px::kSide; ++x) {
                const bool on = p[(static_cast<std::size_t>(y) * px::kSide + x) * 4u + 3u] > 0;
                if (on) { ++visible; }
                if (on && (x == 0 || y == 0 || x == px::kSide - 1 || y == px::kSide - 1)) {
                    ++onBorder;
                }
            }
        }
        if (visible < 1000 || onBorder != 0) {
            std::printf("  (%s: %d visible, %d on the border)\n", px::kNames[i], visible, onBorder);
        }
        CHECK(visible >= 1000);
        CHECK(onBorder == 0);
    }

    // --- which icon each broadcast category draws ---------------------------
    CHECK(aircraftIconForCategory(CASCADE_AIRCRAFT_LIGHT) == AircraftIcon::Light);
    CHECK(aircraftIconForCategory(CASCADE_AIRCRAFT_MEDIUM1) == AircraftIcon::Airliner);
    CHECK(aircraftIconForCategory(CASCADE_AIRCRAFT_MEDIUM2) == AircraftIcon::Airliner);
    CHECK(aircraftIconForCategory(CASCADE_AIRCRAFT_HIGH_VORTEX) == AircraftIcon::Airliner);
    CHECK(aircraftIconForCategory(CASCADE_AIRCRAFT_HEAVY) == AircraftIcon::Heavy);
    CHECK(aircraftIconForCategory(CASCADE_AIRCRAFT_ROTORCRAFT) == AircraftIcon::Helicopter);
    CHECK(aircraftIconForCategory(CASCADE_AIRCRAFT_GLIDER) == AircraftIcon::Glider);
    CHECK(aircraftIconForCategory(CASCADE_AIRCRAFT_ULTRALIGHT) == AircraftIcon::Microlight);
    CHECK(aircraftIconForCategory(CASCADE_AIRCRAFT_UAV) == AircraftIcon::Uav);
    CHECK(aircraftIconForCategory(CASCADE_AIRCRAFT_LIGHTER_THAN_AIR) == AircraftIcon::Balloon);
    CHECK(aircraftIconForCategory(CASCADE_AIRCRAFT_PARACHUTIST) == AircraftIcon::Balloon);
    CHECK(aircraftIconForCategory(CASCADE_AIRCRAFT_SURFACE_EMERGENCY) == AircraftIcon::Surface);
    CHECK(aircraftIconForCategory(CASCADE_AIRCRAFT_SURFACE_SERVICE) == AircraftIcon::Surface);
    CHECK(aircraftIconForCategory(CASCADE_AIRCRAFT_GROUND_OBSTRUCTION) == AircraftIcon::Surface);

    // THE TESTER'S REPORT: an aircraft that broadcast no type drew as an
    // airliner, so an untyped light aeroplane looked like a jet. It must be
    // its own icon now. RED WHEN "none" goes back to the airliner.
    CHECK(aircraftIconForCategory(CASCADE_AIRCRAFT_NONE) == AircraftIcon::Unknown);
    CHECK(aircraftIconForCategory(CASCADE_AIRCRAFT_NONE) != AircraftIcon::Airliner);
    // A category from a newer plugin, or garbage, is "no type" - never a guess.
    CHECK(aircraftIconForCategory(CASCADE_AIRCRAFT_CATEGORY_COUNT) == AircraftIcon::Unknown);
    CHECK(aircraftIconForCategory(999u) == AircraftIcon::Unknown);

    // --- the light helicopter, from the registry type (0.99.26) -----------
    //
    // Every designator on the verified list (Doc 8643 description H1P/H1T,
    // checked on doc8643.com 2026-09-23) makes a rotorcraft the light one.
    {
        const char* single[] = {"R22", "R44", "R66", "B06", "B407", "B505", "AS50", "EC20",
                                "EC30", "H500", "H269", "EN28", "EN48", "G2CA", "A119", "B47G"};
        for (const char* t : single) {
            if (!cascade::gui::isSingleEngineHelicopterType(t)) {
                std::printf("  (%s not recognised as single-engine)\n", t);
            }
            CHECK(cascade::gui::isSingleEngineHelicopterType(t));
            CHECK(cascade::gui::aircraftIconFor(CASCADE_AIRCRAFT_ROTORCRAFT, t) ==
                  AircraftIcon::HelicopterLight);
        }
        // The twins checked as H2T on the same site stay the five-blade
        // helicopter. RED WHEN the list grows a twin by mistake.
        const char* twins[] = {"H60", "EC35", "EC45", "A109", "S76", "B429", "AS55"};
        for (const char* t : twins) {
            CHECK(!cascade::gui::isSingleEngineHelicopterType(t));
            CHECK(cascade::gui::aircraftIconFor(CASCADE_AIRCRAFT_ROTORCRAFT, t) ==
                  AircraftIcon::Helicopter);
        }
        // A registry answering in lower case or padded is the same aircraft.
        CHECK(cascade::gui::isSingleEngineHelicopterType("r44"));
        CHECK(cascade::gui::isSingleEngineHelicopterType(" R44 "));
        // Nothing known, nothing changed.
        CHECK(!cascade::gui::isSingleEngineHelicopterType(nullptr));
        CHECK(!cascade::gui::isSingleEngineHelicopterType(""));
        CHECK(!cascade::gui::isSingleEngineHelicopterType("R44XXXXXXXXXXXX"));
        CHECK(cascade::gui::aircraftIconFor(CASCADE_AIRCRAFT_ROTORCRAFT, nullptr) ==
              AircraftIcon::Helicopter);
        // ONLY A ROTORCRAFT. The type decides which helicopter, never whether
        // it is one: a light aeroplane whose registry entry is somehow "R44"
        // keeps the category it broadcast. RED WHEN the type overrides it.
        CHECK(cascade::gui::aircraftIconFor(CASCADE_AIRCRAFT_LIGHT, "R44") == AircraftIcon::Light);
        CHECK(cascade::gui::aircraftIconFor(CASCADE_AIRCRAFT_NONE, "R44") == AircraftIcon::Unknown);
    }

    // Every icon is reachable from some category and registry type - an
    // unreachable picture would be dead weight in the binary.
    {
        std::set<int> used;
        for (std::uint32_t c = 0; c < CASCADE_AIRCRAFT_CATEGORY_COUNT; ++c) {
            used.insert(static_cast<int>(aircraftIconForCategory(c)));
            used.insert(static_cast<int>(cascade::gui::aircraftIconFor(c, "R44")));
        }
        CHECK(static_cast<int>(used.size()) == kAircraftIconCount);
    }

    // --- THE POINT OF THE CHANGE: the icons read apart at the standard size --
    //
    // At 32 px - two thirds of the 48 px standard, so a user who turns the
    // size down still gets icons that read apart (the chain halved twice,
    // exactly as the texture is drawn) - the light aeroplane's outline must differ from the airliner's in a large
    // share of its pixels - the flat silhouettes differed so little that a
    // tester saw them as one icon. The same for the other pairs a user has to
    // tell apart at a glance. RED WHEN a redesign makes two of them lookalikes.
    {
        struct Pair { AircraftIcon a, b; const char* what; };
        const Pair pairs[] = {
            {AircraftIcon::Light, AircraftIcon::Airliner, "light vs airliner"},
            {AircraftIcon::Heavy, AircraftIcon::Airliner, "heavy vs airliner"},
            {AircraftIcon::Unknown, AircraftIcon::Airliner, "no type vs airliner"},
            {AircraftIcon::Helicopter, AircraftIcon::Light, "helicopter vs light"},
            {AircraftIcon::Glider, AircraftIcon::Light, "glider vs light"},
            {AircraftIcon::HelicopterLight, AircraftIcon::Helicopter,
             "light helicopter vs helicopter"},
        };
        for (const Pair& p : pairs) {
            const double d = outlineDifference(atSize(p.a, 32), atSize(p.b, 32));
            std::printf("  outline difference at 32 px, %s: %.0f%%\n", p.what, d * 100.0);
            CHECK(d >= 0.20);
        }
    }

    // --- the size setting ---------------------------------------------------
    CHECK(cascade::gui::kAircraftIconDefaultPx == 48);  // the owner's standard
    CHECK(cascade::gui::kAircraftIconMinPx == 16);      // config.cpp clamps to
    CHECK(cascade::gui::kAircraftIconMaxPx == 96);      // these same numbers
    CHECK(clampAircraftIconPx(32) == 32);
    CHECK(clampAircraftIconPx(16) == 16);
    CHECK(clampAircraftIconPx(96) == 96);
    CHECK(clampAircraftIconPx(15) == 16);
    CHECK(clampAircraftIconPx(97) == 96);
    CHECK(clampAircraftIconPx(0) == 16);
    CHECK(clampAircraftIconPx(INT_MIN) == 16);
    CHECK(clampAircraftIconPx(INT_MAX) == 96);
    // The texture is 128 px: the largest setting never draws it bigger than
    // itself, which is what keeps it sharp.
    CHECK(cascade::gui::kAircraftIconMaxPx <= cascade::gui::aircraft_pixels::kSide);
    // The marker never shrinks its click target below what the flat icons had.
    CHECK(cascade::gui::aircraftMarkerRadius(16.0f) >= 14.0f);
    CHECK(cascade::gui::aircraftMarkerRadius(96.0f) >= 48.0f);

    // --- the trail width (0.99.26) -----------------------------------------
    //
    // Up to the wingspan: the heavy's wing is 114 of the 128 pixels of its
    // image, so at the 48 px standard a trail may be 43 px wide.
    {
        using cascade::gui::aircraftWingspanPx;
        using cascade::gui::clampTrailWidthPx;
        CHECK(cascade::gui::kTrailWidthDefaultPx == 4);
        CHECK(aircraftWingspanPx(48) == 43);
        CHECK(aircraftWingspanPx(96) == 86);  // 85.5, rounded half up
        CHECK(aircraftWingspanPx(16) == 14);
        // Never wider than the aircraft, never thinner than a hairline.
        // RED WHEN the cap is dropped or computed from the wrong icon size.
        CHECK(clampTrailWidthPx(1000, 48) == 43);
        CHECK(clampTrailWidthPx(43, 48) == 43);
        CHECK(clampTrailWidthPx(4, 48) == 4);
        CHECK(clampTrailWidthPx(0, 48) == 1);
        CHECK(clampTrailWidthPx(-7, 48) == 1);
        // Shrinking the icons pulls a wide trail down with them.
        CHECK(clampTrailWidthPx(40, 16) == 14);
        // The span measured from the PIXELS agrees with the constant: the
        // heavy's visible wing, tip to tip, is within one texel of it.
        const std::uint8_t* hv = px::kRgba[static_cast<int>(AircraftIcon::Heavy)];
        int minX = px::kSide, maxX = -1;
        for (int y = 0; y < px::kSide; ++y) {
            for (int x = 0; x < px::kSide; ++x) {
                if (hv[(static_cast<std::size_t>(y) * px::kSide + x) * 4u + 3u] >= 64) {
                    minX = std::min(minX, x);
                    maxX = std::max(maxX, x);
                }
            }
        }
        const double measured = static_cast<double>(maxX - minX + 1) / px::kSide;
        std::printf("  heavy wingspan measured %.3f of the icon, constant %.3f\n", measured,
                    static_cast<double>(cascade::gui::kAircraftSpanFraction));
        CHECK(std::fabs(measured - cascade::gui::kAircraftSpanFraction) <= 1.0 / px::kSide);
    }

    // --- which way round ----------------------------------------------------
    {
        const ImVec2 c(100.0f, 50.0f);
        // Course 0 is north: the image stands upright, top edge above.
        const IconQuad n = aircraftIconQuad(c, 0.0, 32.0f);
        CHECK(near(n.p[0].x, 84.0f) && near(n.p[0].y, 34.0f));
        CHECK(near(n.p[2].x, 116.0f) && near(n.p[2].y, 66.0f));
        // Course 90 is EAST: the image's top edge - its nose - is on the right.
        // RED WHEN the rotation turns the wrong way (a westbound picture of an
        // eastbound aircraft looks entirely plausible on a map).
        const IconQuad e = aircraftIconQuad(c, 90.0, 32.0f);
        CHECK(near(e.p[0].x, 116.0f) && near(e.p[0].y, 34.0f));
        CHECK(near(e.p[1].x, 116.0f) && near(e.p[1].y, 66.0f));
        // Unknown course (NaN): points north rather than inventing a heading.
        const IconQuad u = aircraftIconQuad(c, std::nan(""), 32.0f);
        for (int i = 0; i < 4; ++i) {
            CHECK(near(u.p[i].x, n.p[i].x) && near(u.p[i].y, n.p[i].y));
        }
        // The shadow falls down and to the right on the screen.
        const ImVec2 off = cascade::gui::aircraftShadowOffset(32.0f);
        CHECK(off.x > 0.0f && off.y > 0.0f);
    }

    // --- the mipmap step keeps edges clean ----------------------------------
    {
        // One opaque red pixel beside three transparent BLACK ones: a plain
        // average would pull the edge to dark red - the fringe the alpha
        // weighting exists to prevent. RED WHEN the weighting is dropped.
        const std::uint8_t in[16] = {255, 0, 0, 255, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        const std::vector<std::uint8_t> out = halveRgba(in, 2);
        CHECK(out.size() == 4u);
        CHECK(out[0] == 255 && out[1] == 0 && out[2] == 0);
        CHECK(out[3] == 64);
        // All transparent: the colour carried down is their plain average, so
        // the bled edge colour survives the chain.
        const std::uint8_t clear[16] = {200, 100, 0, 0, 200, 100, 0, 0,
                                        200, 100, 0, 0, 200, 100, 0, 0};
        const std::vector<std::uint8_t> c2 = halveRgba(clear, 2);
        CHECK(c2[0] == 200 && c2[1] == 100 && c2[2] == 0 && c2[3] == 0);
        // The full chain runs down to one pixel without complaint.
        std::vector<std::uint8_t> v = pixelsOf(AircraftIcon::Heavy);
        int s = px::kSide;
        while (s > 1) {
            v = halveRgba(v.data(), s);
            s /= 2;
            CHECK(v.size() == static_cast<std::size_t>(s) * s * 4u);
        }
    }

    // --- the shadow ---------------------------------------------------------
    {
        const std::vector<std::uint8_t> icon = pixelsOf(AircraftIcon::Airliner);
        const std::vector<std::uint8_t> sh = shadowFromAlpha(icon.data(), px::kSide, 0.55f);
        CHECK(sh.size() == icon.size());
        int maxA = 0, spread = 0;
        bool black = true;
        for (std::size_t i = 0; i < sh.size(); i += 4) {
            if (sh[i] != 0 || sh[i + 1] != 0 || sh[i + 2] != 0) { black = false; }
            maxA = std::max<int>(maxA, sh[i + 3]);
            // Soft: shadow where the icon itself is fully clear.
            if (sh[i + 3] > 0 && icon[i + 3] == 0) { ++spread; }
        }
        CHECK(black);
        CHECK(maxA <= 141);  // 0.55 of 255, rounded
        CHECK(maxA >= 100);  // but a shadow, not a smudge
        CHECK(spread > 200);
        const std::vector<std::uint8_t> none(icon.size(), 0u);
        const std::vector<std::uint8_t> sh0 = shadowFromAlpha(none.data(), px::kSide, 0.55f);
        bool allClear = true;
        for (std::size_t i = 3; i < sh0.size(); i += 4) { allClear = allClear && sh0[i] == 0; }
        CHECK(allClear);
    }

    return testSummary("test_aircraft_icons");
}
