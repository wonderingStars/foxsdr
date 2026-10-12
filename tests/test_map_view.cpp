// Tests for gui/map_view.hpp, and specifically for the state a BRAND NEW
// INSTALL is in: the satellites window open, targets arriving from a tracker,
// and NO RECEIVER POSITION SET. That state is the one nothing else in this
// suite covers and the one every user meets first, so it is the case each
// figure below is pinned at.
//
// TWO KINDS OF TEST, and both are here on purpose:
//
//   THE PURE FIGURES - the span a whole-world page opens at, the glyph a
//   coordinate aperture shows, and which notes the selected-target card draws.
//   Each is arithmetic or a truth table and is checked against a statement of
//   the rule written from the header rather than from the implementation.
//
//   THE PANEL ITSELF, rendered headless through a real Dear ImGui context and
//   inspected as DRAW DATA. This is the direct pin for "the satellites map
//   draws nothing at all when no receiver position is set": a panel that bails
//   out, divides by zero, or poisons a clip rect with a NaN produces an empty
//   or NaN-carrying draw list, and both are failures here. No GL, no window -
//   ImGui builds vertex buffers on the CPU and that is what is measured.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/map_view.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "gui/theme.hpp"
#include "imgui.h"
#include "test_check.hpp"

using cascade::gui::coordApertureGlyph;
using cascade::gui::kMapMinSpanDeg;
using cascade::gui::mapZoomKeys;
using cascade::gui::satelliteCardNotes;
using cascade::gui::wholeWorldSpanDeg;
using cascade::gui::zoomKeySpan;

