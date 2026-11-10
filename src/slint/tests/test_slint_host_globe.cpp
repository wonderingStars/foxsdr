// The globe views' rules (src/slint/globe_logic.*, solar_ephemeris.*; docs 6.65): no Slint, no window, no engine. Held, section by section:
//   A. which plugin offers which view, the labels, the camera (drag, zoom, clamps)
//   B. the words: kilometres, bearings, latitude and longitude; the bands and their colours; a locator found in text
//   C. the HF propagation globe: the plugin's panel read by what its cells hold, the home from the plugin's locator or the receiver, the target path, the beacons on air,
//      the band filter, the hop counts and colours, the chips
//   D. the FT8 globe: the plugin's line, the message's sender and locator (CQ with modifiers, two calls, reports, RR73), the store (one per call and band, the window, the
//      band filter, the cap) and the summary
//   E. the GOES disk: the fixed-grid navigation against the published inverse equations (GOES-R PUG, as printed at makersportal.com), the limb, the frame, the look angle
//   F. the solar system: Standish's elements against JPL Horizons (2026-11-10 00:00 UT) for all seven planets and the Moon, the Mars opposition of January 2025, the
//      scales, the orbit rings, the panel reader and the check of the host's figures against the plugin's
//   G. the satellite readout and the orbit classes
//   H. what each view draws (globe_draw.*): the baked earth and GOES surfaces, and the five draw lists
//   J. what a view remembers (the framework's one setting: the camera on its elevation scale, the options, other views' entries left alone)
//   I. the picture (globe_compositor.*, on the 3D framework's renderer): the turn of the globe, the silhouette, the texture's orientation, depth hiding, lines, the orbits
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "globe_compositor.hpp"
#include "globe_draw.hpp"
#include "globe_logic.hpp"
#include "test_check.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace foxsdr::globe;
namespace sol = foxsdr::solar;

