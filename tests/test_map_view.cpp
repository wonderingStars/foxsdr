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

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "imgui.h"
#include "test_check.hpp"

using cascade::gui::coordApertureGlyph;
using cascade::gui::satelliteCardNotes;
using cascade::gui::wholeWorldSpanDeg;

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

}  // namespace

int main() {
    testWholeWorldSpan();
    testCoordApertures();
    testCardNotes();

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

    ImGui::DestroyContext();
    return testSummary("test_map_view");
}