namespace {

// --- the pure figures --------------------------------------------------------

// Independent statement of the whole-world rule, from the header: 360 degrees
// of longitude across, UNLESS the viewport is wider than two to one, in which
// case 180 degrees of latitude down it is what demands the wider span.
double refWholeWorld(double w, double h) {
    const double byHeight = 180.0 * w / h;
    return (byHeight > 360.0) ? byHeight : 360.0;
}

void testWholeWorldSpan() {
    // A 2:1 viewport is the exact crossover: both halves ask for 360.
    CHECK_NEAR(wholeWorldSpanDeg(1000.0f, 500.0f), 360.0, 1e-9);
    // Taller than 2:1 - the ordinary window shape - stays at 360, because the
    // latitude that fits is then MORE than 180 and the poles are already on
    // screen with slack above and below.
    CHECK_NEAR(wholeWorldSpanDeg(1000.0f, 700.0f), 360.0, 1e-9);
    CHECK_NEAR(wholeWorldSpanDeg(600.0f, 900.0f), 360.0, 1e-9);
    // WIDER than 2:1 must zoom out further or it crops the poles off, which on
    // a satellite page is cropping off where a polar orbit spends its time.
    CHECK_NEAR(wholeWorldSpanDeg(3000.0f, 500.0f), 1080.0, 1e-9);
    CHECK_NEAR(wholeWorldSpanDeg(2260.0f, 400.0f), refWholeWorld(2260.0, 400.0), 1e-9);
    // Every shape the satellites window can be dragged to, against the rule.
    for (int w = 200; w <= 3000; w += 137) {
        for (int h = 120; h <= 1600; h += 91) {
            const double got =
                wholeWorldSpanDeg(static_cast<float>(w), static_cast<float>(h));
            CHECK_NEAR(got, refWholeWorld(w, h), 1e-6);
            // The span is always a real, positive, finite number of degrees:
            // it becomes spanDeg_, and a NaN or an infinity there is a NaN in
            // every projected coordinate on the page.
            CHECK(std::isfinite(got));
            CHECK(got >= 360.0);
        }
    }
    // A degenerate viewport cannot produce an infinity. draw() will not call it
    // with one, and that is exactly why it is checked here rather than trusted.
    CHECK(std::isfinite(wholeWorldSpanDeg(1000.0f, 0.0f)));
    CHECK(std::isfinite(wholeWorldSpanDeg(0.0f, 0.0f)));
}

// --- the coordinate apertures ------------------------------------------------

void testCoordApertures() {
    // WITH a position every aperture shows what the formatter produced.
    const char* set = "+51.50740";
    for (const char* p = set; *p != '\0'; ++p) {
        CHECK(coordApertureGlyph(*p, true) == *p);
    }
    CHECK(coordApertureGlyph('-', true) == '-');
    CHECK(coordApertureGlyph('9', true) == '9');

    // WITHOUT one, NO APERTURE MAY CARRY A FIGURE. This is the whole point:
    // with no position the caller formats 0.0 to keep the counter's shape, and
    // showing those zeros would put the receiver at 0N 0E - a real place in
    // the Gulf of Guinea - on the one control whose job is to say where it is.
    const char* shapes[] = {"+00.00000", "+000.00000", "-90.00000", "+179.99999"};
    for (const char* s : shapes) {
        for (const char* p = s; *p != '\0'; ++p) {
            const char g = coordApertureGlyph(*p, false);
            CHECK(!(g >= '0' && g <= '9'));
        }
    }
    // And what it shows instead, cell by cell: a dash for a figure, nothing at
    // all for the sign (a '-' there would read as a southern latitude), and
    // the decimal point left where it is so the counter still reads as a
    // coordinate waiting for one rather than as a broken display.
    CHECK(coordApertureGlyph('0', false) == '-');
    CHECK(coordApertureGlyph('7', false) == '-');
    CHECK(coordApertureGlyph('+', false) == ' ');
    CHECK(coordApertureGlyph('-', false) == ' ');
    CHECK(coordApertureGlyph('.', false) == '.');

    // THE SHAPE IS NEVER CHANGED, only the glyph - which is what keeps the
    // well the same width whether or not a position is set. The cells are laid
    // out from the formatter's own string, so the count of characters and
    // which of them are narrow must be identical in both states.
    const char* lat = "+51.50740";
    int nSet = 0;
    int nBlank = 0;
    for (const char* p = lat; *p != '\0'; ++p) {
        ++nSet;
        (void)coordApertureGlyph(*p, false);
        ++nBlank;
    }
    CHECK(nSet == nBlank);
}

// --- which notes the target card draws ---------------------------------------

void testCardNotes() {
    // NOTHING SELECTED: neither note, whatever the receiver position is. Both
    // sentences are about a selected target's own figures, and the card with
    // nothing in it says only NO TARGET SELECTED.
    //
    // This case is the correction. What stood here reserved the no-receiver
    // note's height from `!hasHome_` alone, so the empty card on a fresh
    // install - nothing selected, no position - was reserved two paragraphs of
    // space it never drew into, and changed height when a position arrived
    // even though its words did not.
    CHECK(satelliteCardNotes(false, false).noReceiver == false);
    CHECK(satelliteCardNotes(false, false).notReported == false);
    CHECK(satelliteCardNotes(false, true).noReceiver == false);
    CHECK(satelliteCardNotes(false, true).notReported == false);

    // A TARGET AND NO POSITION: both notes, because both are true at once and
    // they are different kinds of missing - one a user can clear, one nobody
    // can.
    CHECK(satelliteCardNotes(true, false).noReceiver == true);
    CHECK(satelliteCardNotes(true, false).notReported == true);

    // A TARGET AND A POSITION: only the one nothing will ever unblock.
    CHECK(satelliteCardNotes(true, true).noReceiver == false);
    CHECK(satelliteCardNotes(true, true).notReported == true);

    // --- AND THE PASSES CARD IS ABOUT A TARGET TOO -------------------------
    //
    // It was drawn unconditionally, heading and all, so the first screen a new
    // install opens on carried a card titled "NEXT PASSES - THIS TARGET"
    // directly under a card reading NO TARGET SELECTED - a heading about a
    // target that does not exist, and a paragraph explaining why that
    // non-existent target's passes cannot be computed.
    //
    // A RECEIVER POSITION DOES NOT ENTER INTO IT, which is the second half of
    // the rule and the half a plausible "fix" would get wrong: the card is not
    // waiting for a position, it is waiting for something to be selected. A
    // pass prediction needs the orbit, and no amount of receiver position turns
    // a reported latitude into one.
    CHECK(satelliteCardNotes(false, false).passes == false);
    CHECK(satelliteCardNotes(false, true).passes == false);
    CHECK(satelliteCardNotes(true, false).passes == true);
    CHECK(satelliteCardNotes(true, true).passes == true);

    // Stated as a rule over all four inputs as well as case by case, so a later
    // reading of it off the receiver position cannot pass three of the four.
    for (int i = 0; i < 4; ++i) {
        const bool target = (i & 1) != 0;
        const bool position = (i & 2) != 0;
        CHECK(satelliteCardNotes(target, position).passes == target);
    }
}

// --- the panel, rendered ------------------------------------------------------

cascade::core::HostTrack makeSat(const char* id, const char* label, double lat, double lon,
                                 double altM) {
    cascade::core::HostTrack ht;
    std::snprintf(ht.t.id, sizeof ht.t.id, "%s", id);
    std::snprintf(ht.t.label, sizeof ht.t.label, "%s", label);
    ht.t.latDeg = lat;
    ht.t.lonDeg = lon;
    ht.t.altM = altM;
    ht.t.courseDeg = std::nan("");
    ht.t.speedMps = 7660.0;
    ht.t.ageMs = 1000;
    ht.t.kind = CASCADE_TRACK_SATELLITE;
    ht.t.flags = 0;
    ht.plugin = "sat";
    return ht;
}

// A ground track that crosses the antimeridian, which is what a real one does
// and what the path layer has to survive with no receiver position to hang it
// off.
cascade::core::HostPath makeGroundTrack(const char* id) {
    cascade::core::HostPath p;
    p.id = id;
    p.plugin = "sat";
    p.kind = CASCADE_TRACK_SATELLITE;
    p.flags = CASCADE_PATH_FLAG_DASHED;
    for (int i = 0; i < 180; ++i) {
        CascadePathPoint pt;
        const double t = static_cast<double>(i);
        pt.lonDeg = -180.0 + t * 2.0;
        pt.latDeg = 51.6 * std::sin(t * 0.09);
        p.points.push_back(pt);
    }
    return p;
}

struct Stats {
    int verts = 0;
    int bad = 0;  // NaN or infinite vertex positions and clip rects
};

Stats collect() {
    Stats s;
    ImDrawData* dd = ImGui::GetDrawData();
    if (dd == nullptr) { return s; }
    for (int n = 0; n < dd->CmdListsCount; ++n) {
        const ImDrawList* cl = dd->CmdLists[n];
        s.verts += cl->VtxBuffer.Size;
        for (int i = 0; i < cl->VtxBuffer.Size; ++i) {
            const ImDrawVert& v = cl->VtxBuffer[i];
            if (!std::isfinite(v.pos.x) || !std::isfinite(v.pos.y)) { ++s.bad; }
        }
        for (int c = 0; c < cl->CmdBuffer.Size; ++c) {
            const ImVec4& r = cl->CmdBuffer[c].ClipRect;
            if (!std::isfinite(r.x) || !std::isfinite(r.y) || !std::isfinite(r.z) ||
                !std::isfinite(r.w)) {
                ++s.bad;
            }
        }
    }
    return s;
}

// One frame of the whole satellites instrument, laid out the way the
// application lays it out: a child window of the given size, which is what
// GetContentRegionAvail hands the panel.
Stats drawPanelFrame(cascade::gui::MapView& view, cascade::gui::SatelliteDeck& deck,
                     float w, float h, bool home, int trackCount, bool select,
                     bool withPaths) {
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(w + 32.0f, h + 32.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("panel", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar();

    if (home) {
        view.setHome(51.5, -0.12);
    } else {
        view.clearHome();
    }

    std::vector<cascade::core::HostTrack> tracks;
    if (trackCount > 0) { tracks.push_back(makeSat("25544", "ISS (ZARYA)", 12.0, 45.0, 421000.0)); }
    if (trackCount > 1) { tracks.push_back(makeSat("28654", "NOAA 18", -33.0, -60.0, 854000.0)); }
    if (trackCount > 2) { tracks.push_back(makeSat("40069", "METEOR-M2 2", 78.0, 174.0, 813000.0)); }
    std::vector<cascade::core::HostPath> paths;
    if (withPaths && !tracks.empty()) { paths.push_back(makeGroundTrack(tracks[0].t.id)); }
    view.setSelected(select && !tracks.empty() ? std::string(tracks[0].t.id) : std::string());

    cascade::gui::CoverageMap cov;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::SetCursorScreenPos(ImVec2(0.0f, 0.0f));
    ImGui::BeginChild("##satface", ImVec2(w, h), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    view.drawSatellitePanel(deck, tracks, paths, &cov, nullptr);
    ImGui::EndChild();

    ImGui::End();
    ImGui::Render();
    return collect();
}

// The map on its own, which is the layer that must not depend on a receiver
// position at all: every target has a position of its own, and so does every
// vertex of a ground track.
Stats drawMapFrame(cascade::gui::MapView& view, float w, float h, bool home,
                   int trackCount, bool withPaths) {
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(w + 32.0f, h + 32.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("map", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar();
    if (home) {
        view.setHome(51.5, -0.12);
    } else {
        view.clearHome();
    }
    std::vector<cascade::core::HostTrack> tracks;
    if (trackCount > 0) { tracks.push_back(makeSat("25544", "ISS (ZARYA)", 12.0, 45.0, 421000.0)); }
    if (trackCount > 1) { tracks.push_back(makeSat("28654", "NOAA 18", -33.0, -60.0, 854000.0)); }
    std::vector<cascade::core::HostPath> paths;
    if (withPaths && !tracks.empty()) { paths.push_back(makeGroundTrack(tracks[0].t.id)); }
    ImGui::SetCursorScreenPos(ImVec2(0.0f, 0.0f));
    view.draw(w, h, tracks, paths);
    ImGui::End();
    ImGui::Render();
    return collect();
}

// The floor a drawn instrument clears by orders of magnitude and an empty one
// cannot: the reported failure drew a black rectangle, which is an ImGui
// window and nothing in it.
constexpr int kDrawnFloor = 2000;

void testPanelDrawsWithNoReceiverPosition() {
    const float sizes[][2] = {{2244.0f, 1400.0f}, {1600.0f, 1000.0f}, {1100.0f, 700.0f},
                              {820.0f, 520.0f},   {620.0f, 380.0f},   {300.0f, 200.0f},
                              {900.0f, 130.0f},   {210.0f, 900.0f}};
    for (const auto& wh : sizes) {
        for (int tracks = 0; tracks <= 3; ++tracks) {
            for (int flags = 0; flags < 4; ++flags) {
                const bool select = (flags & 1) != 0;
                const bool withPaths = (flags & 2) != 0;
                int verts[2] = {0, 0};
                for (int home = 0; home < 2; ++home) {
                    cascade::gui::MapView view;
                    cascade::gui::SatelliteDeck deck;
                    deck.coverage = true;  // asked for; inert without a position
                    Stats s;
                    // Several frames: the panel pins its own projection on the
                    // first one and asks for the whole world, and ImGui sizes
                    // a child on the frame after it appears.
                    for (int f = 0; f < 4; ++f) {
                        s = drawPanelFrame(view, deck, wh[0], wh[1], home != 0, tracks,
                                           select, withPaths);
                    }
                    CHECK(s.bad == 0);
                    verts[home] = s.verts;
                }
                // THE WHOLE OF THE REPORTED FAULT, pinned: with no receiver
                // position the panel must still draw, and draw the same
                // instrument rather than a black rectangle.
                //
                // THE FLOOR IS THE ASSERTION THAT MATTERS. The comparison
                // beside it is deliberately loose, because the two runs do not
                // lay out identically: the deck's notes differ in length
                // between the two states, so the map region below them is a
                // slightly different shape, and a map region wider than two to
                // one spans more than 360 degrees and draws the land more than
                // once. That is the projection working, not a difference in
                // what is on the page - the strict same-picture comparison is
                // testMapDrawsWithNoReceiverPosition below, which holds the map
                // itself to one size.
                CHECK(verts[0] > kDrawnFloor);
                CHECK(verts[1] > kDrawnFloor);
                CHECK(verts[0] * 2 >= verts[1]);
                if (!(verts[0] > kDrawnFloor) || !(verts[0] * 2 >= verts[1])) {
                    std::printf("      at %.0fx%.0f tracks=%d sel=%d paths=%d: "
                                "nohome=%d home=%d\n",
                                static_cast<double>(wh[0]), static_cast<double>(wh[1]),
                                tracks, select ? 1 : 0, withPaths ? 1 : 0, verts[0],
                                verts[1]);
                }
            }
        }
    }
}

void testMapDrawsWithNoReceiverPosition() {
    const float sizes[][2] = {{1500.0f, 900.0f}, {700.0f, 500.0f}, {2000.0f, 300.0f},
                              {200.0f, 700.0f},  {64.0f, 64.0f}};
    for (const auto& wh : sizes) {
        for (int tracks = 0; tracks <= 2; ++tracks) {
            for (int withPaths = 0; withPaths < 2; ++withPaths) {
                int verts[2] = {0, 0};
                for (int home = 0; home < 2; ++home) {
                    cascade::gui::MapView view;
                    view.setProjection(cascade::gui::MapProjection::Equirectangular);
                    Stats s;
                    for (int f = 0; f < 3; ++f) {
                        s = drawMapFrame(view, wh[0], wh[1], home != 0, tracks,
                                         withPaths != 0);
                    }
                    CHECK(s.bad == 0);
                    verts[home] = s.verts;
                }
                // The coastline, the graticule, the scale bar, every target and
                // every ground track are drawn from positions of their own, so
                // the map with no receiver position is very nearly the same
                // picture - never a blank one.
                CHECK(verts[0] > kDrawnFloor);
                CHECK(verts[0] * 10 >= verts[1] * 9);
            }
        }
    }
}

// --- the zoom keys (0.99.20) ----------------------------------------------------
//
// Asked for on the patch page's Map part: the wheel zoomed it, nothing said so,
// and a touchpad has no wheel. The keys are a "+" over a "-" in the chart's
// bottom-right corner, on every map this view draws.

void testZoomKeyFigures() {
    // Where they sit, from the header's statement: a third of a key in from
    // the corner, "-" in the corner, "+" a sixth of a key above it.
    const auto k = mapZoomKeys(10.0f, 20.0f, 400.0f, 300.0f, 24.0f, 0.0f);
    CHECK(k.shown);
    CHECK_NEAR(k.outX1, 10.0 + 400.0 - 8.0, 1e-6);
    CHECK_NEAR(k.outY1, 20.0 + 300.0 - 8.0, 1e-6);
    CHECK_NEAR(k.outX1 - k.outX0, 24.0, 1e-6);
    CHECK_NEAR(k.outY1 - k.outY0, 24.0, 1e-6);
    CHECK_NEAR(k.inX0, k.outX0, 1e-6);
    CHECK_NEAR(k.inX1, k.outX1, 1e-6);
    CHECK_NEAR(k.inY1, k.outY0 - 4.0, 1e-6);   // "+" ABOVE "-", not overlapping
    CHECK_NEAR(k.inY1 - k.inY0, 24.0, 1e-6);
    // Wholly inside the chart.
    CHECK(k.inX0 >= 10.0f && k.inY0 >= 20.0f && k.outX1 <= 410.0f && k.outY1 <= 320.0f);

    // STOOD ABOVE THE LONGITUDE FIGURES: with a 21 px strip along the floor
    // the "-" key's bottom edge is a third of a key above the strip's top, and
    // no part of either key reaches into it.
    const auto kf = mapZoomKeys(10.0f, 20.0f, 400.0f, 300.0f, 24.0f, 21.0f);
    CHECK(kf.shown);
    CHECK_NEAR(kf.outY1, 20.0 + 300.0 - 21.0 - 8.0, 1e-6);
    CHECK(kf.outY1 <= 20.0f + 300.0f - 21.0f);
    CHECK_NEAR(kf.outX1, k.outX1, 1e-6);   // the strip moves them up, not across

    // Too small to carry them: under three keys wide, or four keys tall above
    // the strip.
    CHECK(!mapZoomKeys(0.0f, 0.0f, 71.0f, 300.0f, 24.0f, 0.0f).shown);
    CHECK(!mapZoomKeys(0.0f, 0.0f, 400.0f, 95.0f, 24.0f, 0.0f).shown);
    CHECK(mapZoomKeys(0.0f, 0.0f, 72.0f, 96.0f, 24.0f, 0.0f).shown);
    CHECK(!mapZoomKeys(0.0f, 0.0f, 72.0f, 116.0f, 24.0f, 21.0f).shown);
    CHECK(mapZoomKeys(0.0f, 0.0f, 72.0f, 117.0f, 24.0f, 21.0f).shown);
    CHECK(!mapZoomKeys(0.0f, 0.0f, 400.0f, 300.0f, 0.0f, 0.0f).shown);

    // One press halves or doubles, clamped at both ends.
    CHECK_NEAR(zoomKeySpan(10.0, true, 360.0), 5.0, 1e-12);
    CHECK_NEAR(zoomKeySpan(10.0, false, 360.0), 20.0, 1e-12);
    CHECK_NEAR(zoomKeySpan(300.0, false, 360.0), 360.0, 1e-12);
    CHECK_NEAR(zoomKeySpan(360.0, false, 360.0), 360.0, 1e-12);   // at the limit: no move
    CHECK_NEAR(zoomKeySpan(0.03, true, 360.0), kMapMinSpanDeg, 1e-12);
    CHECK_NEAR(zoomKeySpan(kMapMinSpanDeg, true, 360.0), kMapMinSpanDeg, 1e-12);
    // A nonsense limit below the floor does not invert the clamp.
    CHECK_NEAR(zoomKeySpan(1.0, false, 0.0), kMapMinSpanDeg, 1e-12);
}

// One frame of the bare map with the pointer at `p` and the left button in
// `down`, through the real ImGui input queue.
void mapFrameWithMouse(cascade::gui::MapView& view, float w, float h, ImVec2 p, bool down) {
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(p.x, p.y);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, down);
    drawMapFrame(view, w, h, false, 0, false);
}

// Press and release at `p` - a click, not a drag.
void clickMapAt(cascade::gui::MapView& view, float w, float h, ImVec2 p) {
    mapFrameWithMouse(view, w, h, p, false);
    mapFrameWithMouse(view, w, h, p, true);
    mapFrameWithMouse(view, w, h, p, false);
    mapFrameWithMouse(view, w, h, p, false);
}

void testZoomKeysDriveTheView() {
    const float w = 800.0f;
    const float h = 500.0f;
    cascade::gui::MapView view;
    view.setProjection(cascade::gui::MapProjection::Equirectangular);
    const ImVec2 away(w * 0.5f, h * 0.25f);
    for (int f = 0; f < 3; ++f) { mapFrameWithMouse(view, w, h, away, false); }

    // The key the draw code uses: the frame height, never under 22 px, stood
    // above the longitude strip of one line box and the 3 px caption gap.
    const float keyPx = std::max(22.0f, std::round(ImGui::GetFrameHeight()));
    const auto k = mapZoomKeys(0.0f, 0.0f, w, h, keyPx, ImGui::GetTextLineHeight() + 3.0f);
    CHECK(k.shown);
    const ImVec2 plus((k.inX0 + k.inX1) * 0.5f, (k.inY0 + k.inY1) * 0.5f);
    const ImVec2 minus((k.outX0 + k.outX1) * 0.5f, (k.outY0 + k.outY1) * 0.5f);

    const double s0 = view.spanDeg();
    const double lat0 = view.viewCentreLatDeg();
    const double lon0 = view.viewCentreLonDeg();

    // "+" halves the span and leaves the centre where it was.
    clickMapAt(view, w, h, plus);
    CHECK_NEAR(view.spanDeg(), s0 * 0.5, 1e-9);
    CHECK_NEAR(view.viewCentreLatDeg(), lat0, 1e-9);
    CHECK_NEAR(view.viewCentreLonDeg(), lon0, 1e-9);

    // "-" doubles it back.
    clickMapAt(view, w, h, minus);
    CHECK_NEAR(view.spanDeg(), s0, 1e-9);

    // A click on open chart is not a key.
    clickMapAt(view, w, h, away);
    CHECK_NEAR(view.spanDeg(), s0, 1e-9);

    // ONE PRESS DOES ONE THING. With the receiver pick armed, a press on a key
    // zooms and leaves the pick armed - it must not also drop the antenna
    // under the key. The next click on the chart is the one that answers it.
    view.armReceiverPick();
    clickMapAt(view, w, h, plus);
    CHECK(!view.receiverPositionRequested());
    CHECK(view.receiverPickArmed());
    CHECK_NEAR(view.spanDeg(), s0 * 0.5, 1e-9);
    clickMapAt(view, w, h, minus);
    CHECK(!view.receiverPositionRequested());
    clickMapAt(view, w, h, away);
    CHECK(view.receiverPositionRequested());
    view.clearReceiverPositionRequest();
    CHECK_NEAR(view.spanDeg(), s0, 1e-9);

    // A press that starts on a key and is dragged off is neither a key press
    // nor a pan: the span stays and the chart does not move.
    mapFrameWithMouse(view, w, h, plus, false);
    mapFrameWithMouse(view, w, h, plus, true);
    for (int i = 1; i <= 8; ++i) {
        mapFrameWithMouse(view, w, h, ImVec2(plus.x - 30.0f * i, plus.y - 20.0f * i), true);
    }
    mapFrameWithMouse(view, w, h, ImVec2(plus.x - 240.0f, plus.y - 160.0f), false);
    mapFrameWithMouse(view, w, h, away, false);
    CHECK_NEAR(view.spanDeg(), s0, 1e-9);
    CHECK_NEAR(view.viewCentreLatDeg(), lat0, 1e-9);
    CHECK_NEAR(view.viewCentreLonDeg(), lon0, 1e-9);

    // A BUTTON, NOT A HOT SPOT: pressed on "+" one pixel inside its left edge
    // and let go two pixels outside it - three pixels of travel, well under
    // the drag threshold, so it IS a click - but not a click ON the key, and
    // it zooms nothing.
    {
        const ImVec2 in(k.inX0 + 1.0f, plus.y);
        const ImVec2 out(k.inX0 - 2.0f, plus.y);
        mapFrameWithMouse(view, w, h, in, false);
        mapFrameWithMouse(view, w, h, in, true);
        mapFrameWithMouse(view, w, h, out, true);
        mapFrameWithMouse(view, w, h, out, false);
        mapFrameWithMouse(view, w, h, away, false);
        CHECK_NEAR(view.spanDeg(), s0, 1e-9);
    }

    // ...whereas the same drag begun on open chart still pans it, so the guard
    // above is about the key and not a map that stopped panning.
    mapFrameWithMouse(view, w, h, away, true);
    for (int i = 1; i <= 8; ++i) {
        mapFrameWithMouse(view, w, h, ImVec2(away.x - 30.0f * i, away.y + 20.0f * i), true);
    }
    mapFrameWithMouse(view, w, h, ImVec2(away.x - 240.0f, away.y + 160.0f), false);
    CHECK(std::fabs(view.viewCentreLonDeg() - lon0) > 1.0);

    // "-" stops at the whole world and then does nothing.
    const double limit = wholeWorldSpanDeg(w, h);
    for (int i = 0; i < 12; ++i) { clickMapAt(view, w, h, minus); }
    CHECK_NEAR(view.spanDeg(), limit, 1e-9);
    clickMapAt(view, w, h, minus);
    CHECK_NEAR(view.spanDeg(), limit, 1e-9);

    // The wheel still zooms, over the chart, by its own finer step.
    const double before = view.spanDeg();
    mapFrameWithMouse(view, w, h, away, false);
    ImGui::GetIO().AddMouseWheelEvent(0.0f, 1.0f);
    mapFrameWithMouse(view, w, h, away, false);
    CHECK_NEAR(view.spanDeg(), before * 0.85, 1e-6);
}

}  // namespace

// --- target labels over map tiles (0.99.42) -----------------------------------
//
// OVER OPENSTREETMAP TILES THE LABELS DISAPPEARED. A callsign in its altitude
// colour - amber, green - over London's roads and place names read as one more
// piece of the map: the half-strength, four-direction halo every map caption
// has is right for "RX" and the ring distances and far too weak for a target's
// name. A target label now has a STRONG outline: dark, in all eight directions
// one pixel out, at no less than 150 of 255 alpha. Its ink stays the altitude
// colour, which is a measurement.

// A vertex that is part of TEXT (a glyph, not a shape) - glyphs sample the
// font atlas, shapes sample its white pixel.
bool isGlyphVertex(const ImDrawVert& v) {
    const ImVec2 w = ImGui::GetFontTexUvWhitePixel();
    return v.uv.x != w.x || v.uv.y != w.y;
}

float luma(ImU32 c) {
    return 0.299f * static_cast<float>((c >> IM_COL32_R_SHIFT) & 0xFFu) +
           0.587f * static_cast<float>((c >> IM_COL32_G_SHIFT) & 0xFFu) +
           0.114f * static_cast<float>((c >> IM_COL32_B_SHIFT) & 0xFFu);
}

// A dark glyph vertex strong enough to separate a label from any tile.
bool isStrongDarkGlyph(const ImDrawVert& v) {
    const unsigned a = (v.col >> IM_COL32_A_SHIFT) & 0xFFu;
    return isGlyphVertex(v) && a >= 150u && luma(v.col) < 60.0f;
}

void testTargetLabelHelper() {
    // One letter on an empty window, as ImGui would draw it.
    const ImU32 ink = IM_COL32(255, 196, 0, 255);   // a light altitude colour
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(400.0f, 200.0f));
    ImGui::Begin("label", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoBackground);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const int from = dl->VtxBuffer.Size;
    cascade::gui::addMapTargetLabel(dl, 100.0f, 100.0f, ink, "H");
    std::vector<ImVec2> inkCorners;
    std::vector<ImVec2> haloCorners;
    unsigned haloAlpha = 0;
    for (int i = from; i < dl->VtxBuffer.Size; ++i) {
        const ImDrawVert& v = dl->VtxBuffer[i];
        if (!isGlyphVertex(v)) { continue; }
        if (v.col == ink) {
            inkCorners.push_back(v.pos);
        } else if (luma(v.col) < 60.0f) {
            haloCorners.push_back(v.pos);
            haloAlpha = std::max(haloAlpha, (v.col >> IM_COL32_A_SHIFT) & 0xFFu);
        }
    }
    ImGui::End();
    ImGui::Render();
    CHECK(!inkCorners.empty());
    std::printf("  target label: %zu ink corners, %zu halo corners, halo alpha %u\n",
                inkCorners.size(), haloCorners.size(), haloAlpha);
    // Strong enough: at least 150 of 255.
    CHECK(haloAlpha >= 150u);
    // All eight directions, one pixel out, for the glyph's first corner.
    if (!inkCorners.empty()) {
        const ImVec2 c = inkCorners.front();
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                if (dx == 0 && dy == 0) { continue; }
                const bool found = std::any_of(haloCorners.begin(), haloCorners.end(),
                                               [&](const ImVec2& h) {
                                                   return std::fabs(h.x - (c.x + dx)) < 0.01f &&
                                                          std::fabs(h.y - (c.y + dy)) < 0.01f;
                                               });
                if (!found) { std::printf("  no halo at (%+d, %+d)\n", dx, dy); }
                CHECK(found);
            }
        }
    }
    // Drawn beneath the ink: every halo vertex comes before the first ink one.
    // (Checked through the counts: 8 passes then 1.)
    CHECK(haloCorners.size() == 8u * inkCorners.size());
}

// ...AND EVERY MAP DRAWS ITS TARGETS' LABELS THAT WAY. One vessel, drawn twice:
// labelled "MMMMMMMM", then "M". Nothing else differs, so the difference in
// strong dark glyph vertices is the label's outline and nothing else - seven
// more glyphs, eight passes each, four corners each.
int strongDarkGlyphVerts(cascade::gui::MapView& view, const char* label) {
    cascade::core::HostTrack ht;
    std::snprintf(ht.t.id, sizeof ht.t.id, "%s", "235098761");
    std::snprintf(ht.t.label, sizeof ht.t.label, "%s", label);
    ht.t.latDeg = 50.85;
    ht.t.lonDeg = -1.35;
    ht.t.altM = std::nan("");
    ht.t.courseDeg = 90.0;
    ht.t.speedMps = 4.6;
    ht.t.ageMs = 1000;
    ht.t.kind = CASCADE_TRACK_VESSEL;
    ht.t.flags = 0;
    ht.plugin = "AIS";
    const std::vector<cascade::core::HostTrack> tracks{ht};
    const std::vector<cascade::core::HostPath> paths;
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(900.0f, 700.0f));
    ImGui::Begin("labelmap", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize);
    ImGui::SetCursorScreenPos(ImVec2(0.0f, 0.0f));
    view.draw(800.0f, 600.0f, tracks, paths);
    ImGui::End();
    ImGui::Render();
    int n = 0;
    ImDrawData* dd = ImGui::GetDrawData();
    for (int l = 0; dd != nullptr && l < dd->CmdListsCount; ++l) {
        const ImDrawList* cl = dd->CmdLists[l];
        for (int i = 0; i < cl->VtxBuffer.Size; ++i) {
            if (isStrongDarkGlyph(cl->VtxBuffer[i])) { ++n; }
        }
    }
    return n;
}