namespace {

PanelRowText cells(std::initializer_list<const char*> c) {
    PanelRowText r;
    for (const char* s : c) { r.cells.push_back(s); }
    return r;
}
PanelRowText heading(const char* h) {
    PanelRowText r;
    r.kind = 1;
    r.cells.push_back(h);
    return r;
}
PanelRowText rule() {
    PanelRowText r;
    r.kind = 2;
    return r;
}

// ------------------------------------------------------------------------------------------------------------------------
void sectionViews() {
    CHECK(viewKindFor("Propagation Tools") == ViewKind::Propagation);
    CHECK(viewKindFor("FT8") == ViewKind::Ft8);
    CHECK(viewKindFor("GOES HRIT / LRIT") == ViewKind::Goes);
    CHECK(viewKindFor("Solar System View") == ViewKind::SolarSystem);
    CHECK(viewKindFor("Satellites") == ViewKind::Satellites);
    CHECK(viewKindFor("ADS-B") == ViewKind::None);
    CHECK(viewKindFor("") == ViewKind::None);
    CHECK(viewKindFor("ft8") == ViewKind::None);   // exact names: a plugin called something else gets no globe
    CHECK(std::string(flatViewLabel(ViewKind::SolarSystem)) == "PAGES");
    CHECK(std::string(globeViewLabel(ViewKind::SolarSystem)) == "3D ORBITS");
    CHECK(std::string(flatViewLabel(ViewKind::Ft8)) == "LIST");
    CHECK(std::string(flatViewLabel(ViewKind::Goes)) == "FLAT");
    CHECK(std::string(flatViewLabel(ViewKind::Satellites)) == "FLAT MAP");
    CHECK(std::string(globeViewLabel(ViewKind::Goes)) == "3D GLOBE");
    CHECK(std::string(flatViewLabel(ViewKind::None)).empty());

    // the camera: a drag right turns the globe so the centre goes WEST (the surface follows the pointer), a drag down shows more of the north
    GlobeCamera c;
    c.centreLonDeg = 10.0;
    c.centreLatDeg = 0.0;
    c.zoom = 1.0;
    dragCamera(c, 90.0, 0.0, 400.0);       // radius 180 px: 90 px is 28.65 degrees
    CHECK_NEAR(c.centreLonDeg, 10.0 - 28.6479, 1e-3);
    CHECK_NEAR(c.centreLatDeg, 0.0, 1e-9);
    dragCamera(c, 0.0, 90.0, 400.0);
    CHECK_NEAR(c.centreLatDeg, 28.6479, 1e-3);
    dragCamera(c, 0.0, 100000.0, 400.0);
    CHECK_NEAR(c.centreLatDeg, 85.0, 1e-9);                  // held short of the pole
    dragCamera(c, -100000.0, 0.0, 400.0);
    CHECK(c.centreLonDeg > -180.0 && c.centreLonDeg <= 180.0);   // wrapped
    const GlobeCamera before = c;
    dragCamera(c, std::nan(""), 5.0, 400.0);
    dragCamera(c, 5.0, 5.0, 0.0);
    CHECK(c.centreLonDeg == before.centreLonDeg && c.centreLatDeg == before.centreLatDeg);   // nonsense moves nothing
    // zoomed in, the same drag turns less
    GlobeCamera z;
    z.centreLonDeg = 0.0;
    z.zoom = 2.0;
    dragCamera(z, 90.0, 0.0, 400.0);
    CHECK_NEAR(z.centreLonDeg, -14.3239, 1e-3);
    // the zoom: a step is x1.25, held to its range
    GlobeCamera zz;
    zoomCamera(zz, 1.0);
    CHECK_NEAR(zz.zoom, 1.25, 1e-12);
    zoomCamera(zz, -2.0);
    CHECK_NEAR(zz.zoom, 0.8, 1e-12);
    zoomCamera(zz, 100.0);
    CHECK_NEAR(zz.zoom, kZoomMax, 1e-12);
    zoomCamera(zz, -100.0);
    CHECK_NEAR(zz.zoom, kZoomMin, 1e-12);
    zoomCamera(zz, std::nan(""));
    CHECK_NEAR(zz.zoom, kZoomMin, 1e-12);
    // the Solar System View's plate keys drive the 3D camera
    {
        ViewState v;
        v.spinDeg = 20.0; v.tiltDeg = 55.0; v.camera.zoom = 1.0;
        CHECK(applySolarPlateKey(v, "ROTATE RIGHT") && v.spinDeg == 30.0 && v.cameraSet);
        CHECK(applySolarPlateKey(v, "rotate left") && v.spinDeg == 20.0);
        for (int i = 0; i < 3; ++i) { applySolarPlateKey(v, "ROTATE LEFT"); }
        CHECK(v.spinDeg == 350.0);                                          // the turn wraps below zero
        for (int i = 0; i < 2; ++i) { applySolarPlateKey(v, "ROTATE RIGHT"); }
        CHECK(v.spinDeg == 10.0);                                           // and above 360
        CHECK(applySolarPlateKey(v, "TILT") && v.tiltDeg == 65.0);
        CHECK(applySolarPlateKey(v, "TILT") && v.tiltDeg == 75.0);
        CHECK(applySolarPlateKey(v, "TILT") && v.tiltDeg == 85.0);
        CHECK(applySolarPlateKey(v, "TILT") && v.tiltDeg == 10.0);          // past the top it starts again at the lowest
        CHECK(applySolarPlateKey(v, "ZOOM IN") && std::fabs(v.camera.zoom - 1.25) < 1e-12);
        CHECK(applySolarPlateKey(v, "ZOOM OUT") && applySolarPlateKey(v, "ZOOM OUT") && std::fabs(v.camera.zoom - 0.8) < 1e-12);
        for (int i = 0; i < 40; ++i) { applySolarPlateKey(v, "ZOOM IN"); }
        CHECK(v.camera.zoom == kZoomMax);
        ViewState fresh;
        for (const char* other : {"NEXT PAGE", "PREVIOUS PAGE", "NEXT CHANNEL", "", "ROTATE", "TILT ", "rotate  left"}) {
            CHECK(!applySolarPlateKey(fresh, other));
        }
        CHECK(!fresh.cameraSet && fresh.spinDeg == ViewState{}.spinDeg && fresh.tiltDeg == ViewState{}.tiltDeg);   // the plugin's own keys move nothing here
    }
    CHECK_NEAR(defaultCamera(ViewKind::Goes).centreLonDeg, -75.2, 1e-12);
    CHECK_NEAR(defaultCamera(ViewKind::Goes).centreLatDeg, 0.0, 1e-12);
    CHECK_NEAR(defaultCamera(ViewKind::Propagation).zoom, 1.0, 1e-12);
}

// ------------------------------------------------------------------------------------------------------------------------
void sectionWords() {
    CHECK(kmText(5570.2) == "5 570 km");
    CHECK(kmText(16994.0) == "16 994 km");
    CHECK(kmText(999.4) == "999 km");
    CHECK(kmText(0.0) == "0 km");
    CHECK(kmText(1234567.0) == "1 234 567 km");
    CHECK(kmText(-1.0) == "-");
    CHECK(kmText(std::nan("")) == "-");
    CHECK(bearingText(288.3) == "288\xC2\xB0");
    CHECK(bearingText(359.6) == "0\xC2\xB0");
    CHECK(bearingText(-90.0) == "270\xC2\xB0");
    CHECK(bearingText(std::nan("")) == "-");
    CHECK(latLonText(LatLon{51.5074, -0.1278}) == "51.51 N 0.13 W");
    CHECK(latLonText(LatLon{-33.8688, 151.2093}) == "33.87 S 151.21 E");

    // the bands
    CHECK(std::string(bandOfMhz(14.074)->name) == "20 m");
    CHECK(std::string(bandOfMhz(7.074)->name) == "40 m");
    CHECK(std::string(bandOfMhz(21.074)->name) == "15 m");
    CHECK(std::string(bandOfMhz(28.074)->name) == "10 m");
    CHECK(std::string(bandOfMhz(1.84)->name) == "160 m");
    CHECK(std::string(bandOfMhz(50.313)->name) == "6 m");
    CHECK(std::string(bandOfMhz(70.154)->name) == "4 m");
    CHECK(std::string(bandOfMhz(144.174)->name) == "2 m");
    CHECK(std::string(bandOfMhz(10.136)->name) == "30 m");
    CHECK(std::string(bandOfMhz(18.100)->name) == "17 m");
    CHECK(std::string(bandOfMhz(24.915)->name) == "12 m");
    CHECK(std::string(bandOfMhz(3.573)->name) == "80 m");
    CHECK(bandOfMhz(100.0) == nullptr);
    CHECK(bandOfMhz(0.0) == nullptr);
    CHECK(bandOfMhz(15.0) == nullptr);
    CHECK(bandOfMhz(14.1)->id == 6);
    CHECK(bandById(6) == bandOfMhz(14.0));
    CHECK(bandById(0) == nullptr && bandById(99) == nullptr);
    CHECK(bandCount() == 13);
    // the artboards' colours: 20 m orange, 40 m cyan, 15 m pink, 10 m green; every band has its own and the hop legend follows the other artboard
    CHECK(bandColour(6) == 0xFFF5A524u && bandColour(4) == 0xFF6FD3FFu && bandColour(8) == 0xFFFF6B9Du && bandColour(10) == 0xFF5FD28Au);
    for (int a = 1; a <= bandCount(); ++a) {
        for (int b = a + 1; b <= bandCount(); ++b) { CHECK(bandColour(a) != bandColour(b)); }
    }
    CHECK(hopColour(1) == 0xFFF5A524u && hopColour(2) == 0xFF6FD3FFu && hopColour(3) == 0xFFFF6B9Du && hopColour(4) == hopColour(7));

    // a locator in text
    CHECK(locatorIn("IO91wm") == "IO91wm");
    CHECK(locatorIn("IO91wm (the receiver)") == "IO91wm");
    CHECK(locatorIn("not set") == "");
    CHECK(locatorIn("") == "");
    CHECK(locatorIn("-") == "");
    CHECK(locatorIn("for the VOACAP page") == "");
    CHECK(locatorIn("set io91 here") == "io91");
    CHECK(locatorIn("ZZ99") == "");              // outside the grid
    CHECK(locatorIn("IO91w") == "");             // five characters is not a locator
    CHECK(locatorIn("5 570 km 288") == "");
}

// ------------------------------------------------------------------------------------------------------------------------
std::vector<PanelRowText> propagationPanel() {
    return {heading("Status"),
            cells({"Figures by", "EMPIRICAL", "no VOACAP engine"}),
            cells({"Locator", "IO91wm", "setting", ""}),
            cells({"Target", "FN42", "for the VOACAP page", ""}),
            rule(),
            heading("Beacons on air now (NCDXF/IARU)"),
            cells({"14.100 MHz", "4U1UN New York City", "12 s left"}),
            cells({"18.110 MHz", "VK6RBP Rolystone", "12 s left"}),
            cells({"21.150 MHz", "ZL6B Masterton", "12 s left"}),
            cells({"24.930 MHz", "ZS6DN Pretoria", "12 s left"}),
            cells({"28.200 MHz", "NOCALL Nowhere", "12 s left"}),
            heading("Meteors (IMO)"),
            cells({"Active now", "Quadrantids", "ZHR 80"})};
}

void sectionPropagation() {
    const PropagationFacts f = readPropagationPanel(propagationPanel());
    CHECK(f.panelSeen);
    CHECK(f.homeLocator == "IO91wm");
    CHECK(f.targetLocator == "FN42");
    CHECK(f.onAir.size() == 4u);                        // the fifth row names a call that is no beacon: not invented
    CHECK(std::string(f.onAir[0].beacon->call) == "4U1UN");
    CHECK_NEAR(f.onAir[0].freqMhz, 14.1, 1e-12);
    CHECK(f.onAir[0].secondsLeft == 12);
    CHECK(std::string(f.onAir[3].beacon->place) == "Pretoria");
    // rows of another block are not beacons: a beacon's call under the Meteors heading does not count
    std::vector<PanelRowText> other = propagationPanel();
    other.push_back(cells({"14.100 MHz", "4U1UN New York City", "9 s left"}));
    CHECK(readPropagationPanel(other).onAir.size() == 4u);
    // a separator closes the block
    std::vector<PanelRowText> cut = propagationPanel();
    cut.insert(cut.begin() + 7, rule());
    CHECK(readPropagationPanel(cut).onAir.size() == 1u);
    // not set: no locator, nothing invented
    std::vector<PanelRowText> unset = {cells({"Locator", "not set", "no setting, no receiver position"}), cells({"Target", "not set", ""})};
    const PropagationFacts u = readPropagationPanel(unset);
    CHECK(u.panelSeen && u.homeLocator.empty() && u.targetLocator.empty() && u.onAir.empty());
    CHECK(!readPropagationPanel({}).panelSeen);
    CHECK(!readPropagationPanel({cells({"Something", "else"})}).panelSeen);

    // every beacon in the table is placeable and the call finds it
    for (int i = 0; i < kBeaconCount; ++i) {
        const Beacon& b = beaconAt(i);
        LatLon p;
        CHECK(locatorToLatLon(b.grid, p));
        CHECK(beaconByCall(b.call) == &b);
        CHECK(beaconByCall(std::string(b.call) + "x") == nullptr);
    }
    CHECK(beaconByCall("4u1un") != nullptr);
    CHECK(std::string(beaconAt(0).call) == "4U1UN" && std::string(beaconAt(17).call) == "YV5B");
    // spot checks of the table against the places: Cape-side positions within a grid square of the town
    LatLon ny, perth, nz;
    CHECK(locatorToLatLon(beaconByCall("4U1UN")->grid, ny));
    CHECK_NEAR(ny.lat, 40.7, 0.3);
    CHECK_NEAR(ny.lon, -74.0, 0.3);
    CHECK(locatorToLatLon(beaconByCall("VK6RBP")->grid, perth));
    CHECK_NEAR(perth.lat, -32.1, 0.3);
    CHECK_NEAR(perth.lon, 116.0, 0.3);
    CHECK(locatorToLatLon(beaconByCall("ZL6B")->grid, nz));
    CHECK_NEAR(nz.lat, -40.9, 0.3);
    CHECK_NEAR(nz.lon, 175.6, 0.3);

    // the scene: home is the plugin's locator, the target path first, the beacons after in the panel's order
    const LatLon rx{10.0, 10.0};
    const PropagationScene s = buildPropagationScene(f, true, rx, 0);
    CHECK(s.homeKnown);
    CHECK_NEAR(s.home.lat, 51.5208333, 1e-6);          // IO91wm's centre, not the receiver
    CHECK(s.homeSource == "the plugin's locator");
    CHECK(s.paths.size() == 5u);
    CHECK(s.paths[0].label == "TARGET FN42" && !s.paths[0].beacon);
    CHECK(s.paths[1].label == "4U1UN New York City" && s.paths[1].beacon);
    CHECK(s.beaconsListed == 4);
    CHECK(s.markers.size() == 1u + 1u + 4u);
    CHECK(s.markers[0].shape == 1);                     // the receiver is the triangle
    // distances and hop counts are the geometry's: London - New York is 5 570 km = 2 hops, to Perth about 14 500 km = 4 hops
    CHECK_NEAR(s.paths[1].distKm, 5570.0, 25.0);
    CHECK(s.paths[1].hops == 2 && s.paths[1].argb == 0xFF6FD3FFu);
    CHECK(s.paths[2].hops >= 4 && s.paths[2].argb == hopColour(4));
    CHECK_NEAR(s.paths[1].bearingDeg, 288.0, 1.0);
    CHECK(s.paths[1].bandId == 6 && s.paths[2].bandId == 7 && s.paths[3].bandId == 8 && s.paths[4].bandId == 9);
    CHECK(pathChipText(s.paths[1]).find("4U1UN New York City 5 5") == 0);
    CHECK(pathChipText(s.paths[1]).find("288\xC2\xB0") != std::string::npos);
    // the band filter shows the beacons of that band only, the target stays
    const PropagationScene s20 = buildPropagationScene(f, true, rx, 6);
    CHECK(s20.paths.size() == 2u && s20.paths[1].label.rfind("4U1UN", 0) == 0);
    CHECK(s20.beaconsListed == 4);
    CHECK(buildPropagationScene(f, true, rx, 1).paths.size() == 1u);   // 160 m: nobody on air
    // without the plugin's locator the receiver is home; without either, an honest empty scene
    PropagationFacts noLoc = f;
    noLoc.homeLocator.clear();
    const PropagationScene sr = buildPropagationScene(noLoc, true, rx, 0);
    CHECK(sr.homeKnown && sr.home.lat == 10.0 && sr.homeSource == "the receiver position");
    const PropagationScene sn = buildPropagationScene(noLoc, false, rx, 0);
    CHECK(!sn.homeKnown && sn.paths.empty() && sn.markers.empty() && sn.homeSource.empty());
    // a target at home is no path; a bad locator is no target
    PropagationFacts same = f;
    same.targetLocator = "IO91wm";
    CHECK(buildPropagationScene(same, true, rx, 0).paths.size() == 4u);
    PropagationFacts bad = f;
    bad.targetLocator = "ZZ99";
    CHECK(buildPropagationScene(bad, true, rx, 0).paths.size() == 4u);
    // a beacon with no frequency in the panel shows only when no band is chosen
    PropagationFacts nofreq;
    nofreq.homeLocator = "IO91wm";
    nofreq.onAir.push_back({&beaconAt(0), 0.0, -1});
    CHECK(buildPropagationScene(nofreq, false, rx, 0).paths.size() == 1u);
    CHECK(buildPropagationScene(nofreq, false, rx, 6).paths.empty());
}

// ------------------------------------------------------------------------------------------------------------------------
void sectionFt8() {
    Ft8Decode d;
    CHECK(parseFt8Line("120015 -12  0.3 1243 ~  CQ DX K1ABC FN42", d));
    CHECK(d.hour == 12 && d.minute == 0 && d.second == 15 && d.snr == -12 && d.hz == 1243 && d.message == "CQ DX K1ABC FN42");
    CHECK_NEAR(d.dt, 0.3, 1e-9);
    CHECK(parseFt8Line("235945  +2 -0.2  756 ~  W9XYZ G4ABC R-14", d));
    CHECK(d.hour == 23 && d.snr == 2 && d.hz == 756 && d.dt < 0.0 && d.message == "W9XYZ G4ABC R-14");
    CHECK(parseFt8Line("120015   0  0.1  300 ~  CQ K1ABC FN42", d) && d.snr == 0);
    CHECK(!parseFt8Line("FT8: listening. Messages appear at the end of each 15 s UTC slot", d));
    CHECK(!parseFt8Line("FT8: listening again", d));
    CHECK(!parseFt8Line("", d));
    CHECK(!parseFt8Line("120015 -12  0.3 1243 ~  ", d));                    // no message
    CHECK(!parseFt8Line("250015 -12  0.3 1243 ~  CQ K1ABC FN42", d));       // 25:00:15 is no time
    CHECK(!parseFt8Line("12001 -12  0.3 1243 ~  CQ K1ABC FN42", d));        // the time is six digits
    CHECK(!parseFt8Line("120015 xx  0.3 1243 ~  CQ K1ABC FN42", d));
    CHECK(!parseFt8Line("120015 -12  0.3 1243 CQ K1ABC FN42", d));          // the plugin's tilde is the mark of a decode

    // the wall-clock time of a decode: the most recent such time of day, not more than a minute ahead
    {
        Ft8Decode t;
        CHECK(parseFt8Line("235945 -12  0.3 1243 ~  CQ K1ABC FN42", t));
        const std::int64_t day = 86400000LL, base = 20000LL * day;       // some midnight
        CHECK(ft8LineTimeMs(t, base + 12 * 3600000LL) == base - 15000);   // 23:59:45 read at noon is yesterday's
        CHECK(ft8LineTimeMs(t, base + 23 * 3600000LL + 59 * 60000LL + 50000) == base + 86400000LL - 15000);   // read five seconds later: today's
        CHECK(ft8LineTimeMs(t, base + 5000) == base - 15000);             // read just after midnight
        Ft8Decode u;
        CHECK(parseFt8Line("120015 -12  0.3 1243 ~  CQ K1ABC FN42", u));
        CHECK(ft8LineTimeMs(u, base + 12 * 3600000LL + 20000) == base + 12 * 3600000LL + 15000);
        CHECK(ft8LineTimeMs(u, base + 12 * 3600000LL + 10000) == base + 12 * 3600000LL + 15000);   // the clock is a few seconds behind the decoder: slack of a minute
        CHECK(ft8LineTimeMs(u, base + 12 * 3600000LL - 120000) == base - 86400000LL + 12 * 3600000LL + 15000);   // two minutes ahead: yesterday's
    }
    Ft8Heard h;
    CHECK(ft8Heard("CQ DX K1ABC FN42", h) && h.call == "K1ABC" && h.locator == "FN42");
    CHECK(ft8Heard("CQ K1ABC FN42", h) && h.call == "K1ABC");
    CHECK(ft8Heard("CQ POTA W9XYZ EN52", h) && h.call == "W9XYZ" && h.locator == "EN52");
    CHECK(ft8Heard("cq ja1xyz pm95", h) && h.call == "JA1XYZ" && h.locator == "PM95");
    CHECK(ft8Heard("G4ABC JA1XYZ PM95", h) && h.call == "JA1XYZ" && h.locator == "PM95");   // TO FROM LOC: the sender is the second
    CHECK(ft8Heard("CQ EA8/DL1ABC JL18", h) && h.call == "EA8/DL1ABC");
    CHECK(!ft8Heard("G4ABC JA1XYZ -09", h));                 // a report carries no locator
    CHECK(!ft8Heard("W9XYZ G4ABC R-14", h));
    CHECK(!ft8Heard("G4ABC JA1XYZ RR73", h));                // RR73 is shaped like a locator and is not one
    CHECK(!ft8Heard("G4ABC JA1XYZ 73", h));
    CHECK(!ft8Heard("CQ K1ABC", h));                         // no locator at all
    CHECK(!ft8Heard("CQ DX", h));
    CHECK(!ft8Heard("<...> G4ABC IO91", h));                 // a hashed call cannot be named
    CHECK(!ft8Heard("TNX BOB 73 GL", h));                    // free text
    CHECK(!ft8Heard("", h));
    CHECK(looksLikeCallsign("K1ABC") && looksLikeCallsign("G4ABC") && looksLikeCallsign("EA8/DL1ABC") && looksLikeCallsign("JA1XYZ"));
    CHECK(!looksLikeCallsign("CQ") && !looksLikeCallsign("FN42") && !looksLikeCallsign("RR73") && !looksLikeCallsign("-09") && !looksLikeCallsign("ABCDE") &&
          !looksLikeCallsign("12345") && !looksLikeCallsign("<...>"));

    // the store: London receives
    const LatLon rx{51.5208, -0.125};
    Ft8SpotStore st;
    const std::int64_t t0 = 1000000000LL;
    CHECK(st.addLine("120015 -12  0.3 1243 ~  CQ DX K1ABC FN42", t0, 14.074, rx));
    CHECK(st.addLine("120030  -7  0.1 1812 ~  G4ABC JA1XYZ PM95", t0 + 15000, 14.074, rx));
    CHECK(st.addLine("120045  -3  0.1  900 ~  CQ ZL1ABC RF73", t0 + 30000, 7.074, rx));
    CHECK(!st.addLine("120100  -3  0.1  900 ~  G4ABC JA1XYZ -09", t0 + 45000, 14.074, rx));
    CHECK(!st.addLine("FT8: listening again", t0 + 46000, 14.074, rx));
    CHECK(st.size() == 3u);
    const std::vector<Ft8Spot> all = st.visible(t0 + 60000, 15LL * 60LL * 1000LL, 0);
    CHECK(all.size() == 3u);
    CHECK(all[0].call == "K1ABC" && all[0].bandId == 6 && all[0].locator == "FN42");
    CHECK_NEAR(all[0].km, 5200.0, 400.0);                    // London to New England
    CHECK(all[2].call == "ZL1ABC" && all[2].bandId == 4);
    CHECK(all[2].km > 18000.0);                              // London to Auckland: the far side of the world
    CHECK(all[0].atMs < all[1].atMs && all[1].atMs < all[2].atMs);
    // the band filter, the window
    CHECK(st.visible(t0 + 60000, 15LL * 60LL * 1000LL, 6).size() == 2u);
    CHECK(st.visible(t0 + 60000, 15LL * 60LL * 1000LL, 4).size() == 1u);
    CHECK(st.visible(t0 + 60000, 15LL * 60LL * 1000LL, 8).empty());
    CHECK(st.visible(t0 + 15LL * 60LL * 1000LL + 20000, 15LL * 60LL * 1000LL, 0).size() == 1u);   // the first two are older than 15 min, the third is not
    CHECK(st.visible(t0 + 16LL * 60LL * 1000LL, 60LL * 60LL * 1000LL, 0).size() == 3u);
    // one per call and band: a newer decode replaces, another band is another entry
    CHECK(st.addLine("120100  -1  0.1 1243 ~  CQ K1ABC FN42", t0 + 70000, 14.074, rx));
    CHECK(st.size() == 3u);
    CHECK(st.addLine("120115  -1  0.1 1243 ~  CQ K1ABC FN42", t0 + 80000, 21.074, rx));
    CHECK(st.size() == 4u);
    // the summary: stations (calls), the furthest, the bands
    const Ft8Summary sum = summariseSpots(st.visible(t0 + 90000, 15LL * 60LL * 1000LL, 0));
    CHECK(sum.stations == 3);
    CHECK(sum.hasFurthest && sum.furthest.call == "ZL1ABC");
    CHECK(sum.bands == 3);
    CHECK(!summariseSpots({}).hasFurthest);
    CHECK(summariseSpots({}).stations == 0);
    // 24 hours old is gone for good
    CHECK(st.visible(t0 + 25LL * 3600LL * 1000LL, 48LL * 3600LL * 1000LL, 0).empty());
    CHECK(st.size() == 0u);
    // a dial outside every band is band 0: shown with no filter only
    Ft8SpotStore off;
    CHECK(off.addLine("120015 -12  0.3 1243 ~  CQ K1ABC FN42", t0, 15.0, rx));
    CHECK(off.visible(t0, 1000, 0).size() == 1u && off.visible(t0, 1000, 6).empty());
    // the cap
    Ft8SpotStore cap;
    for (std::size_t i = 0; i < Ft8SpotStore::kCap + 50u; ++i) {
        Ft8Spot s;
        s.atMs = t0 + static_cast<std::int64_t>(i);
        s.call = "K" + std::to_string(i) + "AB";
        s.bandId = 6;
        cap.add(s);
    }
    CHECK(cap.size() == Ft8SpotStore::kCap);
    const auto kept = cap.visible(t0 + 100000, 1000000, 0);
    CHECK(kept.front().atMs == t0 + 50);                     // the oldest 50 went
}

// ------------------------------------------------------------------------------------------------------------------------
// the published inverse of the fixed-grid navigation (GOES-R PUG, as printed at makersportal.com: req 6378137 m, rpol 6356752.31414 m, H 42164160 m)
void inverseGoes(double x, double y, double lon0, double& latDeg, double& lonDeg) {
    const double req = 6378137.0, rpol = 6356752.31414, H = 42164160.0;
    const double a = std::sin(x) * std::sin(x) + std::cos(x) * std::cos(x) * (std::cos(y) * std::cos(y) + req * req / (rpol * rpol) * std::sin(y) * std::sin(y));
    const double b = -2.0 * H * std::cos(x) * std::cos(y);
    const double c = H * H - req * req;
    const double rs = (-b - std::sqrt(b * b - 4.0 * a * c)) / (2.0 * a);
    const double sx = rs * std::cos(x) * std::cos(y), sy = -rs * std::sin(x), sz = rs * std::cos(x) * std::sin(y);
    latDeg = std::atan(req * req / (rpol * rpol) * sz / std::sqrt((H - sx) * (H - sx) + sy * sy)) * kRadToDeg;
    lonDeg = lon0 - std::atan(sy / (H - sx)) * kRadToDeg;
}

void sectionGoes() {
    CHECK(std::string(goesSatellite(0).name) == "GOES-East");
    CHECK_NEAR(goesSatellite(0).lonDeg, -75.2, 1e-12);
    CHECK(std::string(goesSatellite(1).name) == "GOES-West");
    CHECK_NEAR(goesSatellite(1).lonDeg, -137.0, 1e-12);
    CHECK_NEAR(goesSatellite(-5).lonDeg, -75.2, 1e-12);          // clamped
    CHECK_NEAR(goesSatellite(9).lonDeg, -137.0, 1e-12);

    // the sub-satellite point is the middle of the picture
    double u = 0, v = 0, x = 1, y = 1;
    CHECK(goesPicturePoint(LatLon{0.0, -75.2}, -75.2, u, v));
    CHECK_NEAR(u, 0.5, 1e-9);
    CHECK_NEAR(v, 0.5, 1e-9);
    CHECK(goesScanAngle(LatLon{0.0, -75.2}, -75.2, x, y));
    CHECK_NEAR(x, 0.0, 1e-12);
    CHECK_NEAR(y, 0.0, 1e-12);
    // east is to the right and north is up
    CHECK(goesPicturePoint(LatLon{0.0, -60.0}, -75.2, u, v));
    CHECK(u > 0.5 && std::fabs(v - 0.5) < 1e-9);
    CHECK(goesPicturePoint(LatLon{40.0, -75.2}, -75.2, u, v));
    CHECK(v < 0.5 && std::fabs(u - 0.5) < 1e-9);
    CHECK(goesPicturePoint(LatLon{0.0, -90.0}, -75.2, u, v));
    CHECK(u < 0.5);
    // the forward navigation against the PUG's inverse: any point of the visible disc comes back to itself
    int worst = 0, n = 0;
    double worstErr = 0.0;
    for (int la = -80; la <= 80; la += 10) {
        for (int dlon = -80; dlon <= 80; dlon += 10) {
            const LatLon g{static_cast<double>(la), -75.0 + dlon};
            double xr = 0, yr = 0;
            if (!goesScanAngle(g, -75.0, xr, yr)) { continue; }
            double lat2 = 0, lon2 = 0;
            inverseGoes(xr, yr, -75.0, lat2, lon2);
            ++n;
            const double err = std::max(std::fabs(lat2 - g.lat), std::fabs(lon2 - g.lon));
            worstErr = std::max(worstErr, err);
            if (err > 1e-6) { ++worst; std::printf("  goes mismatch %d,%d -> %.6f,%.6f (err %.3g)\n", la, dlon, lat2, lon2, err); }
        }
    }
    CHECK(n > 120);
    CHECK(worst == 0);
    CHECK(worstErr < 1e-6);
    // the limb: a point just inside it is visible, one beyond is not, and the equatorial limb sits at the frame's edge (the frame is the disc's bounding square)
    CHECK(goesScanAngle(LatLon{0.0, -75.0 + 80.0}, -75.0, x, y));
    CHECK(!goesScanAngle(LatLon{0.0, -75.0 + 82.0}, -75.0, x, y));
    CHECK(!goesScanAngle(LatLon{85.0, -75.0}, -75.0, x, y));
    CHECK(!goesScanAngle(LatLon{0.0, 105.0}, -75.0, x, y));    // the far side
    CHECK(goesPicturePoint(LatLon{0.0, -75.0 + 81.2}, -75.0, u, v));
    CHECK(u > 0.995 && u <= 1.0);
    CHECK(!goesPicturePoint(LatLon{0.0, -75.0 + 81.9}, -75.0, u, v));
    // the pole of the disc is nearer than the equator's limb: the vertical radius is a little smaller (the Earth is flat at the poles)
    CHECK(goesPicturePoint(LatLon{80.0, -75.0}, -75.0, u, v));
    CHECK(v > 0.0 && v < 0.5);
    // the longitude of the spacecraft moves the picture: New York is east of centre for GOES-West and west of it for none
    CHECK(goesPicturePoint(LatLon{40.7, -74.0}, -137.0, u, v));
    CHECK(u > 0.6);
    CHECK(goesPicturePoint(LatLon{40.7, -74.0}, -75.2, u, v));
    CHECK(std::fabs(u - 0.5) < 0.02);
    // from a place on the far side of the Earth (Moscow, 37.6 E) the spacecraft is below the horizon
    const LookAngle lon = goesLook(LatLon{55.75, 37.6}, -75.2);
    CHECK(!lon.visible && lon.elevationDeg < 0.0);
    // from New York it is high in the south-east
    const LookAngle ny = goesLook(LatLon{40.7, -74.0}, -75.2);
    CHECK(ny.visible && ny.elevationDeg > 40.0 && ny.elevationDeg < 50.0);
    CHECK(ny.azimuthDeg > 175.0 && ny.azimuthDeg < 185.0);
}

// ------------------------------------------------------------------------------------------------------------------------
struct HorizonsRow {
    sol::Planet planet;
    const char* name;
    double r, delta, elong;   // JPL Horizons, geocentric observer table, 2026-11-10 00:00 UT: r (Sun distance), delta (Earth distance), S-O-T
    bool evening;             // /T (trailing: east of the Sun, the evening side) or /L (leading: west)
};

void sectionSolar() {
    const double jd = julianDay(2026, 11, 10, 0, 0, 0.0);
    CHECK_NEAR(jd, 2461354.5, 1e-9);
    // JPL Horizons (ssd.jpl.nasa.gov/api/horizons.api, COMMAND 199..899 and 301, CENTER 500@399, QUANTITIES 19 20 23 10; retrieved 2026-11-10)
    const HorizonsRow rows[] = {
        {sol::Planet::Mercury, "Mercury", 0.307536910796, 0.73328180662064, 11.3539, false},
        {sol::Planet::Venus, "Venus", 0.721983323506, 0.30768946719722, 24.5394, false},
        {sol::Planet::Mars, "Mars", 1.603792875833, 1.35815589285222, 84.5885, false},
        {sol::Planet::Jupiter, "Jupiter", 5.319743306238, 5.36135035672005, 82.2817, false},
        {sol::Planet::Saturn, "Saturn", 9.422509855134, 8.63131082362156, 141.0914, true},
        {sol::Planet::Uranus, "Uranus", 19.43403710938, 18.4833519530985, 163.2666, false},
        {sol::Planet::Neptune, "Neptune", 29.87710528973, 29.1771155832593, 134.2940, true},
    };
    double worstR = 0, worstD = 0, worstE = 0;
    for (const HorizonsRow& h : rows) {
        const sol::PlanetFacts f = sol::planetFacts(h.planet, jd);
        worstR = std::max(worstR, std::fabs(f.sunDistanceAu - h.r));
        worstD = std::max(worstD, std::fabs(f.earthDistanceAu - h.delta));
        worstE = std::max(worstE, std::fabs(f.elongationDeg - h.elong));
        CHECK_NEAR(f.sunDistanceAu, h.r, 0.006);
        CHECK_NEAR(f.earthDistanceAu, h.delta, 0.006);
        CHECK_NEAR(f.elongationDeg, h.elong, 0.15);
        CHECK(f.evening == h.evening);
        CHECK(std::string(sol::planetName(h.planet)) == h.name);
    }
    std::printf("  ephemeris vs Horizons 2026-11-10: worst r %.5f AU, delta %.5f AU, elongation %.3f deg\n", worstR, worstD, worstE);
    // the Mars opposition (Horizons, 2025-01-12 00:00 UT: delta 0.64231191259244 AU, S-O-T 172.8075, leading = the morning side)
    const sol::PlanetFacts mars = sol::planetFacts(sol::Planet::Mars, julianDay(2025, 1, 12, 0, 0, 0.0));
    CHECK_NEAR(mars.earthDistanceAu, 0.642312, 0.003);
    CHECK_NEAR(mars.elongationDeg, 172.81, 0.3);
    CHECK(!mars.evening);
    // after the opposition Mars is on the evening side (Horizons: 2025-01-13 S-O-T 173.8834 /L - still leading, its opposition is Jan 16)
    CHECK(!sol::planetFacts(sol::Planet::Mars, julianDay(2025, 1, 13, 0, 0, 0.0)).evening);
    CHECK(sol::planetFacts(sol::Planet::Mars, julianDay(2025, 2, 10, 0, 0, 0.0)).evening);
    // the Earth: perihelion 4 January 2025 at 0.98333 AU, aphelion 3 July 2025 at 1.01667 AU (the almanac's figures; Standish's EM barycentre is within 0.0002)
    CHECK_NEAR(sol::earthSunDistanceAu(julianDay(2025, 1, 4, 13, 28, 0.0)), 0.98333, 0.0004);
    CHECK_NEAR(sol::earthSunDistanceAu(julianDay(2025, 7, 3, 19, 54, 0.0)), 1.01667, 0.0004);
    CHECK_NEAR(sol::earthSunDistanceAu(jd), 0.99048142, 0.0003);        // Horizons, Earth from the Sun's centre, 2026-11-10 00:00 UT

    // the Moon (Horizons 301: delta 0.00267233043600 AU = 399 775 km, illuminated 0.68044 %, S-O-T 9.4332 trailing = evening)
    const sol::MoonFacts moon = sol::moonFacts(jd);
    CHECK_NEAR(moon.distanceKm, 0.00267233043600 * 149597870.7, 1500.0);
    CHECK_NEAR(moon.elongationDeg, 9.4332, 0.15);
    CHECK_NEAR(moon.illuminatedFraction * 100.0, 0.68044, 0.3);
    CHECK(moon.evening);
    std::printf("  moon vs Horizons: distance %.0f km (Horizons %.0f), elongation %.3f deg (9.433), lit %.3f %% (0.680)\n", moon.distanceKm,
                0.00267233043600 * 149597870.7, moon.elongationDeg, moon.illuminatedFraction * 100.0);
    // a full moon is nearly 100 % lit at 180 deg elongation (the new moon was on 2026-11-09, so the full one falls about 14.8 days on: found by scanning the hours),
    // a quarter moon is about half lit
    double bestLit = 0.0;
    for (int h = 0; h < 4 * 24; ++h) { bestLit = std::max(bestLit, sol::moonFacts(julianDay(2026, 11, 22, 0, 0, 0.0) + h / 24.0).illuminatedFraction); }
    CHECK(bestLit > 0.995);
    const sol::MoonFacts quarter = sol::moonFacts(julianDay(2026, 11, 17, 0, 0, 0.0));
    CHECK(quarter.illuminatedFraction > 0.3 && quarter.illuminatedFraction < 0.7 && quarter.evening);

    // the orbit rings: closed, in the plane of the planet's elements, at the right sizes; the Earth's a circle of about 1 AU
    for (int i = 0; i < sol::kPlanetCount; ++i) {
        const auto ring = sol::orbitRing(static_cast<sol::Planet>(i), jd, 120);
        CHECK(ring.size() == 121u);
        CHECK_NEAR(ring.front().x, ring.back().x, 1e-12);
        double rmin = 1e9, rmax = 0;
        for (const auto& p : ring) {
            const double r = std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
            rmin = std::min(rmin, r);
            rmax = std::max(rmax, r);
        }
        const sol::PlanetFacts f = sol::planetFacts(static_cast<sol::Planet>(i), jd);
        CHECK(f.sunDistanceAu >= rmin * 0.99 && f.sunDistanceAu <= rmax * 1.01);   // the planet is on its ring
    }
    const auto er = sol::earthOrbitRing(jd, 120);
    double emin = 9, emax = 0;
    for (const auto& p : er) { const double r = std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z); emin = std::min(emin, r); emax = std::max(emax, r); }
    CHECK_NEAR(emin, 0.9833, 0.001);
    CHECK_NEAR(emax, 1.0167, 0.001);
    CHECK(sol::orbitRing(sol::Planet::Mars, jd, 3).size() == 17u);      // a minimum of 16 samples
    CHECK(std::string(sol::planetName(7)).empty() && std::string(sol::planetName(-1)).empty());

