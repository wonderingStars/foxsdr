// The PATCH page's two live faces on the host's side (src/slint/patch_faces.*; docs/multi-radio-records/patch-faces.md): no Slint, no window, no engine.
//   A. the size a part's pictures are painted at (from the node's size; usable, capped, refused when the face has no room)
//   B. the Spectrum well: a frame cut to a Radio-fed slice puts the generator's tone at the column the arithmetic says, with the floor under it; a Channel-fed slice of the same frame is
//      narrower and puts the same tone in its middle; a second frame scrolls the waterfall a row; the same slice again keeps the picture, any change of slice or size starts it afresh;
//      an unusable frame paints nothing
//   C. the Map face's view: Whole world (the planet's centre, the span the picture's shape needs, a Mercator basemap's flat 360), Fit to targets (the map pages' fitView over the
//      targets that are shown and placed), a pan and a wheel notch (the point under the pointer stays under it)
//   D. the targets a Map part draws: those of the modules wired in, projected onto the picture, the stale and the unplaced and the off-picture ones left out, capped
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "patch_faces.hpp"
#include "test_check.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

using namespace foxsdr::slintpatch;
using foxsdr::slinthost::Rgba;
namespace pc = cascade::core::patch;

namespace {

// A frame as the engine's spectrum is: N bins dBFS, fftshifted, bin k at -rate/2 + (k + 0.5) rate / N. A floor and tones (offset Hz from the centre, level dBFS).
std::vector<float> frameOf(std::size_t n, double rate, float floorDb, const std::vector<std::pair<double, float>>& tones) {
    std::vector<float> f(n, floorDb);
    for (const auto& t : tones) {
        const long k = static_cast<long>(std::floor((t.first + 0.5 * rate) / (rate / static_cast<double>(n))));
        if (k >= 0 && k < static_cast<long>(n)) { f[static_cast<std::size_t>(k)] = t.second; }
    }
    return f;
}

// The topmost lit pixel row of a trace column (the trace is drawn from the bottom up), or -1.
int topLit(const SpectrumWell& w, int col) {
    for (std::uint32_t y = 0; y < w.size().traceH; ++y) {
        if (w.trace()[static_cast<std::size_t>(y) * w.size().w + static_cast<std::size_t>(col)].a != 0) { return static_cast<int>(y); }
    }
    return -1;
}

cascade::core::TrackPresentation shownUnlessOld(const CascadeTrack& t) {
    cascade::core::TrackPresentation p;
    p.visible = t.ageMs < 60000u;
    p.alpha = t.ageMs < 30000u ? 1.0f : 0.5f;
    return p;
}

cascade::core::HostTrack track(const char* id, double lat, double lon, const char* plugin = "ADS-B", std::uint32_t kind = CASCADE_TRACK_AIRCRAFT, std::uint64_t age = 0) {
    cascade::core::HostTrack h;
    std::memset(&h.t, 0, sizeof(h.t));
    std::snprintf(h.t.id, sizeof(h.t.id), "%s", id);
    h.t.latDeg = lat;
    h.t.lonDeg = lon;
    h.t.altM = std::numeric_limits<double>::quiet_NaN();
    h.t.courseDeg = std::numeric_limits<double>::quiet_NaN();
    h.t.speedMps = std::numeric_limits<double>::quiet_NaN();
    h.t.ageMs = age;
    h.t.kind = kind;
    h.plugin = plugin;
    return h;
}

void sectionA() {
    std::printf("A. the pictures' size\n");
    float w = 0.0f, h = 0.0f;
    pc::defaultNodeSize(pc::NodeKind::Display, w, h);
    const WellSize s = wellSizeOf(w, h);
    CHECK(s.usable());
    CHECK(s.w == static_cast<std::uint32_t>(w - 22.0f));
    CHECK(s.traceH + s.wfH + 2u <= static_cast<std::uint32_t>(h - 40.0f) + 1u);
    CHECK(s.traceH * 100u / (s.traceH + s.wfH + 2u) >= 36u && s.traceH * 100u / (s.traceH + s.wfH + 2u) <= 40u);   // the trace is the upper 38 %
    CHECK(!wellSizeOf(30.0f, 30.0f).usable());                      // no room for a face
    CHECK(!wellSizeOf(0.0f, 0.0f).usable());
    CHECK(!wellSizeOf(-5.0f, 200.0f).usable());
    const WellSize big = wellSizeOf(5000.0f, 4000.0f);              // dragged huge: a bounded picture
    CHECK(big.w == kWellMaxW && big.traceH == kWellMaxTraceH && big.wfH == kWellMaxWfH);
    const WellSize taller = wellSizeOf(w, h + 100.0f);              // follows the node's resize
    CHECK(taller.wfH > s.wfH && taller.w == s.w);
    const WellSize wider = wellSizeOf(w + 100.0f, h);
    CHECK(wider.w == s.w + 100u);
}

void sectionB() {
    std::printf("B. the Spectrum well\n");
    const double rate = 2.0e6;
    const std::vector<float> frame = frameOf(4096, rate, -90.0f, {{300000.0, -30.0f}, {-500000.0, -45.0f}});
    SpectrumWell radio;
    WellSize size;
    size.w = 300;
    size.traceH = 60;
    size.wfH = 100;
    CHECK(radio.shape(size, -1.0e6, 1.0e6, rate));
    CHECK(radio.frames() == 0 && radio.litWaterfallRows() == 0);
    CHECK(radio.push(frame));
    // the tone at +300 kHz of a 2 MHz slice: 0.65 of the way across; the loudest column is its column
    const int expectCol = static_cast<int>(std::floor(0.65 * size.w));
    CHECK(std::abs(radio.peakColumn() - expectCol) <= 1);
    CHECK_NEAR(radio.peakDb(), -30.0, 0.01);
    CHECK_NEAR(radio.floorDb(), -90.0, 0.01);
    CHECK(radio.litWaterfallRows() == 1);
    // the trace stands above the floor at that column and not away from it
    const int tone = topLit(radio, radio.peakColumn());
    const int away = topLit(radio, 20);
    CHECK(tone >= 0 && away >= 0 && tone < away);
    // and the second tone, at -500 kHz = 0.25 across, stands above the floor too, lower than the first
    const int second = topLit(radio, static_cast<int>(0.25 * size.w));
    CHECK(second >= 0 && second < away && second > tone);
    // the waterfall: a row a frame, newest at the top, the picture changes
    const std::uint64_t w1 = radio.waterfallHash(), t1 = radio.traceHash();
    CHECK(radio.push(frameOf(4096, rate, -90.0f, {{-200000.0, -30.0f}})));
    CHECK(radio.frames() == 2 && radio.litWaterfallRows() == 2);
    CHECK(radio.waterfallHash() != w1);
    CHECK(radio.traceHash() != t1);                                          // the tone moved to -200 kHz: 0.4 across
    CHECK(std::abs(radio.peakColumn() - static_cast<int>(std::floor(0.4 * size.w))) <= 1);
    for (int i = 0; i < 150; ++i) { CHECK(radio.push(frame)); }
    CHECK(radio.litWaterfallRows() == static_cast<int>(size.wfH));            // the picture is full, it scrolls, it does not grow
    // the same shape again keeps the picture; a changed slice, size or rate starts it afresh
    CHECK(!radio.shape(size, -1.0e6, 1.0e6, rate));
    CHECK(radio.frames() == 152);
    WellSize taller = size;
    taller.wfH = 120;
    CHECK(radio.shape(taller, -1.0e6, 1.0e6, rate));
    CHECK(radio.frames() == 0 && radio.litWaterfallRows() == 0);
    CHECK(radio.shape(taller, -0.9e6, 1.0e6, rate));
    CHECK(radio.shape(taller, -0.9e6, 1.0e6, rate * 2.0));

    // a CHANNEL's slice of the same frame: 48 kHz about the tone, so the tone is in the middle and the other tone is not in it
    SpectrumWell chan;
    CHECK(chan.shape(size, 300000.0 - 24000.0, 300000.0 + 24000.0, rate));
    CHECK(chan.push(frame));
    CHECK(std::abs(chan.peakColumn() - static_cast<int>(size.w / 2)) <= 1);
    CHECK_NEAR(chan.peakDb(), -30.0, 0.01);
    SpectrumWell other;
    CHECK(other.shape(size, -500000.0 - 24000.0, -500000.0 + 24000.0, rate));
    CHECK(other.push(frame));
    CHECK_NEAR(other.peakDb(), -45.0, 0.01);                                 // its own slice holds only the second tone
    CHECK(std::abs(other.peakColumn() - static_cast<int>(size.w / 2)) <= 1);
    const double radioSpan = 2.0e6, chanSpan = 48000.0;
    CHECK(chanSpan < radioSpan / 10.0);
    // a slice that holds no tone shows only the floor: the loudest column is the floor
    SpectrumWell quiet;
    CHECK(quiet.shape(size, 100000.0, 148000.0, rate));
    CHECK(quiet.push(frame));
    CHECK_NEAR(quiet.peakDb(), -90.0, 0.01);

    // unusable input paints nothing and counts nothing
    SpectrumWell bad;
    CHECK(bad.shape(size, -1.0e6, 1.0e6, rate));
    CHECK(!bad.push({}));
    CHECK(bad.frames() == 0);
    SpectrumWell inverted;
    CHECK(inverted.shape(size, 1.0e6, -1.0e6, rate));
    CHECK(!inverted.push(frame));
    SpectrumWell norate;
    CHECK(norate.shape(size, -1.0e6, 1.0e6, 0.0));
    CHECK(!norate.push(frame));
    SpectrumWell tiny;
    WellSize none;
    CHECK(tiny.shape(none, -1.0e6, 1.0e6, rate));
    CHECK(!tiny.push(frame));                                                // no room: nothing to paint into
}

void sectionC() {
    std::printf("C. the Map face's view\n");
    CHECK_NEAR(wholeWorldSpan(512.0, 384.0), 360.0, 1e-9);                    // wider than tall by less than 2:1: the flat 360
    CHECK_NEAR(wholeWorldSpan(1000.0, 300.0), 600.0, 1e-9);                   // a wide picture needs more longitude for 180 degrees of latitude
    CHECK_NEAR(wholeWorldSpan(0.0, 100.0), 360.0, 1e-9);
    CHECK_NEAR(wholeWorldSpan(100.0, 0.0), 360.0, 1e-9);
    MapFaceView v;
    v.centreLat = 51.0;
    v.centreLon = 3.0;
    v.spanDeg = 2.0;
    wholeWorld(v, 1000.0, 300.0, false);
    CHECK(v.centreLat == 0.0 && v.centreLon == 0.0 && v.fitted);
    CHECK_NEAR(v.spanDeg, 600.0, 1e-9);
    wholeWorld(v, 1000.0, 300.0, true);                                       // a Mercator basemap keeps the flat 360
    CHECK_NEAR(v.spanDeg, 360.0, 1e-9);

    // fit: the centre of the targets and the map pages' span
    MapFaceView f;
    std::vector<cascade::core::HostTrack> tracks = {track("A", 50.0, -1.0), track("B", 52.0, 1.0)};
    CHECK(fitToTargets(f, tracks, 400.0, 300.0, &shownUnlessOld));
    CHECK_NEAR(f.centreLat, 51.0, 1e-9);
    CHECK_NEAR(f.centreLon, 0.0, 1e-9);
    CHECK_NEAR(f.spanDeg, (2.0 * 1.3 + 0.1) * (400.0 / 300.0), 1e-9);        // the taller-than-wide case: latitude times the aspect
    CHECK(f.fitted);
    // a stale target and a target with no position are not fitted to
    MapFaceView g;
    g.centreLat = 7.0;
    g.centreLon = 8.0;
    g.spanDeg = 9.0;
    std::vector<cascade::core::HostTrack> bad = {track("OLD", 10.0, 10.0, "ADS-B", CASCADE_TRACK_AIRCRAFT, 600000), track("NOPOS", 11.0, 11.0)};
    bad[1].t.flags |= CASCADE_TRACK_FLAG_NO_POSITION;
    bad.push_back(track("NAN", std::numeric_limits<double>::quiet_NaN(), 1.0));
    CHECK(!fitToTargets(g, bad, 400.0, 300.0, &shownUnlessOld));              // nothing to fit to: the view is untouched
    CHECK(g.centreLat == 7.0 && g.centreLon == 8.0 && g.spanDeg == 9.0 && !g.fitted);
    bad.push_back(track("GOOD", 40.0, 5.0));
    CHECK(fitToTargets(g, bad, 400.0, 300.0, &shownUnlessOld));
    CHECK_NEAR(g.centreLat, 40.0, 1e-9);
    CHECK_NEAR(g.centreLon, 5.0, 1e-9);
    CHECK(!fitToTargets(g, {}, 400.0, 300.0, &shownUnlessOld));

    // pan and zoom, over a compositor
    foxsdr::slintmap::TileCompositor comp;
    comp.setSize(400, 300);
    MapFaceView p;
    p.centreLat = 51.0;
    p.centreLon = 0.0;
    p.spanDeg = 10.0;
    comp.setView(p.centreLat, p.centreLon, p.spanDeg);
    panView(p, comp, 0.1f, 0.0f);                                             // dragged right: the map follows the hand, so the centre moves west
    CHECK(p.centreLon < -0.5 && std::fabs(p.centreLat - 51.0) < 1e-6 && p.fitted);
    comp.setView(51.0, 0.0, 10.0);
    p.centreLat = 51.0;
    p.centreLon = 0.0;
    panView(p, comp, 0.0f, 0.1f);                                             // dragged down: the centre moves north
    CHECK(p.centreLat > 51.0);
    // the point under the pointer stays under it across a zoom
    MapFaceView z;
    z.centreLat = 30.0;
    z.centreLon = 20.0;
    z.spanDeg = 40.0;
    comp.setView(z.centreLat, z.centreLon, z.spanDeg);
    double latBefore = 0.0, lonBefore = 0.0;
    comp.unproject(0.75, 0.25, latBefore, lonBefore);
    zoomView(z, comp, 0.5, 0.75f, 0.25f);
    CHECK_NEAR(z.spanDeg, 20.0, 1e-9);
    comp.setView(z.centreLat, z.centreLon, z.spanDeg);
    double latAfter = 0.0, lonAfter = 0.0;
    comp.unproject(0.75, 0.25, latAfter, lonAfter);
    CHECK_NEAR(latAfter, latBefore, 1e-6);
    CHECK_NEAR(lonAfter, lonBefore, 1e-6);
    // the limits
    for (int i = 0; i < 40; ++i) { zoomView(z, comp, 0.5, 0.5f, 0.5f); }
    CHECK_NEAR(z.spanDeg, foxsdr::slintmap::TileCompositor::kMinSpanDeg, 1e-12);
    for (int i = 0; i < 40; ++i) { zoomView(z, comp, 2.0, 0.5f, 0.5f); }
    CHECK_NEAR(z.spanDeg, foxsdr::slintmap::TileCompositor::kMaxSpanDeg, 1e-12);
    CHECK(z.centreLat >= -85.0 && z.centreLat <= 85.0);
}

void sectionD() {
    std::printf("D. the targets a Map part draws\n");
    foxsdr::slintmap::TileCompositor comp;
    comp.setSize(400, 300);
    comp.setView(51.0, 0.0, 10.0);
    const foxsdr::slintmap::MapLook look;
    std::vector<cascade::core::HostTrack> all = {track("CENTRE", 51.0, 0.0), track("SHIP", 51.0, 2.0, "AIS", CASCADE_TRACK_VESSEL), track("OTHER", 51.0, 1.0, "APRS", CASCADE_TRACK_STATION),
                                                 track("FAR", -40.0, 150.0), track("OLD", 51.0, 0.5, "ADS-B", CASCADE_TRACK_AIRCRAFT, 600000), track("NOPOS", 51.0, 0.6)};
    all[5].t.flags |= CASCADE_TRACK_FLAG_NO_POSITION;
    // only the modules wired in
    const std::vector<cascade::core::HostTrack> mine = tracksOfSources(all, {"ADS-B", "AIS"});
    CHECK(mine.size() == 5u);                                                 // CENTRE, SHIP, FAR, OLD, NOPOS - not APRS
    CHECK(tracksOfSources(all, {}).empty());
    CHECK(tracksOfSources(all, {"Nothing"}).empty());
    const std::vector<MapFaceTarget> t = projectTargets(mine, comp, look, &shownUnlessOld);
    CHECK(t.size() == 2u);                                                    // FAR is off the picture, OLD is stale, NOPOS has no place
    CHECK(t[0].id == "CENTRE" && std::fabs(t[0].x - 0.5f) < 0.01f && std::fabs(t[0].y - 0.5f) < 0.01f);
    CHECK(t[0].heading < 0.0f);                                               // no course: -1, never NaN
    CHECK(t[0].size == static_cast<float>(look.iconPx));                      // an aircraft is drawn at the Display setting's size
    CHECK(t[1].id == "SHIP" && t[1].x > 0.5f && t[1].size == 0.0f);           // another kind: the pane's own glyph
    CHECK(t[1].kind == static_cast<int>(CASCADE_TRACK_VESSEL));
    // the label is the id when there is none, the label when there is
    std::vector<cascade::core::HostTrack> one = {track("ABC123", 51.0, 0.0)};
    CHECK(projectTargets(one, comp, look, &shownUnlessOld)[0].label == "ABC123");
    std::snprintf(one[0].t.label, sizeof(one[0].t.label), "%s", "BAW9");
    one[0].t.courseDeg = 270.0;
    one[0].t.flags |= CASCADE_TRACK_FLAG_EMERGENCY;
    const std::vector<MapFaceTarget> lab = projectTargets(one, comp, look, &shownUnlessOld);
    CHECK(lab[0].label == "BAW9" && lab[0].heading == 270.0f && lab[0].emergency && !lab[0].estimated);
    // a faded target keeps its fade
    std::vector<cascade::core::HostTrack> faded = {track("F", 51.0, 0.0, "ADS-B", CASCADE_TRACK_AIRCRAFT, 40000)};
    CHECK(projectTargets(faded, comp, look, &shownUnlessOld)[0].alpha == 0.5f);
    // a crowd is capped
    std::vector<cascade::core::HostTrack> crowd;
    for (int i = 0; i < 1000; ++i) { crowd.push_back(track(("T" + std::to_string(i)).c_str(), 51.0, 0.001 * i)); }
    CHECK(projectTargets(crowd, comp, look, &shownUnlessOld).size() == kMaxFaceTargets);
    CHECK(projectTargets({}, comp, look, &shownUnlessOld).empty());
}

}  // namespace

int main() {
    std::printf("test_slint_host_patchfaces\n");
    sectionA();
    sectionB();
    sectionC();
    sectionD();
    return testSummary("test_slint_host_patchfaces");
}
