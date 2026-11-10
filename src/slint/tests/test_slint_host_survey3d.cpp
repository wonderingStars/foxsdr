// The SURVEY ENGINE'S 3D SURFACE (src/slint/survey3d_logic.hpp and survey3d_compositor.hpp, docs 6.64) against synthetic panel rows: the cells read back as numbers (frequency, width, dB, percent,
// by heading), the floor, the span, the ridge grid (a ridge is as wide, as high and as long as its channel's cells say), the level range, and the painted surface with the rank numbers at the
// ridges' far ends. No Slint, no engine, no window.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "survey3d_compositor.hpp"
#include "survey3d_logic.hpp"
#include "test_check.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace foxsdr::slintview3d;
namespace s3 = foxsdr::scene3d;

namespace {

const std::vector<std::string> kHeads{"Frequency", "Width", "Guess", "Peak dB", "Over floor", "Seen", "Band"};

SurveyChannel chan(double mhz, double widthKhz, double peak, double over, double seen, const char* band = "") {
    SurveyChannel c;
    c.freqHz = mhz * 1e6;
    c.widthHz = widthKhz * 1e3;
    c.peakDb = peak;
    c.overDb = over;
    c.seenPct = seen;
    c.haveWidth = c.havePeak = c.haveOver = c.haveSeen = true;
    char b[48];
    std::snprintf(b, sizeof(b), "%.3f MHz", mhz);
    c.freqText = b;
    std::snprintf(b, sizeof(b), "%.0f %% of dwells", seen);
    c.seenText = b;
    c.band = band;
    return c;
}

std::size_t opaque(const std::vector<s3::Rgba>& px) { std::size_t n = 0; for (const auto& p : px) { n += p.a > 0 ? 1 : 0; } return n; }
const s3::Label3D* find(const SurveyResult& r, const std::string& text) {
    for (const s3::Label3D& l : r.labels) { if (l.text == text) { return &l; } }
    return nullptr;
}

void testParse() {
    double v = 0.0;
    std::string rest;
    CHECK(parseLeadingNumber("  -48 dB", v, rest) && v == -48.0 && rest == "db");
    CHECK(parseLeadingNumber("+31 dB", v, rest) && v == 31.0);
    CHECK(parseLeadingNumber("12.5 kHz", v, rest) && v == 12.5 && rest == "khz");
    CHECK(!parseLeadingNumber("", v, rest));
    CHECK(!parseLeadingNumber("abc", v, rest));
    CHECK(!parseLeadingNumber("-", v, rest));
    CHECK(!parseLeadingNumber(".", v, rest));
    double hz = 0.0;
    CHECK(parseFrequencyHz("145.500 MHz", hz) && std::fabs(hz - 145.5e6) < 1);
    CHECK(parseFrequencyHz("144.8500", hz) && std::fabs(hz - 144.85e6) < 1);          // a bare number under 30 000 is MHz
    CHECK(parseFrequencyHz("145500000", hz) && std::fabs(hz - 145.5e6) < 1);          // above it Hz
    CHECK(parseFrequencyHz("0.5 GHz", hz) && std::fabs(hz - 5e8) < 1);
    CHECK(parseFrequencyHz("7074 kHz", hz) && std::fabs(hz - 7.074e6) < 1);
    CHECK(parseFrequencyHz("1090 mhz", hz) && std::fabs(hz - 1.09e9) < 1);
    CHECK(!parseFrequencyHz("Survey", hz));
    CHECK(!parseFrequencyHz("145.5 parsecs", hz));
    CHECK(!parseFrequencyHz("-5 MHz", hz));
    CHECK(parseWidthHz("12.5 kHz", hz) && std::fabs(hz - 12500.0) < 1e-6);
    CHECK(parseWidthHz("9 Hz", hz) && hz == 9.0);
    CHECK(parseWidthHz("0.2 MHz", hz) && std::fabs(hz - 2e5) < 1e-6);
    CHECK(parseWidthHz("3000", hz) && hz == 3000.0);
    CHECK(!parseWidthHz("wide", hz));
    double db = 0.0;
    CHECK(parseDb("-48", db) && db == -48.0);
    CHECK(parseDb("+31 dB", db) && db == 31.0);
    CHECK(!parseDb("31 volts", db));
    double pct = 0.0;
    CHECK(parsePercent("86 % of dwells", pct) && pct == 86.0);
    CHECK(parsePercent("9", pct) && pct == 9.0);
    CHECK(parsePercent("250 %", pct) && pct == 100.0);                                 // clamped
    CHECK(parsePercent("2/2", pct) && pct == 100.0);                                   // the plugin's own "seen / made" count
    CHECK(parsePercent("1/4", pct) && pct == 25.0);
    CHECK(parsePercent("1 / 6", pct) && std::fabs(pct - 100.0 / 6.0) < 1e-9);
    CHECK(!parsePercent("1/0", pct));
    CHECK(!parsePercent("1/x", pct));
    CHECK(!parsePercent("often", pct));
    // a panel row by its headings
    SurveyChannel c;
    CHECK(channelOfCells(kHeads, {"145.500 MHz", "12.5 kHz", "NFM?", "-48", "+31 dB", "86 % of dwells", "2 m amateur FM"}, c));
    CHECK(std::fabs(c.freqHz - 145.5e6) < 1 && c.widthHz == 12500.0 && c.peakDb == -48.0 && c.overDb == 31.0 && c.seenPct == 86.0);
    CHECK(c.band == "2 m amateur FM" && c.guess == "NFM?" && c.freqText == "145.500 MHz" && c.seenText == "86 % of dwells");
    CHECK(c.haveWidth && c.havePeak && c.haveOver && c.haveSeen);
    // reordered and missing columns still read; a status row does not
    const std::vector<std::string> odd{"Seen", "Frequency", "Peak dB"};
    CHECK(channelOfCells(odd, {"41 %", "144.800 MHz", "-55"}, c) && std::fabs(c.freqHz - 144.8e6) < 1 && c.seenPct == 41.0 && c.peakDb == -55.0 && !c.haveWidth && !c.haveOver);
    CHECK(!channelOfCells(kHeads, {"Survey", "sweeping", "", "", "", "", ""}, c));
    CHECK(!channelOfCells(kHeads, {"Cannot sweep", "no radio open", "", "", "", "", ""}, c));
    CHECK(!channelOfCells({"Width"}, {"12 kHz"}, c));                                  // no frequency column
    CHECK(!channelOfCells(kHeads, {}, c));
}

void testGrid() {
    const std::vector<SurveyChannel> ch{chan(145.5, 12.5, -48, 31, 86), chan(144.8, 15, -55, 24, 41), chan(145.2, 3.1, -62, 17, 9)};
    // the floor is the median of peak - over: -79, -79, -79
    CHECK_NEAR(floorDb(ch), -79.0, 1e-9);
    CHECK_NEAR(floorDb({}), -110.0, 1e-9);
    CHECK_NEAR(floorDb({chan(145.0, 10, -50, 30, 50), chan(145.1, 10, -60, 10, 50), chan(145.2, 10, -40, 5, 50)}), -70.0, 1e-9);   // floors -80, -70, -35 (peak minus over): the median, not the lowest or the mean
    SurveyChannel nof;
    nof.havePeak = true;
    nof.peakDb = -40.0;
    CHECK_NEAR(floorDb({nof}), -50.0, 1e-9);                                           // no 'over floor': ten under the lowest peak
    double lo = 0, hi = 0;
    spanOf(ch, lo, hi);
    CHECK(lo < 144.8e6 && hi > 145.5e6);
    CHECK_NEAR(lo, 144.8e6 - std::max(50e3, 0.06 * 0.7e6), 1e-3);
    spanOf({chan(145.0, 10, -50, 20, 50)}, lo, hi);                                    // one channel gets 400 kHz
    CHECK_NEAR(hi - lo, 400e3, 1e-3);
    spanOf({}, lo, hi);
    CHECK(lo == 144.0e6 && hi == 148.0e6);
    double ll = 0, hh = 0;
    levelRange(ch, -79.0, ll, hh);
    CHECK(ll == -80.0 && hh == -40.0);                                                  // floor rounded down, peak -48 + 3 up to -40
    levelRange({}, -110.0, ll, hh);
    CHECK(ll == -110.0 && hh == -80.0);                                                 // never narrower than 30 dB
    CHECK(formatSurveyMHz(145.5e6, 4e6) == "145.5");                                  // a 1 MHz step: one decimal
    CHECK(formatSurveyMHz(145.5e6, 0.4e6) == "145.50");                               // a 100 kHz step: two
    CHECK(formatSurveyMHz(145.5e6, 0.04e6) == "145.500");                           // a 10 kHz step: three
    CHECK(formatSurveyMHz(145.5e6, 400.0) == "145.5000");                           // a 100 Hz step: four
    CHECK(formatSurveyMHz(145.5e6, 40e6) == "145.5");

    // the grid: the busiest channel's ridge is as high as its peak and runs the whole length of its Seen share; elsewhere the floor
    spanOf(ch, lo, hi);
    const int rows = 21, cols = 121;                                                    // occupancy in 5 % rows
    std::vector<float> g;
    ridgeGrid(ch, lo, hi, -79.0, rows, cols, g);
    CHECK(g.size() == static_cast<std::size_t>(rows * cols));
    const double colHz = (hi - lo) / (cols - 1);
    const int cMain = static_cast<int>(std::lround((145.5e6 - lo) / colHz));
    const int cLow = static_cast<int>(std::lround((144.8e6 - lo) / colHz));
    const auto at = [&](int r, int c2) { return g[static_cast<std::size_t>(r * cols + c2)]; };
    CHECK_NEAR(at(0, cMain), -48.0, 1e-4);                                              // the peak (the column nearest the centre)
    CHECK(at(17, cMain) > -60.0);                                                       // 85 % occupancy: still standing (86 % seen)
    CHECK_NEAR(at(18, cMain), -79.0, 1e-4);                                             // 90 %: the ridge has ended
    CHECK(at(8, cLow) > -70.0);                                                         // the 41 % channel stands at 40 %
    CHECK_NEAR(at(9, cLow), -79.0, 1e-4);                                               // ... and not at 45 %
    CHECK_NEAR(at(0, 3), -79.0, 1e-6);                                                  // far from any channel: the floor
    // a ridge is as wide as its width: 12.5 kHz about 145.5 MHz, a column is 5.8 kHz here, so it spans about 2-3 columns each side at most
    int wide = 0;
    for (int c2 = 0; c2 < cols; ++c2) { wide += at(0, c2) > -78.0f && std::fabs(lo + colHz * c2 - 145.5e6) < 1e6 ? 1 : 0; }
    CHECK(wide >= 3 && wide <= 9);
    // a channel with no peak but an 'over floor' stands over the floor; one with neither is not drawn
    SurveyChannel overOnly = chan(146.0, 10, 0, 20, 100);
    overOnly.havePeak = false;
    SurveyChannel nothing = chan(146.5, 10, 0, 0, 100);
    nothing.havePeak = nothing.haveOver = false;
    std::vector<float> g2;
    ridgeGrid({overOnly, nothing}, 145.0e6, 147.0e6, -80.0, 5, 101, g2);
    float maxv = -1e9f;
    for (float v : g2) { maxv = std::max(maxv, v); }
    CHECK_NEAR(maxv, -60.0, 1.5);
    // degenerate sizes
    std::vector<float> g3;
    ridgeGrid(ch, lo, hi, -79.0, 0, 0, g3);
    CHECK(g3.size() == 4u);
}

void testPaint() {
    s3::Scene scene;
    SurveyInput in;
    in.channels = {chan(145.5, 12.5, -48, 31, 86, "2 m amateur FM"), chan(144.8, 15, -55, 24, 41, "APRS"), chan(145.2, 3.1, -62, 17, 9)};
    in.camera = survey3dDefaultCamera();
    std::vector<s3::Rgba> px(480 * 320);
    const SurveyResult r = paintSurvey3d(scene, px.data(), 480, 320, in);
    CHECK(r.hasData);
    CHECK(r.triangles > 1500);
    CHECK(opaque(px) > 5000);
    CHECK_NEAR(r.floorDb, -79.0, 1e-9);
    CHECK(r.loDb == -80.0 && r.hiDb == -40.0);
    // 15 ticks, 3 captions, the three rank numbers
    CHECK(r.ranked == 3u);
    CHECK(find(r, "1") != nullptr && find(r, "2") != nullptr && find(r, "3") != nullptr);
    CHECK(find(r, "MHz") != nullptr && find(r, "Seen (%)") != nullptr && find(r, "dB") != nullptr);
    CHECK(find(r, "0") != nullptr && find(r, "100") != nullptr && find(r, "50") != nullptr);          // the occupancy ticks
    CHECK(find(r, "-80") != nullptr && find(r, "-40") != nullptr);                                      // the level ticks
    // rank 1 stands over the busiest channel: its label is higher on the screen than rank 3's (a lower, shorter ridge)
    const s3::Label3D* l1 = find(r, "1");
    const s3::Label3D* l3 = find(r, "3");
    if (l1 != nullptr && l3 != nullptr) { CHECK(l1->y != l3->y); CHECK(l1->kind == 2); }
    // more channels than the rank list: only the busiest six are numbered
    SurveyInput many = in;
    for (int i = 0; i < 12; ++i) { many.channels.push_back(chan(145.0 + 0.05 * i, 10, -60 - i, 20, 30)); }
    std::vector<s3::Rgba> pm(480 * 320);
    CHECK(paintSurvey3d(scene, pm.data(), 480, 320, many).ranked == 6u);
    // no channels: the box and nothing else, no words (there is no span to speak of)
    SurveyInput none;
    none.camera = survey3dDefaultCamera();
    std::vector<s3::Rgba> pn(480 * 320);
    const SurveyResult rn = paintSurvey3d(scene, pn.data(), 480, 320, none);
    CHECK(!rn.hasData && rn.triangles == 0u && rn.labels.empty());
    CHECK(opaque(pn) > 500);
    // determinism and the camera
    std::vector<s3::Rgba> a(480 * 320), b(480 * 320), c(480 * 320);
    paintSurvey3d(scene, a.data(), 480, 320, in);
    paintSurvey3d(scene, b.data(), 480, 320, in);
    CHECK(std::memcmp(a.data(), b.data(), a.size() * sizeof(s3::Rgba)) == 0);
    SurveyInput turned = in;
    turned.camera.azimuthDeg = 20.0f;
    paintSurvey3d(scene, c.data(), 480, 320, turned);
    CHECK(std::memcmp(a.data(), c.data(), a.size() * sizeof(s3::Rgba)) != 0);
    // a longer Seen share makes a bigger picture of the same channel
    SurveyInput brief = in;
    brief.channels = {chan(145.5, 12.5, -48, 31, 10)};
    SurveyInput full = in;
    full.channels = {chan(145.5, 12.5, -48, 31, 100)};
    std::vector<s3::Rgba> pb(480 * 320), pf(480 * 320);
    paintSurvey3d(scene, pb.data(), 480, 320, brief);
    paintSurvey3d(scene, pf.data(), 480, 320, full);
    CHECK(opaque(pf) != opaque(pb));
}

}  // namespace

int main() {
    testParse();
    testGrid();
    testPaint();
    return testSummary("test_slint_host_survey3d");
}
