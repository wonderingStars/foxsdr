// The phase-3 stubs: five contracts drawn as well-coloured panels with a cap that names them and the words "phase 3".
// What is tested is what phase 1 promises: each one's rectangle (the well's ground inside a line border, rounded), its cap in
// its top band and "phase 3" in the middle, and - the point of a contract - that EVERY property the contract lists can be set
// from C++ and read back (the specimen hands each one through a property of its root, so this file compiling is the check
// that the names and types are right), and that a tooltip rides on each like on every interactive component.
// Text is in a fallback typeface on this machine: it is asserted as INK in a region, never as glyph shapes or widths.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "render_harness.hpp"
#include "test_check.hpp"

#include "stubs_specimen.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <vector>

using namespace foxsdr::slinttest;
using std::chrono::milliseconds;

namespace {

constexpr int kW = 740, kH = 350;

const Rgb kBg = rgb(0x15171b);
const Rgb kWell = rgb(0x0d0f12);
const Rgb kLine = rgb(0x2b2f37);
const Rgb kMenu = rgb(0x262a32);

struct Rect { int x, y, w, h; const char* name; };
const Rect kInstrument{10, 10, 220, 150, "InstrumentFace"};
const Rect kMap{250, 10, 220, 150, "MapView"};
const Rect kGlobe{490, 10, 240, 150, "GlobeView"};
const Rect kScope{360, 180, 370, 160, "ScopeTube"};

// The right-most column holding ink in the rows [y0, y1) of [x0, x1) - how wide the text there is, whatever the typeface.
int inkRight(const Frame& f, int x0, int y0, int x1, int y1, Rgb bg) {
    int right = -1;
    for (int x = x0; x < x1; ++x) {
        if (f.countInk(x, y0, x + 1, y1, bg, 30) > 0) { right = x; }
    }
    return right;
}

TrackPoint track(int id, float lat, float lon, float heading, const char* label) {
    TrackPoint t;
    t.id = id;
    t.lat = lat;
    t.lon = lon;
    t.heading = heading;
    t.label = label;
    return t;
}

void checkPanel(const Frame& f, const Rect& r) {
    std::printf("  %s\n", r.name);
    // the geometry: the well's ground inside a 1 px line, edge by edge, and the sheet's own ground just outside
    CHECK(f.near(r.x + r.w / 2, r.y, kLine));                       // top edge
    CHECK(f.near(r.x + r.w / 2, r.y + r.h - 1, kLine));             // bottom edge
    CHECK(f.near(r.x, r.y + r.h / 2, kLine));                       // left edge
    CHECK(f.near(r.x + r.w - 1, r.y + r.h / 2, kLine));             // right edge
    CHECK(f.near(r.x + r.w / 2, r.y - 1, kBg) && f.near(r.x + r.w / 2, r.y + r.h, kBg));
    CHECK(f.near(r.x - 1, r.y + r.h / 2, kBg) && f.near(r.x + r.w, r.y + r.h / 2, kBg));
    CHECK(f.near(r.x + 1, r.y + r.h / 2, kWell) && f.near(r.x + r.w - 2, r.y + r.h / 2, kWell));   // the ground, right inside it
    CHECK(f.near(r.x + r.w / 2, r.y + 1, kWell) && f.near(r.x + r.w / 2, r.y + r.h - 2, kWell));
    CHECK(f.near(r.x + 14, r.y + r.h - 14, kWell) && f.near(r.x + r.w - 14, r.y + r.h - 14, kWell));
    CHECK(f.near(r.x, r.y, kBg, 10) && f.near(r.x + 1, r.y + 1, kBg, 10));   // the corners are cut round (radius 8): the sheet shows there
    CHECK(f.near(r.x + r.w - 1, r.y + r.h - 1, kBg, 10) && f.near(r.x + r.w - 2, r.y + r.h - 2, kBg, 10));
    // the cap in the top band (x 10, y 8), and nothing between it and the middle of the panel
    CHECK(f.countInk(r.x + 8, r.y + 6, r.x + r.w - 8, r.y + 22, kWell) > 15);
    CHECK(f.countInk(r.x + 4, r.y + 26, r.x + r.w - 4, r.y + r.h / 2 - 12, kWell) == 0);
    CHECK(f.countInk(r.x + 9, r.y + 1, r.x + r.w - 9, r.y + 5, kWell) == 0);
    // "phase 3", centred in the panel, in the dim of the sheet (0x94 is the blue of Theme.dim)
    const int cx = r.x + r.w / 2, cy = r.y + r.h / 2;
    CHECK(f.countInk(cx - 30, cy - 8, cx + 30, cy + 8, kWell) > 12);
    int brightest = 0;
    for (int y = cy - 8; y < cy + 8; ++y) { for (int x = cx - 30; x < cx + 30; ++x) { brightest = std::max(brightest, int(f.at(x, y).b)); } }
    CHECK(brightest > 0x50 && brightest <= 0x94 + 8);
    CHECK(f.countInk(r.x + 4, cy + 12, r.x + r.w - 4, r.y + r.h - 4, kWell) == 0);
    // the ink of "phase 3" is left of the right half and right of the left half: it is centred
    CHECK(inkRight(f, r.x + 2, cy - 8, r.x + r.w - 2, cy + 8, kWell) > cx && inkRight(f, r.x + 2, cy - 8, cx, cy + 8, kWell) > r.x + 20);
}

}  // namespace