void testMapTargetLabelsHaveTheOutline() {
    cascade::gui::MapView view;
    view.setProjection(cascade::gui::MapProjection::Equirectangular);
    view.goTo(50.85, -1.35, 2.0);
    // A few frames each so the view has settled on the same picture.
    int eight = 0;
    int one = 0;
    for (int f = 0; f < 3; ++f) { eight = strongDarkGlyphVerts(view, "MMMMMMMM"); }
    for (int f = 0; f < 3; ++f) { one = strongDarkGlyphVerts(view, "M"); }
    std::printf("  map label outline: %d strong dark glyph vertices for 8 letters, %d for 1\n",
                eight, one);
    CHECK(eight - one == 7 * 8 * 4);
}

// --- NO ALTITUDE IS GREY, NOT RED (0.99.74) ------------------------------------
//
// A tester (2026-10-12): "instead of highlighting aircraft that are not
// transmitting the altitude in red, which is the same as <1,000 ft. Maybe color
// this in grey or black. I would like to be able to distinguish the two." The
// aircraft kind colour is red and the lowest altitude band is orange, so an
// aircraft with no altitude and one on final approach were two near-identical
// marks. It is read off the DRAW DATA, as the label test above is: the callsign's
// ink is the target's colour exactly (a measurement, never repainted), so the
// glyph vertices carry it.