    // the scales: linear is r / rmax, log is the plugin's knee form; both put 0 at 0 and rmax at 1, and the log keeps Mercury off the Sun
    CHECK_NEAR(sol::displayRadius(15.5, 31.0, false), 0.5, 1e-12);
    CHECK_NEAR(sol::displayRadius(31.0, 31.0, true), 1.0, 1e-12);
    CHECK_NEAR(sol::displayRadius(31.0, 31.0, false), 1.0, 1e-12);
    CHECK_NEAR(sol::displayRadius(0.0, 31.0, true), 0.0, 1e-12);
    CHECK_NEAR(sol::displayRadius(-1.0, 31.0, true), 0.0, 1e-12);
    CHECK_NEAR(sol::displayRadius(1.0, 31.0, true), std::log1p(10.0) / std::log1p(310.0), 1e-12);
    CHECK(sol::displayRadius(0.39, 31.0, true) > 10.0 * sol::displayRadius(0.39, 31.0, false));
    CHECK(sol::displayRadius(5.0, 31.0, true) < sol::displayRadius(10.0, 31.0, true));   // monotonic
    const sol::Xyz dp = sol::displayPosition({3.0, 4.0, 0.0}, 31.0, false);
    CHECK_NEAR(dp.x, 3.0 / 31.0, 1e-12);
    CHECK_NEAR(dp.y, 4.0 / 31.0, 1e-12);
    const sol::Xyz zero = sol::displayPosition({0, 0, 0}, 31.0, true);
    CHECK(zero.x == 0.0 && zero.y == 0.0 && zero.z == 0.0);