int main() {
    installHeadlessPlatform();
    auto ui = StubsSpecimen::create();
    slint::Window& win = ui->window();

    Frame f = render(ui, kW, kH);
    CHECK(f.width == kW && f.height == kH);
    for (const Rect& r : {kInstrument, kMap, kGlobe, kScope}) { checkPanel(f, r); }
    // the five do not touch: the sheet's ground runs between them
    CHECK(f.near(240, 80, kBg) && f.near(480, 80, kBg) && f.near(120, 170, kBg) && f.near(350, 260, kBg));
    f.savePng(shotDir() + "stubs-specimen.png");

    // ---- the caps name them: "INSTRUMENT · <kind>" grows with the kind, the others are one word ----
    const int capNoKind = [&] {
        ui->set_face_kind("");
        Frame g = render(ui, kW, kH);
        return inkRight(g, kInstrument.x + 8, kInstrument.y + 6, kInstrument.x + kInstrument.w - 8, kInstrument.y + 22, kWell);
    }();
    const int capKind = [&] {
        ui->set_face_kind("S METER");
        Frame g = render(ui, kW, kH);
        return inkRight(g, kInstrument.x + 8, kInstrument.y + 6, kInstrument.x + kInstrument.w - 8, kInstrument.y + 22, kWell);
    }();
    CHECK(capNoKind > 0 && capKind > capNoKind + 30);                       // " · S METER" is 10 more characters
    Frame caps = render(ui, kW, kH);
    const int capMap = inkRight(caps, kMap.x + 8, kMap.y + 6, kMap.x + kMap.w - 8, kMap.y + 22, kWell) - kMap.x;
    const int capGlobe = inkRight(caps, kGlobe.x + 8, kGlobe.y + 6, kGlobe.x + kGlobe.w - 8, kGlobe.y + 22, kWell) - kGlobe.x;
    const int capScope = inkRight(caps, kScope.x + 8, kScope.y + 6, kScope.x + kScope.w - 8, kScope.y + 22, kWell) - kScope.x;
    CHECK(capMap > 10 && capMap < 70 && capGlobe > capMap && capScope > capMap);   // MAP is the shortest word
    CHECK(capGlobe < 90 && capScope < 90);
    // the cap's ink is the sheet's muted caption colour, brighter than "phase 3"
    {
        int brightest = 0;
        for (int y = kMap.y + 6; y < kMap.y + 22; ++y) { for (int x = kMap.x + 8; x < kMap.x + 60; ++x) { brightest = std::max(brightest, int(caps.at(x, y).b)); } }
        CHECK(brightest > 0x94 + 6 && brightest <= 0xad + 8);               // Theme.muted is 0x9aa3ad
    }

    // ---- every property of the contract can be set from C++ and read back ----
    // InstrumentFace: kind, caption, value, min, max, unit, needle
    ui->set_face_kind("RADIO METER");
    ui->set_face_caption("S METER");
    ui->set_face_value(0.5f);
    ui->set_face_min(-120.0f);
    ui->set_face_max(0.0f);
    ui->set_face_unit("dBm");
    ui->set_face_needle(0.25f);
    CHECK(ui->get_face_kind() == "RADIO METER" && ui->get_face_caption() == "S METER" && ui->get_face_unit() == "dBm");
    CHECK(ui->get_face_value() == 0.5f && ui->get_face_min() == -120.0f && ui->get_face_max() == 0.0f && ui->get_face_needle() == 0.25f);
    // MapView: lat, lon, zoom, tracks, followed
    ui->set_map_lat(51.5f);
    ui->set_map_lon(-0.12f);
    ui->set_map_zoom(8.0f);
    ui->set_map_tracks(std::make_shared<slint::VectorModel<TrackPoint>>(std::vector<TrackPoint>{
        track(11, 51.4f, -0.1f, 90.0f, "BAW123"), track(12, 51.6f, -0.2f, 270.0f, "RYR9XY")}));
    ui->set_map_followed(12);
    CHECK(ui->get_map_lat() == 51.5f && ui->get_map_lon() == -0.12f && ui->get_map_zoom() == 8.0f && ui->get_map_followed() == 12);
    CHECK(ui->get_map_tracks()->row_count() == 2);
    {
        const auto t = ui->get_map_tracks()->row_data(1);
        CHECK(t.has_value() && t->id == 12 && t->heading == 270.0f && t->label == "RYR9XY" && t->lat == 51.6f && t->lon == -0.2f);
    }
    // GlobeView: lat, lon, tracks, terminator
    ui->set_globe_lat(-33.9f);
    ui->set_globe_lon(151.2f);
    ui->set_globe_tracks(std::make_shared<slint::VectorModel<TrackPoint>>(std::vector<TrackPoint>{track(21, 0.0f, 0.0f, 0.0f, "ISS")}));
    ui->set_globe_terminator(true);
    CHECK(ui->get_globe_lat() == -33.9f && ui->get_globe_lon() == 151.2f && ui->get_globe_terminator());
    CHECK(ui->get_globe_tracks()->row_count() == 1 && ui->get_globe_tracks()->row_data(0)->label == "ISS");
    // ScopeTube: trace, caption, persistence
    {
        slint::SharedPixelBuffer<slint::Rgba8Pixel> trace(64, 32);
        for (auto* p = trace.begin(); p != trace.end(); ++p) { *p = slint::Rgba8Pixel{0, 255, 0, 255}; }
        ui->set_scope_trace(slint::Image(trace));
    }
    ui->set_scope_caption("CH1");
    ui->set_scope_persistence(0.8f);
    CHECK(ui->get_scope_trace().size().width == 64 && ui->get_scope_trace().size().height == 32);
    CHECK(ui->get_scope_caption() == "CH1" && ui->get_scope_persistence() == 0.8f);
    // the callbacks have the contract's signatures: the specimen's handlers compiled against them, nothing has fired by itself,
    // and each, called the way a page will call it, reaches its handler with its arguments
    CHECK(ui->get_map_target() == 0 && ui->get_map_zoom_notches() == 0);
    ui->invoke_fire_target(12);
    CHECK(ui->get_map_target() == 12);
    ui->invoke_fire_pan(0.25f, -0.5f);
    CHECK(ui->get_map_pan_dx() == 0.25f && ui->get_map_pan_dy() == -0.5f);
    ui->invoke_fire_zoom(-2);
    ui->invoke_fire_zoom(3);
    CHECK(ui->get_map_zoom_notches() == 1);

    // a stub with every property set is still a panel: the same rectangles, the same caps
    Frame set = render(ui, kW, kH);
    for (const Rect& r : {kInstrument, kMap, kGlobe, kScope}) { checkPanel(set, r); }
    set.savePng(shotDir() + "stubs-specimen-set.png");

    // ---- the tooltip every stub carries (the contract's "interactive" rule): shown under the stub after the pointer rests ----
    ui->set_stub_tooltip("Phase 3 draws this");
    moveTo(win, 120, 80);
    settle(milliseconds(750));
    Frame tip = render(ui, kW, kH);
    CHECK(tip.countNear(kInstrument.x, kInstrument.y + kInstrument.h + 6, kInstrument.x + 140, kInstrument.y + kInstrument.h + 18, kMenu, 4) > 300);
    moveTo(win, 700, 346);                                                                // off every stub
    settle(milliseconds(60));
    Frame gone = render(ui, kW, kH);
    CHECK(gone.countNear(kInstrument.x, kInstrument.y + kInstrument.h + 6, kInstrument.x + 140, kInstrument.y + kInstrument.h + 18, kMenu, 4) == 0);
    // a disabled stub shows none
    ui->set_stub_enabled(false);
    moveTo(win, 120, 80);
    settle(milliseconds(750));
    Frame off = render(ui, kW, kH);
    CHECK(off.countNear(kInstrument.x, kInstrument.y + kInstrument.h + 6, kInstrument.x + 140, kInstrument.y + kInstrument.h + 18, kMenu, 4) == 0);
    ui->set_stub_enabled(true);

    return testSummary("test_slint_stubs");
}