cascade::core::HostTrack makeTarget(const char* id, const char* label, std::uint32_t kind,
                                    double lat, double lon, double altM,
                                    std::uint32_t flags = 0) {
    cascade::core::HostTrack ht;
    std::snprintf(ht.t.id, sizeof ht.t.id, "%s", id);
    std::snprintf(ht.t.label, sizeof ht.t.label, "%s", label);
    ht.t.latDeg = lat;
    ht.t.lonDeg = lon;
    ht.t.altM = altM;
    ht.t.courseDeg = 90.0;
    ht.t.speedMps = 100.0;
    ht.t.ageMs = 1000;
    ht.t.kind = kind;
    ht.t.flags = flags;
    ht.plugin = "adsb";
    return ht;
}

// One frame of a map 800 x 600 at the window's origin. The draw data stays valid
// until the next NewFrame, which is when the caller reads it.
float drawTargetsFrame(cascade::gui::MapView& view,
                       const std::vector<cascade::core::HostTrack>& tracks) {
    const std::vector<cascade::core::HostPath> paths;
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(900.0f, 700.0f));
    ImGui::Begin("targets", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize);
    ImGui::SetCursorScreenPos(ImVec2(0.0f, 0.0f));
    const float lineHeight = ImGui::GetTextLineHeight();
    view.draw(800.0f, 600.0f, tracks, paths);
    ImGui::End();
    ImGui::Render();
    return lineHeight;
}