    // the scene
    const OrbitScene sc = buildOrbitScene(jd, true, true);
    CHECK(sc.rings.size() == 8u && sc.bodies.size() == 7u);
    CHECK(sc.bodies[2].name == "Mars");
    CHECK(std::sqrt(sc.bodies[6].pos.x * sc.bodies[6].pos.x + sc.bodies[6].pos.y * sc.bodies[6].pos.y) < 1.0);
    for (const BodyView& b : sc.bodies) {
        const double r = std::sqrt(b.pos.x * b.pos.x + b.pos.y * b.pos.y + b.pos.z * b.pos.z);
        CHECK(r > 0.0 && r < 1.0);
    }
    // the Moon's ring marker is a short way from the Earth, in the Moon's direction
    const double dm = std::sqrt((sc.moonDisplay.x - sc.earth.x) * (sc.moonDisplay.x - sc.earth.x) + (sc.moonDisplay.y - sc.earth.y) * (sc.moonDisplay.y - sc.earth.y));
    CHECK_NEAR(dm, 0.05, 1e-9);
    CHECK(planetChipText(sc.bodies[2]) == "Mars 1.36 AU 85\xC2\xB0 W");
    CHECK(planetChipText(sc.bodies[4]) == "Saturn 8.63 AU 141\xC2\xB0 E");
    // the date
    CHECK(dateText(jd) == "2026-11-10");
    CHECK(dateText(julianDay(2000, 1, 1, 12, 0, 0.0)) == "2000-01-01");
    CHECK(dateText(julianDay(2024, 2, 29, 6, 0, 0.0)) == "2024-02-29");
    CHECK(dateText(dateOfOffset(jd, 51)) == "2026-12-31");
    CHECK(dateText(dateOfOffset(jd, -10)) == "2026-10-31");
    CHECK_NEAR(dateOfOffset(jd, 100000), jd + 366, 1e-9);
    CHECK_NEAR(dateOfOffset(jd, -100000), jd - 366, 1e-9);

    // the panel: the plugin's strings ("%.3f AU", "elong %.0f %c", "r %.2f AU", "%.2f AU", "%.0f %% lit")
    const std::vector<PanelRowText> panel = {heading("Solar wind at L1 (NOAA/SWPC)"),
                                             cells({"Speed", "412 km/s", "", ""}),
                                             heading("Planets from the Earth"),
                                             cells({"Earth", "0.990 AU", "from the Sun", ""}),
                                             cells({"Mercury", "0.73 AU", "elong 11 W", "r 0.31 AU"}),
                                             cells({"Venus", "0.31 AU", "elong 25 W", "r 0.72 AU"}),
                                             cells({"Mars", "1.36 AU", "elong 85 W", "r 1.60 AU"}),
                                             cells({"Jupiter", "5.36 AU", "elong 82 W", "r 5.32 AU"}),
                                             cells({"Saturn", "8.63 AU", "elong 141 E", "r 9.42 AU"}),
                                             cells({"Uranus", "18.48 AU", "elong 163 W", "r 19.43 AU"}),
                                             cells({"Neptune", "29.18 AU", "elong 134 E", "r 29.88 AU"}),
                                             cells({"Moon", "399775 km", "1 % lit", ""}),
                                             heading("Feeds"),
                                             cells({"Mars", "not a planet row here", "", ""})};
    const SolarPanelFacts pf = readSolarPanel(panel);
    CHECK(pf.planets.size() == 7u);
    CHECK(pf.haveMoon && std::fabs(pf.moonKm - 399775.0) < 1.0 && pf.haveMoonLit && pf.moonLitPct == 1.0);
    CHECK(pf.haveEarthSun && std::fabs(pf.earthSunAu - 0.990) < 1e-9);
    CHECK(pf.planets[2].planet == 2 && pf.planets[2].haveEarthAu && std::fabs(pf.planets[2].earthAu - 1.36) < 1e-9);
    CHECK(pf.planets[2].haveElong && pf.planets[2].elongDeg == 85.0 && !pf.planets[2].evening);
    CHECK(pf.planets[4].evening && pf.planets[4].haveSunAu && std::fabs(pf.planets[4].sunAu - 9.42) < 1e-9);
    // the host's figures against the plugin's own for the same instant: every one within the panel's rounding
    const PanelCheck ck = checkAgainstPanel(pf, jd);
    CHECK(ck.ok);
    CHECK(ck.compared >= 21);
    CHECK(ck.worstAuError < 0.007 && ck.worstElongError < 0.8);
    // and a panel that disagrees is caught: Mars at 1.45 AU, or on the evening side, or Saturn at 150 deg
    SolarPanelFacts badAu = pf;
    badAu.planets[2].earthAu = 1.45;
    CHECK(!checkAgainstPanel(badAu, jd).ok);
    CHECK(checkAgainstPanel(badAu, jd).worst == "Mars AU");
    SolarPanelFacts badSide = pf;
    badSide.planets[2].evening = true;
    CHECK(!checkAgainstPanel(badSide, jd).ok);
    SolarPanelFacts badElong = pf;
    badElong.planets[4].elongDeg = 150.0;
    CHECK(!checkAgainstPanel(badElong, jd).ok);
    SolarPanelFacts badEarth = pf;
    badEarth.earthSunAu = 1.01;
    CHECK(!checkAgainstPanel(badEarth, jd).ok);
    // a panel with nothing in it compares nothing and fails nothing
    const PanelCheck none = checkAgainstPanel(SolarPanelFacts{}, jd);
    CHECK(none.compared == 0 && none.ok);
    CHECK(readSolarPanel({}).planets.empty());
    CHECK(readSolarPanel({heading("Planets from the Earth"), rule(), cells({"Mars", "1.36 AU", "elong 85 W", ""})}).planets.empty());   // a rule ends the block
}