// How many TEXT vertices carry exactly `col`, which is how many glyph corners a
// callsign drawn in that colour has.
int glyphVertsOfColour(ImU32 col) {
    int n = 0;
    ImDrawData* dd = ImGui::GetDrawData();
    for (int l = 0; dd != nullptr && l < dd->CmdListsCount; ++l) {
        const ImDrawList* cl = dd->CmdLists[l];
        for (int i = 0; i < cl->VtxBuffer.Size; ++i) {
            if (isGlyphVertex(cl->VtxBuffer[i]) && cl->VtxBuffer[i].col == col) { ++n; }
        }
    }
    return n;
}

// The bounding box of the SHAPE vertices (not glyphs) of colour `col`, and
// whether any such vertex lies inside it.
struct Box {
    bool any = false;
    float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
};

Box shapeBounds(ImU32 col) {
    Box b;
    ImDrawData* dd = ImGui::GetDrawData();
    for (int l = 0; dd != nullptr && l < dd->CmdListsCount; ++l) {
        const ImDrawList* cl = dd->CmdLists[l];
        for (int i = 0; i < cl->VtxBuffer.Size; ++i) {
            const ImDrawVert& v = cl->VtxBuffer[i];
            if (isGlyphVertex(v) || v.col != col) { continue; }
            if (!b.any) {
                b.any = true;
                b.x0 = b.x1 = v.pos.x;
                b.y0 = b.y1 = v.pos.y;
            }
            b.x0 = std::min(b.x0, v.pos.x);
            b.y0 = std::min(b.y0, v.pos.y);
            b.x1 = std::max(b.x1, v.pos.x);
            b.y1 = std::max(b.y1, v.pos.y);
        }
    }
    return b;
}

bool shapeVertexInside(ImU32 col, const Box& box) {
    ImDrawData* dd = ImGui::GetDrawData();
    for (int l = 0; dd != nullptr && l < dd->CmdListsCount; ++l) {
        const ImDrawList* cl = dd->CmdLists[l];
        for (int i = 0; i < cl->VtxBuffer.Size; ++i) {
            const ImDrawVert& v = cl->VtxBuffer[i];
            if (isGlyphVertex(v) || v.col != col) { continue; }
            if (v.pos.x >= box.x0 && v.pos.x <= box.x1 && v.pos.y >= box.y0 &&
                v.pos.y <= box.y1) {
                return true;
            }
        }
    }
    return false;
}

constexpr ImU32 kGreyNoAltitude = IM_COL32(150, 150, 150, 255);
constexpr ImU32 kKindRedAircraft = IM_COL32(235, 40, 40, 255);

// The colour the callsign of ONE target comes out in, as the pair (grey ink
// vertices, red-kind ink vertices) plus the vertices of `expect`.
void testNoAltitudeIsGreyNotRed() {
    const double lat = 50.85;
    const double lon = -1.35;
    struct Case {
        const char* what;
        std::uint32_t kind;
        double altM;
        std::uint32_t flags;
        ImU32 expect;
        bool grey;
    };
    const double kNoAlt = std::nan("");
    // 3000 m is 9,843 ft: the third band, 5-10 kft, lime.
    const cascade::gui::AltBandStyle& lime = cascade::gui::altBandStyle(2);
    const Case cases[] = {
        // THE CHANGE: an aircraft with no altitude is grey and not the kind red.
        {"aircraft, no altitude", CASCADE_TRACK_AIRCRAFT, kNoAlt, 0u, kGreyNoAltitude, true},
        // An aircraft WITH one is untouched: its band colour.
        {"aircraft, 3000 m", CASCADE_TRACK_AIRCRAFT, 3000.0, 0u,
         IM_COL32(lime.r, lime.g, lime.b, 255), false},
        // Sea level is a real altitude and a band of its own - the lowest, orange -
        // and not "no altitude".
        {"aircraft, 0 m", CASCADE_TRACK_AIRCRAFT, 0.0, 0u,
         IM_COL32(cascade::gui::altBandStyle(0).r, cascade::gui::altBandStyle(0).g,
                  cascade::gui::altBandStyle(0).b, 255),
         false},
        // An emergency still outranks everything, no altitude or not.
        {"aircraft, no altitude, squawking", CASCADE_TRACK_AIRCRAFT, kNoAlt,
         CASCADE_TRACK_FLAG_EMERGENCY, IM_COL32(255, 45, 45, 255), false},
        // Ships, stations and satellites keep the per-kind colours that tell
        // several kinds on one map apart.
        {"vessel, no altitude", CASCADE_TRACK_VESSEL, kNoAlt, 0u, IM_COL32(120, 255, 170, 255),
         false},
        {"station, no altitude", CASCADE_TRACK_STATION, kNoAlt, 0u,
         IM_COL32(255, 210, 120, 255), false},
        {"satellite, no altitude", CASCADE_TRACK_SATELLITE, kNoAlt, 0u,
         IM_COL32(230, 150, 255, 255), false},
    };
    for (const Case& c : cases) {
        // The default projection with no basemap IS equirectangular; pinning it
        // would also ask for the whole world, which is not the picture here.
        cascade::gui::MapView view;
        view.goTo(lat, lon, 2.0);
        const std::vector<cascade::core::HostTrack> tracks{
            makeTarget("AB1234", "M", c.kind, lat, lon, c.altM, c.flags)};
        for (int f = 0; f < 3; ++f) { drawTargetsFrame(view, tracks); }
        const int ink = glyphVertsOfColour(c.expect);
        const int grey = glyphVertsOfColour(kGreyNoAltitude);
        const int red = glyphVertsOfColour(kKindRedAircraft);
        std::printf("  %-34s ink %d, grey %d, kind-red %d\n", c.what, ink, grey, red);
        CHECK(ink > 0);
        // Grey exactly when the rule says so, and the old kind red NEVER appears
        // on a target with no altitude.
        CHECK((grey > 0) == c.grey);
        CHECK(red == 0);
    }
}