// ------------------------------------------------------------------------------------------------------------------------
void sectionSatellites() {
    SatIn noaa;
    noaa.id = "33591";
    noaa.label = "NOAA 19";
    noaa.at = {58.2, -18.4};
    noaa.altKm = 870.0;
    noaa.hasAlt = true;
    const LatLon rx{51.5074, -0.1278};
    const SatReadout r = satReadout(noaa, true, rx);
    CHECK(r.valid && r.label == "NOAA 19" && r.hasLook);
    CHECK(r.look.slantRangeKm > 1000.0 && r.look.slantRangeKm < 3500.0);
    CHECK(r.look.elevationDeg > 0.0 && r.look.visible);
    CHECK_NEAR(r.footprintDeg, 28.4, 0.1);
    CHECK(r.look.azimuthDeg > 270.0 && r.look.azimuthDeg < 360.0);    // north-west of London
    // no altitude: the footprint and the look angle are not computed, not zero
    SatIn flat = noaa;
    flat.hasAlt = false;
    const SatReadout rf = satReadout(flat, true, rx);
    CHECK(rf.valid && !rf.hasLook && rf.footprintDeg == 0.0 && !rf.hasAlt);
    // no receiver position: no look angle
    CHECK(!satReadout(noaa, false, rx).hasLook);
    // the label falls back to the id
    SatIn nameless = noaa;
    nameless.label.clear();
    CHECK(satReadout(nameless, true, rx).label == "33591");
    CHECK(std::string(orbitClass(870.0)) == "LEO");
    CHECK(std::string(orbitClass(418.0)) == "LEO");
    CHECK(std::string(orbitClass(20200.0)) == "MEO");
    CHECK(std::string(orbitClass(35786.0)) == "GEO");
    CHECK(std::string(orbitClass(35500.0)) == "GEO");
    CHECK(std::string(orbitClass(42000.0)) == "HEO");
    CHECK(orbitClassColour(870.0) != orbitClassColour(35786.0));
    CHECK(orbitClassColour(870.0) == 0xFFF5A524u);
}

// ------------------------------------------------------------------------------------------------------------------------
std::uint32_t texel(const Texture& t, double lat, double lon) {
    const int c = std::clamp(static_cast<int>((normalizeLon(lon) + 180.0) * t.width / 360.0), 0, t.width - 1);
    const int r = std::clamp(static_cast<int>((90.0 - lat) * t.height / 180.0), 0, t.height - 1);
    return t.argb[static_cast<std::size_t>(r) * static_cast<std::size_t>(t.width) + static_cast<std::size_t>(c)];
}
int chan(std::uint32_t argb, int shift) { return static_cast<int>((argb >> shift) & 0xFFu); }
double angleDeg(const Vec3& a, const Vec3& b) { return std::acos(std::clamp(dot(normalized(a), normalized(b)), -1.0, 1.0)) * kRadToDeg; }