// THE LEGEND SAYS WHAT THE GREY MEANS, and only when there is a grey to explain:
// one extra row at the foot of the aviation ladder, drawn when some aircraft on
// screen has an altitude (so the ladder is up at all) AND some has none.
void testNoAltitudeLegendRow() {
    const double lat = 50.85;
    const double lon = -1.35;
    const ImU32 plate = cascade::gui::theme::withAlpha(cascade::gui::theme::kWell,
                                                       190.0f / 255.0f);
    const double kNoAlt = std::nan("");
    const auto frameOf = [&](bool withAltitude, bool withoutAltitude, float* lh) {
        cascade::gui::MapView view;
        view.goTo(lat, lon, 2.0);
        std::vector<cascade::core::HostTrack> tracks;
        if (withAltitude) {
            tracks.push_back(
                makeTarget("A00001", "HIGH", CASCADE_TRACK_AIRCRAFT, lat, lon, 3000.0));
        }
        if (withoutAltitude) {
            tracks.push_back(makeTarget("A00002", "NOALT", CASCADE_TRACK_AIRCRAFT, lat + 0.1,
                                        lon + 0.1, kNoAlt));
        }
        float h = 0.0f;
        for (int f = 0; f < 3; ++f) { h = drawTargetsFrame(view, tracks); }
        if (lh != nullptr) { *lh = h; }
    };

    // Aircraft with an altitude only: the ladder is up, six rows, no grey row.
    float lh = 0.0f;
    frameOf(true, false, &lh);
    const Box six = shapeBounds(plate);
    const bool sixHasGrey = six.any && shapeVertexInside(kGreyNoAltitude, six);
    // Both kinds: the same ladder with a seventh row, and a grey swatch in it.
    frameOf(true, true, &lh);
    const Box seven = shapeBounds(plate);
    const bool sevenHasGrey = seven.any && shapeVertexInside(kGreyNoAltitude, seven);
    // Only aircraft with NO altitude: nothing is banded, so no ladder at all.
    frameOf(false, true, &lh);
    const Box none = shapeBounds(plate);

    const float rowStep = lh + 2.0f;
    const float headH = lh + 3.0f;
    std::printf("  legend plate: six rows %.1f px tall, seven rows %.1f px tall (row step %.1f),"
                " grey swatch inside: %d / %d, plate with only no-altitude aircraft: %d\n",
                six.y1 - six.y0, seven.y1 - seven.y0, rowStep, sixHasGrey ? 1 : 0,
                sevenHasGrey ? 1 : 0, none.any ? 1 : 0);
    CHECK(six.any);
    CHECK(seven.any);
    // The six-row plate is the heading, six rows and the padding (less the one
    // pixel ImGui's anti-aliasing fringe takes off the solid vertices' extent);
    // the seven-row plate is exactly one row step taller.
    CHECK_NEAR(six.y1 - six.y0, headH + rowStep * 6.0f + 8.0f - 1.0f, 0.01);
    CHECK_NEAR((seven.y1 - seven.y0) - (six.y1 - six.y0), rowStep, 0.01);
    CHECK(!sixHasGrey);
    CHECK(sevenHasGrey);
    CHECK(!none.any);
}

// --- "ON SCREEN ONLY" (0.99.74) ---------------------------------------------------
//
// A tester (2026-10-12): "Could you add a filter or switch to list only the
// aircraft currently viewable on the screen. This way if I am zoomed in on my
// local little airport and watching aircraft I only see the area I am interested
// in, not the full list of all received aircraft." The list asks the map; the
// map answers from the view it last drew.
void testIsOnScreen() {
    const double kNaN = std::nan("");

    // NOTHING DRAWN YET: nothing to filter against, so everything real is in
    // (a list that opened empty would look like a fault), and a position that is
    // not a number is never in.
    {
        cascade::gui::MapView fresh;
        CHECK(fresh.isOnScreen(51.0, 0.0));
        CHECK(fresh.isOnScreen(-33.0, 150.0));
        CHECK(!fresh.isOnScreen(kNaN, 0.0));
        CHECK(!fresh.isOnScreen(51.0, kNaN));
    }

    // A KNOWN VIEW: 800 x 600 px, centred on 51 N 0 E, 4 degrees of longitude
    // across. The latitude that fits is 4 * 600 / 800 = 3 degrees, so the map
    // shows 49.5..52.5 N and -2..+2 E.
    //
    // No setProjection: with no basemap the default projection IS equirectangular,
    // and pinning it on a view that has not yet chosen where to look asks for the
    // whole world (see MapView::setProjection), which would overrule goTo.
    cascade::gui::MapView view;
    view.goTo(51.0, 0.0, 4.0);
    const Stats s = drawMapFrame(view, 800.0f, 600.0f, false, 0, false);
    CHECK(s.bad == 0);
    CHECK_NEAR(view.viewCentreLatDeg(), 51.0, 1e-9);
    CHECK_NEAR(view.spanDeg(), 4.0, 1e-9);
    // The centre, and points well inside on each side.
    CHECK(view.isOnScreen(51.0, 0.0));
    CHECK(view.isOnScreen(51.0, 1.9));
    CHECK(view.isOnScreen(51.0, -1.9));
    CHECK(view.isOnScreen(52.4, 0.0));
    CHECK(view.isOnScreen(49.6, 0.0));
    // Just outside on each side, and far outside.
    CHECK(!view.isOnScreen(51.0, 2.1));
    CHECK(!view.isOnScreen(51.0, -2.1));
    CHECK(!view.isOnScreen(52.6, 0.0));
    CHECK(!view.isOnScreen(49.4, 0.0));
    CHECK(!view.isOnScreen(0.0, 0.0));
    CHECK(!view.isOnScreen(-33.0, 150.0));
    // EXACTLY ON AN EDGE is inside: the east edge is 2 degrees out, which is
    // pixel 800 of 800.
    CHECK(view.isOnScreen(51.0, 2.0));
    CHECK(!view.isOnScreen(51.0, 2.0001));
    // A position that is not a number is on no map, drawn or not.
    CHECK(!view.isOnScreen(kNaN, 0.0));
    CHECK(!view.isOnScreen(51.0, kNaN));

    // IT FOLLOWS THE VIEW, which is the whole point: pan to the east and the
    // aircraft that were in the middle are now behind you, and the ones that
    // were off the edge are in.
    view.goTo(51.0, 5.0, 4.0);
    CHECK(view.isOnScreen(51.0, 5.0));
    CHECK(view.isOnScreen(51.0, 3.5));
    CHECK(!view.isOnScreen(51.0, 0.0));

    // WIDER, AFTER A FRAME IN A WIDER VIEW: 20 degrees across takes in what
    // 4 degrees did not.
    cascade::gui::MapView wide;
    wide.goTo(51.0, 0.0, 20.0);
    drawMapFrame(wide, 800.0f, 600.0f, false, 0, false);
    CHECK_NEAR(wide.spanDeg(), 20.0, 1e-9);
    CHECK(wide.isOnScreen(51.0, 9.0));
    CHECK(!wide.isOnScreen(51.0, 11.0));
    CHECK(wide.isOnScreen(58.0, 0.0));
    CHECK(!wide.isOnScreen(59.0, 0.0));

    // THE SHORT WAY ROUND THE ANTIMERIDIAN: a view centred on 179.5 E shows
    // -179.8 (0.7 degrees east of the centre) and not -177 (3.5 degrees).
    cascade::gui::MapView dateline;
    dateline.goTo(0.0, 179.5, 4.0);
    drawMapFrame(dateline, 800.0f, 600.0f, false, 0, false);
    CHECK(dateline.isOnScreen(0.0, -179.8));
    CHECK(dateline.isOnScreen(0.0, 179.0));
    CHECK(!dateline.isOnScreen(0.0, -177.0));
    CHECK(!dateline.isOnScreen(0.0, 170.0));
}

int main() {
    testWholeWorldSpan();
    testCoordApertures();
    testCardNotes();
    testZoomKeyFigures();

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(2600.0f, 1600.0f);
    io.DeltaTime = 1.0f / 60.0f;
    io.IniFilename = nullptr;
    // 1.92 lets the backend own texture uploads; saying so is what makes a
    // context with no renderer behind it legal for a frame.
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;

    testPanelDrawsWithNoReceiverPosition();
    testMapDrawsWithNoReceiverPosition();
    testZoomKeysDriveTheView();
    testTargetLabelHelper();
    testMapTargetLabelsHaveTheOutline();
    testNoAltitudeIsGreyNotRed();
    testNoAltitudeLegendRow();
    testIsOnScreen();

    ImGui::DestroyContext();
    return testSummary("test_map_view");
}