void sectionDraw() {
    const Palette& pal = palette();
    static const LandMask land(1440, 720);
    // ---- the earth bake ----
    Texture lit, shaded;
    bakeEarth(lit, land, nullptr, false);
    CHECK(lit.width == 1024 && lit.height == 512 && lit.argb.size() == 1024u * 512u);
    CHECK(texel(lit, 51.5, -0.1) == pal.landDay);              // London: land, lit (no sun given)
    CHECK(texel(lit, 45.0, -30.0) == pal.oceanDay);            // the North Atlantic
    CHECK(texel(lit, 0.0, -150.0) == pal.oceanDay);
    const SunPosition noon = sunPosition(julianDay(2025, 3, 20, 12, 0, 0.0));
    bakeEarth(shaded, land, &noon, false);
    CHECK_NEAR(noon.subLatDeg, 0.0, 3.0);
    const LatLon sub{noon.subLatDeg, noon.subLonDeg};
    CHECK(texel(shaded, sub.lat, sub.lon) == texel(lit, sub.lat, sub.lon));            // under the Sun: the day colour
    const double antiLon = normalizeLon(sub.lon + 180.0);
    CHECK(texel(shaded, -sub.lat, antiLon) == pal.oceanNight || texel(shaded, -sub.lat, antiLon) == pal.landNight);    // the antipode: the night colour
    // land and sea keep their own colours under the Sun
    int dayLand = 0, dayOcean = 0;
    for (int i = 0; i < 200; ++i) {
        // the texel's own centre: the bake asks the mask there
        const double lat0 = -60.0 + 120.0 * i / 200.0, lon0 = sub.lon - 40.0 + 80.0 * ((i * 37) % 200) / 200.0;
        const double lat = 90.0 - (std::floor((90.0 - lat0) * 512.0 / 180.0) + 0.5) * 180.0 / 512.0;
        const double lon = -180.0 + (std::floor((normalizeLon(lon0) + 180.0) * 1024.0 / 360.0) + 0.5) * 360.0 / 1024.0;
        if (land.land(lat, lon)) { ++dayLand; CHECK(texel(shaded, lat, lon) == pal.landDay); } else { ++dayOcean; CHECK(texel(shaded, lat, lon) == pal.oceanDay); }
    }
    CHECK(dayLand > 5 && dayOcean > 5);
    // the terminator is a smooth ramp: on the terminator the colour is half way between night and day
    const LatLon edge = fromVec(terminator(noon, 0.0, 360, 1.0)[90]);
    const std::uint32_t atEdge = texel(shaded, edge.lat, edge.lon);
    const bool edgeLand = land.land(edge.lat, edge.lon);
    CHECK(atEdge != (edgeLand ? pal.landDay : pal.oceanDay) && atEdge != (edgeLand ? pal.landNight : pal.oceanNight));
    // the greyline tint warms the band and leaves the rest alone
    Texture grey;
    bakeEarth(grey, land, &noon, true);
    CHECK(texel(grey, edge.lat, edge.lon) != texel(shaded, edge.lat, edge.lon));
    CHECK(texel(grey, sub.lat, sub.lon) == texel(shaded, sub.lat, sub.lon));
    CHECK(texel(grey, -sub.lat, antiLon) == texel(shaded, -sub.lat, antiLon));
    // the bake key: a quarter of a degree of the Sun's longitude moves it, a hundredth does not, the greyline flag and the size do
    SunPosition moved = noon;
    moved.subLonDeg += 0.01;
    CHECK(earthBakeKey(&noon, false, 1024, 512) == earthBakeKey(&moved, false, 1024, 512));
    moved.subLonDeg += 0.3;
    CHECK(earthBakeKey(&noon, false, 1024, 512) != earthBakeKey(&moved, false, 1024, 512));
    CHECK(earthBakeKey(&noon, false, 1024, 512) != earthBakeKey(&noon, true, 1024, 512));
    CHECK(earthBakeKey(&noon, false, 1024, 512) != earthBakeKey(&noon, false, 512, 256));
    CHECK(earthBakeKey(nullptr, false, 1024, 512) != earthBakeKey(&noon, false, 1024, 512));
    CHECK(shaded.key == earthBakeKey(&noon, false, 1024, 512));
    Texture small;
    bakeEarth(small, land, &noon, false, 1, 1);               // clamped to a usable size
    CHECK(small.width >= 16 && small.height >= 8);

    // ---- the GOES bake ----
    const int pw = 64, ph = 64;
    std::vector<std::uint8_t> white(static_cast<std::size_t>(pw) * ph, 255u);
    Texture goes;
    bakeGoes(goes, white, pw, ph, false, -75.2);
    CHECK(goes.width == 1024 && goes.height == 512);
    CHECK(chan(texel(goes, 0.0, -75.2), 16) > 200 && chan(texel(goes, 0.0, -75.2), 0) > 200);        // the middle of the disc is the picture's white
    CHECK(texel(goes, 0.0, -75.2 + 100.0) == pal.oceanDay);                                             // behind the limb: the plain ocean
    CHECK(texel(goes, 0.0, -75.2 - 100.0) == pal.oceanDay);
    CHECK(texel(goes, 88.0, -75.2) == pal.oceanDay);                                                    // beyond the frame at the pole
    // a black picture is nothing received: the ocean
    std::vector<std::uint8_t> black(static_cast<std::size_t>(pw) * ph, 0u);
    Texture goes0;
    bakeGoes(goes0, black, pw, ph, false, -75.2);
    CHECK(texel(goes0, 0.0, -75.2) == pal.oceanDay);
    // the picture's own geometry: a picture white only on its right half shows white east of the sub-satellite meridian and ocean to the west
    std::vector<std::uint8_t> half(static_cast<std::size_t>(pw) * ph, 0u);
    for (int y = 0; y < ph; ++y) { for (int x = pw / 2; x < pw; ++x) { half[static_cast<std::size_t>(y) * pw + x] = 255u; } }
    Texture goesH;
    bakeGoes(goesH, half, pw, ph, false, -75.2);
    CHECK(chan(texel(goesH, 0.0, -75.2 + 30.0), 16) > 200);
    CHECK(texel(goesH, 0.0, -75.2 - 30.0) == pal.oceanDay);
    // rgb: a red picture keeps its red
    std::vector<std::uint8_t> red(static_cast<std::size_t>(pw) * ph * 3u, 0u);
    for (std::size_t i = 0; i < red.size(); i += 3) { red[i] = 255u; }
    Texture goesR;
    bakeGoes(goesR, red, pw, ph, true, -75.2);
    CHECK(chan(texel(goesR, 0.0, -75.2), 16) > 200 && chan(texel(goesR, 0.0, -75.2), 8) < 100);
    // too few pixels, no size: the plain ocean, no crash
    Texture goesS;
    bakeGoes(goesS, std::vector<std::uint8_t>(10, 255u), pw, ph, false, -75.2);
    CHECK(texel(goesS, 0.0, -75.2) == pal.oceanDay);
    bakeGoes(goesS, white, 0, 0, false, -75.2);
    CHECK(texel(goesS, 0.0, -75.2) == pal.oceanDay);

    // ---- altitude lift, initial camera ----
    CHECK_NEAR(altitudeLift(870.0), 870.0 / kEarthRadiusKm, 1e-12);
    CHECK_NEAR(altitudeLift(35786.0), 0.25, 1e-12);
    CHECK(altitudeLift(0.0) == 0.0 && altitudeLift(-5.0) == 0.0 && altitudeLift(std::nan("")) == 0.0);
    CHECK_NEAR(initialCamera(ViewKind::Propagation, true, LatLon{51.5, -0.1}).centreLonDeg, -0.1, 1e-12);
    CHECK_NEAR(initialCamera(ViewKind::Propagation, false, LatLon{51.5, -0.1}).centreLatDeg, 20.0, 1e-12);
    CHECK_NEAR(initialCamera(ViewKind::Goes, true, LatLon{51.5, -0.1}).centreLonDeg, -75.2, 1e-12);
    CHECK_NEAR(initialCamera(ViewKind::Satellites, true, LatLon{89.9, 10.0}).centreLatDeg, 85.0, 1e-12);

    // ---- the propagation list ----
    const PropagationFacts pfacts = readPropagationPanel(propagationPanel());
    const PropagationScene pscene = buildPropagationScene(pfacts, true, LatLon{10, 10}, 0);
    ViewState view;
    const GlobeDrawList pl = drawPropagation(pscene, view, shaded);
    CHECK(pl.space == GlobeDrawList::Space::Globe && pl.sphere && pl.emptyNote.empty());
    CHECK_NEAR(pl.camera.centreLonDeg, pscene.home.lon, 1e-9);                       // untouched: centred on home
    const std::size_t grat = graticule(30.0, 3.0, 1.0).size();
    CHECK(pl.lines.size() == grat + pscene.paths.size());
    CHECK(pl.markers.size() == pscene.markers.size());
    CHECK(pl.markers[0].shape == MarkerShape::Triangle && pl.markers[0].label == "HOME");
    // a path's arc touches the ground at both ends and lifts to the shell between (300 km drawn 6x)
    const DrawLine& arc = pl.lines[grat + 1];
    CHECK(arc.argb == pscene.paths[1].argb);
    double rmax = 0.0;
    for (const Vec3& p : arc.points) { rmax = std::max(rmax, length(p)); }
    CHECK_NEAR(rmax, 1.0 + 300.0 * 6.0 / kEarthRadiusKm, 1e-3);
    CHECK_NEAR(length(arc.points.front()), 1.0, 1e-9);
    // a touched camera is kept
    ViewState touched;
    touched.cameraSet = true;
    touched.camera = GlobeCamera{33.0, 44.0, 2.0};
    CHECK_NEAR(drawPropagation(pscene, touched, shaded).camera.zoom, 2.0, 1e-12);
    // no home: an honest empty state and no paths
    const GlobeDrawList pe = drawPropagation(buildPropagationScene(PropagationFacts{}, false, LatLon{}, 0), view, shaded);
    CHECK(!pe.emptyNote.empty() && pe.lines.size() == grat && pe.markers.empty());

    // ---- the FT8 list ----
    std::vector<Ft8Spot> spots;
    Ft8Spot a;
    a.call = "K1ABC"; a.bandId = 6; a.at = LatLon{42.5, -71.0}; a.km = 5200.0;
    Ft8Spot b;
    b.call = "ZL1ABC"; b.bandId = 4; b.at = LatLon{-36.5, 175.0}; b.km = 18300.0;
    spots = {a, b};
    const LatLon rx{51.5208, -0.125};
    const GlobeDrawList fl = drawFt8(spots, true, rx, view, shaded);
    const std::size_t g30 = graticule(30.0, 3.0, 1.0).size();
    CHECK(fl.lines.size() == g30 + 2u);
    CHECK((fl.lines[g30].argb & 0x00FFFFFFu) == (bandColour(6) & 0x00FFFFFFu) && (fl.lines[g30].argb >> 24) < 0xFFu);   // band colour, a little translucent
    CHECK((fl.lines[g30 + 1].argb & 0x00FFFFFFu) == (bandColour(4) & 0x00FFFFFFu));
    CHECK(fl.markers.size() == 3u);                                                   // two stations and the receiver
    CHECK(fl.markers[1].label == "ZL1ABC" && fl.markers[0].label.empty());           // only the furthest is named
    CHECK(fl.markers[2].shape == MarkerShape::Triangle && fl.markers[2].label == "RX");
    CHECK(fl.emptyNote.empty());
    // a longer path lifts higher
    double ra = 0, rb = 0;
    for (const Vec3& p : fl.lines[g30].points) { ra = std::max(ra, length(p)); }
    for (const Vec3& p : fl.lines[g30 + 1].points) { rb = std::max(rb, length(p)); }
    CHECK(rb > ra && ra > 1.02);
    // nothing heard: the empty state; no receiver position: dots only and a note
    CHECK(!drawFt8({}, true, rx, view, shaded).emptyNote.empty());
    const GlobeDrawList fn = drawFt8(spots, false, rx, view, shaded);
    CHECK(fn.lines.size() == g30 && fn.markers.size() == 2u && !fn.emptyNote.empty());

    // ---- the GOES list ----
    const GlobeDrawList gl = drawGoes(view, -75.2, true, LatLon{40.7, -74.0}, goes);
    const std::size_t g15 = graticule(15.0, 3.0, 1.0).size();
    CHECK(gl.lines.size() == g15 + 1u);
    const DrawLine& limb = gl.lines.back();
    CHECK(limb.dashed && limb.closed && limb.argb == pal.limb);
    for (const Vec3& p : limb.points) { CHECK_NEAR(angleDeg(p, toUnit(0.0, -75.2)), footprintAngularRadiusDeg(kGoesAltitudeKm, 0.0), 1e-6); }
    CHECK(gl.markers.size() == 2u);
    CHECK(gl.markers[0].shape == MarkerShape::Square && gl.markers[0].label == "SUB-SAT 0.0 N 75.2 W");
    CHECK(gl.markers[1].label == "RX" && gl.markers[1].argb == 0xFFFF6B9Du);
    CHECK_NEAR(gl.camera.centreLonDeg, -75.2, 1e-12);
    CHECK(drawGoes(view, -137.0, false, LatLon{}, goes).markers.size() == 1u);
    CHECK(drawGoes(view, -137.0, false, LatLon{}, goes).markers[0].label == "SUB-SAT 0.0 N 137.0 W");

    // ---- the orbits list ----
    const double jd = julianDay(2026, 11, 10, 0, 0, 0.0);
    ViewState ov;
    ov.spinDeg = 33.0;
    ov.tiltDeg = 48.0;
    const GlobeDrawList ol = drawOrbits(buildOrbitScene(jd, true, true), ov);
    CHECK(ol.space == GlobeDrawList::Space::Orbits && !ol.sphere);
    CHECK_NEAR(ol.camera.centreLonDeg, 33.0, 1e-12);
    CHECK_NEAR(ol.camera.centreLatDeg, 48.0, 1e-12);
    CHECK(ol.lines.size() == 8u + 1u);                                              // seven planets' rings, the Earth's, the Moon's ring
    CHECK(ol.markers.size() == 1u + 7u + 1u + 1u);                                  // the Sun, seven planets, the Earth, the Moon
    CHECK(ol.markers[0].sizePx > 8.0f && ol.markers[0].argb == pal.sun);
    CHECK(ol.markers[3].label == "Mars");
    for (const DrawLine& l : ol.lines) { CHECK(!l.surface && l.closed); }
    const GlobeDrawList ol2 = drawOrbits(buildOrbitScene(jd, true, false), ov);
    CHECK(ol2.lines.size() == 8u && ol2.markers.size() == 9u);                       // no Moon: no marker and no ring
    // the scale switch moves the planets: Mercury is much further out on the log scale
    const GlobeDrawList lin = drawOrbits(buildOrbitScene(jd, false, true), ov);
    CHECK(length(ol.markers[1].at) > 3.0 * length(lin.markers[1].at));

    // ---- the satellite list ----
    SatSceneIn si;
    SatIn noaa;
    noaa.id = "33591"; noaa.label = "NOAA 19"; noaa.at = LatLon{58.2, -18.4}; noaa.altKm = 870.0; noaa.hasAlt = true;
    SatIn geo;
    geo.id = "41866"; geo.label = "GOES 16"; geo.at = LatLon{0.0, -75.2}; geo.altKm = 35786.0; geo.hasAlt = true;
    SatIn bare;
    bare.id = "x"; bare.at = LatLon{0.0, 0.0};
    si.sats = {noaa, geo, bare};
    SatPathIn track;
    track.id = "33591"; track.dashed = true;
    for (int i = 0; i < 20; ++i) { track.points.push_back(LatLon{-80.0 + 8.0 * i, -20.0 + i}); }
    si.paths = {track};
    si.selectedId = "33591";
    si.rxKnown = true;
    si.rx = rx;
    const GlobeDrawList sl = drawSatellites(si, view, shaded);
    // graticule + the ground track + the selected footprint + the slant line
    CHECK(sl.lines.size() == g30 + 3u);
    CHECK(sl.lines[g30].dashed && !sl.lines[g30].closed);
    CHECK(sl.lines[g30 + 1].closed && sl.lines[g30 + 1].argb == 0xFF6FD3FFu);
    CHECK(sl.lines[g30 + 2].dashed && sl.lines[g30 + 2].points.size() == 2u);
    CHECK(sl.markers.size() == 4u);                                                  // three satellites and the receiver
    CHECK(sl.markers[0].sizePx > sl.markers[1].sizePx);                              // the selected one is larger
    CHECK(sl.markers[0].argb == orbitClassColour(870.0) && sl.markers[1].argb == orbitClassColour(35786.0));
    CHECK(sl.markers[2].argb == pal.white);                                          // no altitude: no class colour
    CHECK_NEAR(length(sl.markers[0].at), 1.0 + altitudeLift(870.0) + 0.004, 1e-9);
    CHECK_NEAR(length(sl.markers[1].at), 1.25 + 0.004, 1e-9);                        // not to scale above 1 600 km
    CHECK(sl.markers[3].label == "RX");
    CHECK(sl.emptyNote.empty());
    // the layers switch off
    SatSceneIn off = si;
    off.footprints = false;
    off.groundTracks = false;
    CHECK(drawSatellites(off, view, shaded).lines.size() == g30 + 1u);              // graticule + the slant line
    // nothing tracked: the empty state; a selected satellite without an altitude has no footprint and no slant line
    CHECK(!drawSatellites(SatSceneIn{}, view, shaded).emptyNote.empty());
    SatSceneIn noAlt = si;
    noAlt.selectedId = "x";
    CHECK(drawSatellites(noAlt, view, shaded).lines.size() == g30 + 1u);             // graticule + the ground track only
    SatSceneIn norx = si;
    norx.rxKnown = false;
    CHECK(drawSatellites(norx, view, shaded).markers.size() == 3u);
}

// ------------------------------------------------------------------------------------------------------------------------
namespace s3 = foxsdr::scene3d;

const SunPosition& noonForTiming() {
    static const SunPosition sun = sunPosition(julianDay(2025, 3, 20, 12, 0, 0.0));
    return sun;
}

struct Picture {
    int w = 0, h = 0;
    std::vector<s3::Rgba> px;
    s3::Rgba at(int x, int y) const { return px[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)]; }
};
bool isBackground(const s3::Rgba& c) { return c.r == 11 && c.g == 13 && c.b == 18; }

s3::ScreenPoint projectOf(const GlobeCompositor& comp, const GlobeCamera& cam, const Vec3& v) {
    const Vec3 r = rotate(rotationFor(cam), v);
    return comp.scene().project(s3::Vec3{static_cast<float>(r.x), static_cast<float>(r.y), static_cast<float>(r.z)});
}

Picture paintList(GlobeCompositor& comp, const GlobeDrawList& list, int w, int h, std::vector<s3::Label3D>* labels = nullptr) {
    Picture p;
    p.w = w;
    p.h = h;
    p.px.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h), s3::Rgba{1, 2, 3, 4});
    std::vector<s3::Label3D> lab;
    CHECK(comp.paint(list, p.px.data(), w, h, lab));
    if (labels != nullptr) { *labels = lab; }
    return p;
}

void sectionCompositor() {
    // ---- the turn: the centre of the view goes to +Z, north goes up the picture, east goes right ----
    for (const LatLon& c : {LatLon{0, 0}, LatLon{51.5, -0.1}, LatLon{-40, 150}, LatLon{80, -120}, LatLon{-85, 30}, LatLon{10, 180}}) {
        GlobeCamera cam;
        cam.centreLatDeg = c.lat;
        cam.centreLonDeg = c.lon;
        const Rotation r = rotationFor(cam);
        const Vec3 centre = rotate(r, toUnit(c));
        CHECK_NEAR(centre.x, 0.0, 1e-12);
        CHECK_NEAR(centre.y, 0.0, 1e-12);
        CHECK_NEAR(centre.z, 1.0, 1e-12);
        const Vec3 north = rotate(r, toUnit(std::min(89.0, c.lat + 5.0), c.lon));
        CHECK(north.y > 0.0 && std::fabs(north.x) < 1e-9);                     // five degrees north is straight up the picture
        const Vec3 east = rotate(r, toUnit(c.lat, c.lon + 5.0));
        CHECK(east.x > 0.0);                                                    // five degrees east is to the right
        CHECK_NEAR(length(rotate(r, Vec3{0.3, -0.5, 0.7})), length(Vec3{0.3, -0.5, 0.7}), 1e-12);   // a turn keeps lengths
    }
    CHECK(toRgba(0xFF112233u).r == 0x11 && toRgba(0xFF112233u).g == 0x22 && toRgba(0xFF112233u).b == 0x33 && toRgba(0x80112233u).a == 0x80);
    CHECK(toLabelRgba(0x80112233u) == 0x11223380u);

    GlobeCompositor comp;
    const int W = 480, H = 360;
    // ---- an empty list is the plain background; a size under 16 is refused; the same list gives the same bytes ----
    GlobeDrawList none;
    none.sphere = false;
    const Picture bg = paintList(comp, none, W, H);
    bool allBg = true;
    for (const s3::Rgba& c : bg.px) { if (!isBackground(c)) { allBg = false; } }
    CHECK(allBg);
    std::vector<s3::Rgba> tiny(64, s3::Rgba{});
    std::vector<s3::Label3D> lab;
    CHECK(!comp.paint(none, tiny.data(), 8, 8, lab));
    CHECK(!comp.paint(none, nullptr, W, H, lab));

    // ---- a lit sphere: round, centred, the size the framework's sceneRadius promises, the corners untouched ----
    ViewState view;
    Texture ocean;
    {
        static const LandMask land(1440, 720);
        bakeEarth(ocean, land, nullptr, false);
    }
    GlobeDrawList sphere = drawPropagation(buildPropagationScene(PropagationFacts{}, false, LatLon{}, 0), view, ocean);
    sphere.lines.clear();
    sphere.camera = GlobeCamera{0.0, 0.0, 1.0};
    const Picture ps = paintList(comp, sphere, W, H);
    CHECK(isBackground(ps.at(2, 2)) && isBackground(ps.at(W - 3, H - 3)) && isBackground(ps.at(2, H - 3)));
    CHECK(!isBackground(ps.at(W / 2, H / 2)));
    int left = -1, right = -1, top = -1, bottom = -1;
    for (int x = 0; x < W; ++x) { if (!isBackground(ps.at(x, H / 2))) { if (left < 0) { left = x; } right = x; } }
    for (int y = 0; y < H; ++y) { if (!isBackground(ps.at(W / 2, y))) { if (top < 0) { top = y; } bottom = y; } }
    const double diamH = right - left + 1, diamV = bottom - top + 1;
    std::printf("  globe silhouette in %dx%d: %.0f x %.0f px (the shorter side is %d)\n", W, H, diamH, diamV, H);
    CHECK_NEAR(diamH, diamV, 3.0);                                                            // a circle
    CHECK(diamH > 0.70 * H && diamH < 0.90 * H);                                              // the sphere fills about 0.85 of the shorter side (sceneRadius 1.18)
    CHECK_NEAR((left + right) / 2.0, W / 2.0, 2.0);
    CHECK_NEAR((top + bottom) / 2.0, H / 2.0, 2.0);
    // zoom 2 doubles it
    GlobeDrawList zoomed = sphere;
    zoomed.camera.zoom = 2.0;
    const Picture pz = paintList(comp, zoomed, W, H);
    int zl = -1, zr = -1;
    for (int x = 0; x < W; ++x) { if (!isBackground(pz.at(x, H / 2))) { if (zl < 0) { zl = x; } zr = x; } }
    CHECK(zl == -1 || (zr - zl + 1) > 1.8 * diamH || (zl == 0 && zr == W - 1));
    // the same list gives the same bytes
    const Picture ps2 = paintList(comp, sphere, W, H);
    CHECK(ps.px.size() == ps2.px.size() && std::memcmp(ps.px.data(), ps2.px.data(), ps.px.size() * sizeof(s3::Rgba)) == 0);

    // ---- the texture is on the sphere the right way round: west red, east green, north blue (centre 0, 0) ----
    Texture flags;
    flags.width = 64;
    flags.height = 32;
    flags.key = 4242;
    flags.revision = 4242;
    flags.argb.assign(64u * 32u, 0xFF808080u);
    for (int r = 0; r < 32; ++r) {
        for (int c = 0; c < 64; ++c) {
            const std::size_t i = static_cast<std::size_t>(r) * 64u + static_cast<std::size_t>(c);
            if (r < 8) { flags.argb[i] = 0xFF0000FFu; }               // north of 45 N: blue
            else if (c < 32) { flags.argb[i] = 0xFFFF0000u; }         // west of Greenwich: red
            else { flags.argb[i] = 0xFF00FF00u; }                     // east: green
        }
    }
    GlobeDrawList tex = sphere;
    tex.texture = flags;
    const Picture pt = paintList(comp, tex, W, H);
    {
        // the scene's projection is of the LAST paint, which was `sphere`: repaint with the texture and keep it
        (void)paintList(comp, tex, W, H);
        const s3::ScreenPoint west = projectOf(comp, tex.camera, toUnit(0.0, -30.0)), east = projectOf(comp, tex.camera, toUnit(0.0, 30.0)), nrth = projectOf(comp, tex.camera, toUnit(70.0, 0.0)), mid = projectOf(comp, tex.camera, toUnit(0.0, 0.0));
        CHECK_NEAR(mid.x, W / 2.0, 1.5);
        CHECK_NEAR(mid.y, H / 2.0, 5.0);               // the camera is 1 degree short of the zenith (the framework's limit is 89): the point under it sits a few pixels high
        CHECK(west.x < mid.x && east.x > mid.x && nrth.y < mid.y);                  // west is left, east is right, north is up
        const s3::Rgba cw = pt.at(static_cast<int>(west.x), static_cast<int>(west.y));
        const s3::Rgba ce = pt.at(static_cast<int>(east.x), static_cast<int>(east.y));
        const s3::Rgba cn = pt.at(static_cast<int>(nrth.x), static_cast<int>(nrth.y));
        CHECK(cw.r > 200 && cw.g < 60 && cw.b < 60);
        CHECK(ce.g > 200 && ce.r < 60 && ce.b < 60);
        CHECK(cn.b > 200 && cn.r < 60 && cn.g < 60);
    }
    // a texture that changes under the same key is picked up through its revision
    tex.texture.revision = 4243;
    for (std::uint32_t& c : tex.texture.argb) { c = 0xFFFFFF00u; }
    const Picture py = paintList(comp, tex, W, H);
    CHECK(py.at(W / 2, H / 2).r > 200 && py.at(W / 2, H / 2).g > 200 && py.at(W / 2, H / 2).b < 60);

    // ---- a southern, far-side centre: (-40, 150) is turned to the middle of the picture, which the framework's own camera (elevation >= 4) could not have shown ----
    GlobeDrawList south = tex;
    south.camera = GlobeCamera{150.0, -40.0, 1.0};
    DrawMarker pin;
    pin.at = toUnit(-40.0, 150.0) * 1.004;
    pin.argb = 0xFFFF00FFu;
    pin.sizePx = 9.0f;
    south.markers.push_back(pin);
    DrawMarker back = pin;
    back.at = toUnit(40.0, -30.0) * 1.004;                                    // the antipode: behind the globe
    back.label = "far side";
    south.markers.push_back(back);
    DrawMarker front = pin;
    front.at = toUnit(-30.0, 160.0) * 1.004;
    front.argb = 0xFF00FFFFu;
    front.label = "near side";
    south.markers.push_back(front);
    std::vector<s3::Label3D> labels;
    const Picture pn = paintList(comp, south, W, H, &labels);
    const s3::Rgba pinC = pn.at(W / 2, H / 2);
    CHECK(pinC.r > 200 && pinC.g < 60 && pinC.b > 200);                         // the magenta pin is at the middle
    bool sawFar = false, sawNear = false;
    for (const s3::Label3D& l : labels) { if (l.text == "far side") { sawFar = true; } if (l.text == "near side") { sawNear = true; } }
    CHECK(sawNear);
    CHECK(!sawFar);                                                            // a label behind the globe is not offered
    // the antipode's marker is not drawn: the pixel at the middle is the pin, not a second marker; and its label anchor is the near side's only
    for (const s3::Label3D& l : labels) { if (l.text == "near side") { CHECK(l.x > 0.0f && l.x < 1.0f && l.y > 0.0f && l.y < 1.0f); CHECK(l.align == 1 && l.kind == 2); } }

    // ---- lines: a green great-circle arc is on the picture, a dashed one has gaps, a closed one closes ----
    GlobeDrawList lines = tex;
    lines.texture = Texture{};
    lines.camera = GlobeCamera{0.0, 0.0, 1.0};
    DrawLine eq;
    eq.points = greatCircleArc(LatLon{0, -40}, LatLon{0, 40}, 60, 0.0);
    for (Vec3& p : eq.points) { p = p * 1.002; }
    eq.argb = 0xFF00FF00u;
    eq.widthPx = 3.0f;
    lines.lines.push_back(eq);
    const Picture pl = paintList(comp, lines, W, H);
    int greenHits = 0;
    for (int i = 0; i < 20; ++i) {
        const double lon = -35.0 + 70.0 * i / 19.0;
        const s3::ScreenPoint sp = projectOf(comp, lines.camera, toUnit(0.0, lon) * 1.002);
        const s3::Rgba c = pl.at(static_cast<int>(sp.x), static_cast<int>(sp.y));
        if (c.g > 200 && c.r < 80 && c.b < 80) { ++greenHits; }
    }
    CHECK(greenHits >= 19);
    auto litPixels = [&](const Picture& p) { int n = 0; for (const s3::Rgba& c : p.px) { if (c.g > 200 && c.r < 80 && c.b < 80) { ++n; } } return n; };
    const int solidN = litPixels(pl);
    lines.lines[0].dashed = true;
    const Picture pd = paintList(comp, lines, W, H);
    const int dashedN = litPixels(pd);
    std::printf("  line pixels: solid %d dashed %d\n", solidN, dashedN);
    CHECK(dashedN > 0 && dashedN < solidN * 0.8);                               // a dashed line is not a solid one
    // a closed line closes: a ring of 8 points about the middle has its last side drawn
    GlobeDrawList ring;
    ring.sphere = false;
    ring.camera = GlobeCamera{0.0, 0.0, 1.0};
    DrawLine sq;
    sq.points = {toUnit(10, -10), toUnit(10, 10), toUnit(-10, 10), toUnit(-10, -10)};
    sq.argb = 0xFFFF0000u;
    sq.widthPx = 3.0f;
    sq.closed = false;
    const int openN = [&] { ring.lines = {sq}; const Picture p = paintList(comp, ring, W, H); int n = 0; for (const s3::Rgba& c : p.px) { if (c.r > 200 && c.g < 80) { ++n; } } return n; }();
    sq.closed = true;
    const int closedN = [&] { ring.lines = {sq}; const Picture p = paintList(comp, ring, W, H); int n = 0; for (const s3::Rgba& c : p.px) { if (c.r > 200 && c.g < 80) { ++n; } } return n; }();
    CHECK(closedN > openN * 1.2);

    // ---- the frame time: a globe with the baked earth, 40 arcs, 60 markers in a 960 x 700 well (a big page's well), and the bake itself ----
    {
        GlobeDrawList heavy = tex;
        heavy.texture = ocean;
        heavy.camera = GlobeCamera{-0.1, 51.5, 1.0};
        for (const Polyline& g : graticule(30.0, 3.0, 1.002)) { DrawLine l; l.points = g; l.argb = 0x55AEBFDFu; heavy.lines.push_back(l); }
        for (int i = 0; i < 40; ++i) {
            DrawLine l;
            l.points = skywaveArc(LatLon{51.5, -0.1}, LatLon{-60.0 + 3.0 * i, -170.0 + 8.5 * i}, 2, 300.0, 6.0, 24);
            l.argb = 0xFFF5A524u;
            l.widthPx = 2.0f;
            heavy.lines.push_back(l);
        }
        for (int i = 0; i < 60; ++i) { DrawMarker m; m.at = toUnit(-60.0 + 2.0 * i, -170.0 + 5.5 * i) * 1.004; m.argb = 0xFF5FD28Au; m.label = i % 10 == 0 ? "stn" : ""; heavy.markers.push_back(m); }
        GlobeCompositor timing;
        std::vector<s3::Rgba> big(960u * 700u);
        std::vector<s3::Label3D> tl;
        const auto t0 = std::chrono::steady_clock::now();
        const int reps = 20;
        for (int i = 0; i < reps; ++i) { CHECK(timing.paint(heavy, big.data(), 960, 700, tl)); }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / reps;
        Texture tb;
        static const LandMask landForBake(1440, 720);
        const auto b0 = std::chrono::steady_clock::now();
        bakeEarth(tb, landForBake, &noonForTiming(), true);
        const double bakeMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - b0).count();
        std::printf("  frame time: a 960 x 700 globe with 40 arcs and 60 markers %.1f ms per paint; one earth bake (1024 x 512, sun and greyline) %.1f ms\n", ms, bakeMs);
        CHECK(ms < 250.0);
        CHECK(bakeMs < 500.0);
    }
    // ---- the orbits: the Sun at the middle, north (+Y) up and east (+X) right from above, the tilt turns the ring into an ellipse ----
    GlobeDrawList orb;
    orb.space = GlobeDrawList::Space::Orbits;
    orb.sphere = false;
    orb.camera = GlobeCamera{0.0, 89.0, 1.0};
    DrawMarker sunM;
    sunM.at = Vec3{0, 0, 0};
    sunM.argb = 0xFFFFD27Au;
    sunM.sizePx = 9.0f;
    sunM.hiddenBehind = false;
    DrawMarker xM = sunM;
    xM.at = Vec3{0.5, 0, 0};
    xM.argb = 0xFFFF0000u;
    xM.label = "east";
    DrawMarker yM = sunM;
    yM.at = Vec3{0, 0.5, 0};
    yM.argb = 0xFF00FF00u;
    orb.markers = {sunM, xM, yM};
    const Picture po = paintList(comp, orb, W, H);
    const s3::ScreenPoint sx = comp.scene().project(s3::Vec3{0.5f, 0, 0}), sy = comp.scene().project(s3::Vec3{0, 0.5f, 0}), s0 = comp.scene().project(s3::Vec3{0, 0, 0});
    CHECK_NEAR(s0.x, W / 2.0, 1.5);
    CHECK_NEAR(s0.y, H / 2.0, 1.5);
    CHECK(sx.x > s0.x + 20.0f && std::fabs(sx.y - s0.y) < 6.0f);
    CHECK(sy.y < s0.y - 20.0f && std::fabs(sy.x - s0.x) < 6.0f);
    const s3::Rgba cx = po.at(static_cast<int>(sx.x), static_cast<int>(sx.y));
    CHECK(cx.r > 200 && cx.g < 60);
    const s3::Rgba cs = po.at(W / 2, H / 2);
    CHECK(cs.r > 200 && cs.g > 150 && cs.b < 160);                           // the Sun
    GlobeDrawList tilted = orb;
    tilted.camera.centreLatDeg = 30.0;
    (void)paintList(comp, tilted, W, H);
    const s3::ScreenPoint ty = comp.scene().project(s3::Vec3{0, 0.5f, 0});
    const s3::ScreenPoint tx = comp.scene().project(s3::Vec3{0.5f, 0, 0});
    CHECK(std::fabs(ty.y - H / 2.0) < 0.75 * std::fabs(sy.y - s0.y));       // tilted, the far axis is foreshortened
    CHECK(std::fabs(tx.x - W / 2.0) > 0.9 * std::fabs(sx.x - s0.x));        // and the near one is not
    // the spin turns the system about the pole: a quarter turn puts +X where +Y was
    GlobeDrawList spun = orb;
    spun.camera.centreLonDeg = 90.0;
    (void)paintList(comp, spun, W, H);
    const s3::ScreenPoint spX = comp.scene().project(s3::Vec3{0.5f, 0, 0});
    CHECK(std::fabs(spX.x - W / 2.0) < 6.0 && std::fabs(spX.y - H / 2.0) > 20.0);
}

// ------------------------------------------------------------------------------------------------------------------------
void sectionPrefs() {
    namespace v3 = foxsdr::slintview3d;
    // the latitude scale: -85 .. 85 on the framework's 4 .. 89 elevation, and back
    CHECK_NEAR(latitudeToElevation(-85.0), 4.0, 1e-4);
    CHECK_NEAR(latitudeToElevation(85.0), 89.0, 1e-4);
    CHECK_NEAR(latitudeToElevation(0.0), 46.5, 1e-4);
    CHECK_NEAR(latitudeToElevation(-200.0), 4.0, 1e-4);
    for (double lat = -85.0; lat <= 85.0; lat += 5.0) { CHECK_NEAR(elevationToLatitude(latitudeToElevation(lat)), lat, 1e-3); }
    CHECK_NEAR(elevationToLatitude(0.0f), -85.0, 1e-9);                        // outside the scale: held
    CHECK_NEAR(elevationToLatitude(200.0f), 85.0, 1e-9);
    // names and defaults
    CHECK(std::string(prefsName(ViewKind::Propagation)) == "propagation" && std::string(prefsName(ViewKind::Ft8)) == "ft8" && std::string(prefsName(ViewKind::Goes)) == "goes" &&
          std::string(prefsName(ViewKind::SolarSystem)) == "solar" && std::string(prefsName(ViewKind::Satellites)) == "satellites" && std::string(prefsName(ViewKind::None)).empty());
    // the defaults are what a fresh ViewState is: off, the page's own view
    for (ViewKind k : {ViewKind::Propagation, ViewKind::Ft8, ViewKind::Goes, ViewKind::SolarSystem, ViewKind::Satellites}) {
        ViewState fresh;
        const v3::ViewState d = prefsDefaults(k);
        CHECK(!d.on);
        ViewState read;
        read.bandId = 99; read.greyline = false; read.windowMinutes = 5; read.logScale = false; read.showMoon = false; read.footprints = false; read.groundTracks = false; read.goesSatellite = 1;
        fromPrefs(read, k, d);
        CHECK(!read.globeOn && !read.cameraSet);
        // the defaults read back as a fresh view's choices
        if (k == ViewKind::Propagation) { CHECK(read.bandId == 0 && read.greyline == fresh.greyline); }
        if (k == ViewKind::Ft8) { CHECK(read.bandId == 0 && read.windowMinutes == fresh.windowMinutes); }
        if (k == ViewKind::Goes) { CHECK(read.goesSatellite == fresh.goesSatellite); }
        if (k == ViewKind::SolarSystem) { CHECK(read.logScale == fresh.logScale && read.showMoon == fresh.showMoon); }
        if (k == ViewKind::Satellites) { CHECK(read.footprints == fresh.footprints && read.groundTracks == fresh.groundTracks); }
    }
    // every kind's state survives the text of the setting
    {
        ViewState a;
        a.globeOn = true; a.cameraSet = true;
        a.camera = GlobeCamera{-122.4, -37.8, 2.5};
        a.bandId = 3; a.greyline = false;
        const std::string text = [&] { v3::ViewPrefs p; p.set("propagation", toPrefs(a, ViewKind::Propagation)); return p.serialize(); }();
        const v3::ViewState got = v3::ViewPrefs::parse(text).get("propagation", prefsDefaults(ViewKind::Propagation));
        ViewState b;
        fromPrefs(b, ViewKind::Propagation, got);
        CHECK(b.globeOn && b.cameraSet && b.bandId == 3 && !b.greyline);
        CHECK_NEAR(b.camera.centreLonDeg, -122.4, 0.01);
        CHECK_NEAR(b.camera.centreLatDeg, -37.8, 0.02);             // a southern latitude: the framework's 4..89 elevation scale carries it
        CHECK_NEAR(b.camera.zoom, 2.5, 1e-6);
    }
    {
        ViewState a;
        a.globeOn = true; a.windowMinutes = 1440; a.bandId = 12;
        v3::ViewPrefs p;
        p.set("ft8", toPrefs(a, ViewKind::Ft8));
        ViewState b;
        fromPrefs(b, ViewKind::Ft8, v3::ViewPrefs::parse(p.serialize()).get("ft8", prefsDefaults(ViewKind::Ft8)));
        CHECK(b.globeOn && !b.cameraSet && b.windowMinutes == 1440 && b.bandId == 12);
        a.windowMinutes = 15;
        p.set("ft8", toPrefs(a, ViewKind::Ft8));
        fromPrefs(b, ViewKind::Ft8, v3::ViewPrefs::parse(p.serialize()).get("ft8", prefsDefaults(ViewKind::Ft8)));
        CHECK(b.windowMinutes == 15);
    }
    {
        ViewState a;
        a.globeOn = true; a.cameraSet = true; a.spinDeg = 300.0; a.tiltDeg = 70.0; a.camera.zoom = 1.6; a.logScale = false; a.showMoon = false;
        v3::ViewPrefs p;
        p.set("solar", toPrefs(a, ViewKind::SolarSystem));
        ViewState b;
        fromPrefs(b, ViewKind::SolarSystem, v3::ViewPrefs::parse(p.serialize()).get("solar", prefsDefaults(ViewKind::SolarSystem)));
        CHECK(b.globeOn && b.cameraSet && !b.logScale && !b.showMoon);
        CHECK_NEAR(b.spinDeg, 300.0, 0.01);                          // 300 degrees of spin is -60 in the framework's -180..180
        CHECK_NEAR(b.tiltDeg, 70.0, 1e-4);
        CHECK_NEAR(b.camera.zoom, 1.6, 1e-5);
    }
    {
        ViewState a;
        a.globeOn = true; a.goesSatellite = 1; a.footprints = false; a.groundTracks = false;
        v3::ViewPrefs p;
        p.set("goes", toPrefs(a, ViewKind::Goes));
        p.set("satellites", toPrefs(a, ViewKind::Satellites));
        ViewState g, s;
        fromPrefs(g, ViewKind::Goes, v3::ViewPrefs::parse(p.serialize()).get("goes", prefsDefaults(ViewKind::Goes)));
        fromPrefs(s, ViewKind::Satellites, v3::ViewPrefs::parse(p.serialize()).get("satellites", prefsDefaults(ViewKind::Satellites)));
        CHECK(g.globeOn && g.goesSatellite == 1);
        CHECK(s.globeOn && !s.footprints && !s.groundTracks);
    }
    // a camera never touched is not frozen: it reads back untouched, whatever it was showing
    {
        ViewState a;
        a.globeOn = true; a.cameraSet = false; a.camera = GlobeCamera{33.0, 44.0, 3.0};
        ViewState b;
        fromPrefs(b, ViewKind::Propagation, toPrefs(a, ViewKind::Propagation));
        CHECK(b.globeOn && !b.cameraSet && b.camera.zoom == 1.0 && b.camera.centreLonDeg == 0.0);
    }
    // another view's entry, written by the framework's own views, is not disturbed by this one's
    {
        v3::ViewPrefs p = v3::ViewPrefs::parse("waterfall:on=1,az=-40,el=20,zoom=1.5");
        ViewState a;
        a.globeOn = true;
        p.set("solar", toPrefs(a, ViewKind::SolarSystem));
        const v3::ViewPrefs q = v3::ViewPrefs::parse(p.serialize());
        CHECK(q.get("waterfall", v3::ViewState{}).on);
        CHECK(q.get("solar", v3::ViewState{}).on);
        CHECK(!q.get("goes", v3::ViewState{}).on);
    }
    // a hostile or newer setting is read leniently: out of range options are held
    {
        v3::ViewState wild;
        wild.on = true;
        wild.opt = {15, 15, 15, 1};
        ViewState b;
        fromPrefs(b, ViewKind::Propagation, wild);
        CHECK(b.bandId == 5);
        fromPrefs(b, ViewKind::Ft8, wild);
        CHECK(b.bandId == 12 && b.windowMinutes == 1440);
        fromPrefs(b, ViewKind::Goes, wild);
        CHECK(b.goesSatellite == 1);
    }
}

}  // namespace

int main() {
    sectionViews();
    sectionWords();
    sectionPropagation();
    sectionFt8();
    sectionGoes();
    sectionSolar();
    sectionSatellites();
    sectionDraw();
    sectionCompositor();
    sectionPrefs();
    return testSummary("test_slint_host_globe");
}
