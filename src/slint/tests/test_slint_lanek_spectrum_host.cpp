// Screen lane K's spectrum tools (src/slint/lanek_spectrum.hpp; docs/multi-radio-records/screen-k.md): the detector reduction (closed forms for white noise, the rfmon lane's own numbers), the limit
// mask lines, the eight cursors' tabs and the level under a cursor. No Slint, no engine.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <random>
#include <string>
#include <vector>

#include "lanek_spectrum.hpp"
#include "test_check.hpp"

using namespace foxsdr::slintlanek;
using cascade::dsp::RbwDetector;
using foxsdr::slinthost::Rgba;

namespace {

void detectors() {
    // a frame of 8 bins into 2 columns: [0..3] and [4..7]
    const float f[8] = {-50, -40, -60, -30, -20, -80, -70, -10};
    float o[2] = {};
    detectorColumns(f, 8, 2, RbwDetector::Peak, o);    CHECK(o[0] == -30.0f && o[1] == -10.0f);
    detectorColumns(f, 8, 2, RbwDetector::NegPeak, o); CHECK(o[0] == -60.0f && o[1] == -80.0f);
    detectorColumns(f, 8, 2, RbwDetector::AvgLog, o);  CHECK_NEAR(o[0], -45.0, 1e-4); CHECK_NEAR(o[1], -45.0, 1e-4);
    detectorColumns(f, 8, 2, RbwDetector::Sample, o);  CHECK(o[0] == -60.0f && o[1] == -70.0f);   // the bin under the column's middle
    // RMS is the mean POWER: two bins at 0 dB and a floor bin read 10 log10(0.5) = -3.01 dB
    const float g[2] = {0.0f, -200.0f};
    detectorColumns(g, 2, 1, RbwDetector::Rms, o);     CHECK_NEAR(o[0], -3.0103, 1e-3);
    detectorColumns(g, 2, 1, RbwDetector::AvgVolt, o); CHECK_NEAR(o[0], -6.0206, 1e-3);   // (mean of the amplitudes)^2 = 0.25
    // a steady tone: every detector reads the same
    std::vector<float> tone(16, -33.0f);
    for (int d = 0; d < 6; ++d) { float c[1]; detectorColumns(tone.data(), 16, 1, static_cast<RbwDetector>(d), c); CHECK_NEAR(c[0], -33.0, 1e-4); }
    // white noise: one bin's power is exponential; against the RMS (the true mean power) the voltage average reads 10log10(pi/4) = -1.05 dB and the log average -2.51 dB
    std::mt19937 rng(12345);
    std::exponential_distribution<double> expo(1.0);
    std::vector<float> noise(200000);
    for (auto& v : noise) { v = static_cast<float>(10.0 * std::log10(expo(rng))); }
    float rms, av, al;
    detectorColumns(noise.data(), noise.size(), 1, RbwDetector::Rms, &rms);
    detectorColumns(noise.data(), noise.size(), 1, RbwDetector::AvgVolt, &av);
    detectorColumns(noise.data(), noise.size(), 1, RbwDetector::AvgLog, &al);
    CHECK_NEAR(rms, 0.0, 0.05);
    CHECK_NEAR(av - rms, -1.05, 0.06);
    CHECK_NEAR(al - rms, -2.51, 0.06);
    // edges: more columns than bins repeats a bin; non-finite reads as the floor; nothing in gives the floor out; null and zero do not crash
    float wide[10];
    detectorColumns(f, 8, 10, RbwDetector::Peak, wide);
    for (float w : wide) { CHECK(w >= -80.0f && w <= -10.0f); }
    const float nanFrame[3] = {std::nanf(""), std::nanf(""), std::nanf("")};
    detectorColumns(nanFrame, 3, 1, RbwDetector::Peak, o); CHECK(o[0] == -200.0f);
    detectorColumns(nullptr, 0, 2, RbwDetector::Peak, o);  CHECK(o[0] == -200.0f && o[1] == -200.0f);
    detectorColumns(f, 8, 0, RbwDetector::Peak, o);
    detectorColumns(f, 8, 2, RbwDetector::Peak, nullptr);
    // the RBW readout: the Blackman-Harris window's ENBW is about 2.0 bins
    CHECK_NEAR(nativeRbwHz(2.4e6, 4096), 2.0044 * 2.4e6 / 4096.0, 1.0);
    CHECK(nativeRbwHz(0, 4096) == 0.0 && nativeRbwHz(2.4e6, 1) == 0.0);
    CHECK(rbwText(0) == "n/a" && rbwText(341) == "341 Hz" && rbwText(2070) == "2.07 kHz" && rbwText(1.2e6) == "1.20 MHz");
}

void mask() {
    cascade::core::Mask m;
    std::string err;
    CHECK(parseMaskText("foxmask 1\nupper 88e6 -30 108e6 -30\nlower 90e6 -100 100e6 -60\n# a comment\n", m, err));
    CHECK(m.segments.size() == 2);
    double db = 0;
    CHECK(maskLimitAt(m, 100e6, true, db) && db == -30.0);
    CHECK(!maskLimitAt(m, 110e6, true, db));                    // outside every upper line
    CHECK(maskLimitAt(m, 95e6, false, db) && std::fabs(db - (-80.0)) < 1e-9);   // halfway along the lower line
    CHECK(!parseMaskText("not a mask", m, err) && !err.empty());
    CHECK(!parseMaskText("foxmask 1\nupper 1 2 3\n", m, err));
    // the lowest upper line limits where several cover; the highest lower line
    CHECK(parseMaskText("foxmask 1\nupper 1e6 -10 2e6 -10\nupper 1e6 -20 2e6 -20\nlower 1e6 -90 2e6 -90\nlower 1e6 -70 2e6 -70\n", m, err));
    CHECK(maskLimitAt(m, 1.5e6, true, db) && db == -20.0);
    CHECK(maskLimitAt(m, 1.5e6, false, db) && db == -70.0);
    // painting: the upper limit is red at the row of its level, over the span only
    CHECK(parseMaskText("foxmask 1\nupper 1e6 -30 2e6 -30\n", m, err));
    const std::uint32_t w = 100, h = 100;
    std::vector<Rgba> px(static_cast<std::size_t>(w) * h, Rgba{0, 0, 0, 0});
    paintMask(px.data(), w, h, 0.5e6, 2.5e6, -100.0, 0.0, m);
    const std::size_t row = static_cast<std::size_t>(std::lround((1.0 - 0.7) * 99.0));   // -30 dB in [-100, 0]
    CHECK(px[row * w + 50].r == 255 && px[row * w + 50].g == 80);                          // 1.5 MHz is column 50
    CHECK(px[row * w + 5].a == 0);                                                         // 0.6 MHz: before the line starts
    CHECK(px[row * w + 90].a == 0);                                                        // 2.3 MHz: after it ends
    paintMask(nullptr, w, h, 0, 1, -100, 0, m);                                            // null / degenerate: no crash
    paintMask(px.data(), 1, 1, 0, 1, -100, 0, m);
    paintMask(px.data(), w, h, 1, 1, -100, 0, m);
}

void cursors() {
    const std::uint32_t w = 200, h = 60;
    std::vector<Rgba> px(static_cast<std::size_t>(w) * h, Rgba{0, 0, 0, 0});
    std::array<double, 8> f{};
    std::array<bool, 8> shown{};
    for (std::size_t i = 0; i < 8; ++i) { f[i] = 1e6 + 1e5 * static_cast<double>(i); shown[i] = i < 3; }
    f[2] = 9e6;   // outside the span
    paintCursors(px.data(), w, h, 1e6, 2e6, f, shown);
    // cursor 0 is at the left edge (its tab clipped), cursor 1 at 10 % of the width
    CHECK(px[0 * w + 0].a == 255 && px[0 * w + 0].r == 255 && px[0 * w + 0].g == 214);
    const std::size_t x1 = static_cast<std::size_t>(std::lround(0.1 * 199.0));
    CHECK(px[0 * w + x1].a == 255);
    CHECK(px[10 * w + x1].a == 255 && px[11 * w + x1].a == 0);                                // dotted: every second row
    CHECK(px[3 * w + x1 + 2].a == 255 && px[8 * w + x1 + 2].a == 0);                          // the 7 x 7 tab at the top, nothing beside the line below it
    // nothing shown, nothing drawn
    std::fill(px.begin(), px.end(), Rgba{0, 0, 0, 0});
    shown.fill(false);
    paintCursors(px.data(), w, h, 1e6, 2e6, f, shown);
    for (const auto& p : px) { CHECK(p.a == 0); }
    paintCursors(nullptr, w, h, 1e6, 2e6, f, shown);
    paintCursors(px.data(), 4, 4, 1e6, 2e6, f, shown);
    // the level under a cursor: the strongest bin within the search width (span 1 MHz, centre 0: bin i is at i * 1000 - 500000 Hz)
    std::vector<float> frame(1000, -100.0f);
    frame[500] = -40.0f;
    CHECK_NEAR(cursorLevel(frame.data(), frame.size(), 0.0, 1e6, 500.0, 0.0), -40.0, 1e-6);          // bin 500 covers 0 to 1000 Hz
    CHECK_NEAR(cursorLevel(frame.data(), frame.size(), 0.0, 1e6, 3500.0, 3000.0), -40.0, 1e-6);      // within 3 bins
    CHECK_NEAR(cursorLevel(frame.data(), frame.size(), 0.0, 1e6, 10500.0, 1000.0), -100.0, 1e-6);
    CHECK(std::isnan(cursorLevel(frame.data(), frame.size(), 0.0, 1e6, 9e6, 1000.0)));               // outside the frame
    CHECK(std::isnan(cursorLevel(nullptr, 0, 0.0, 1e6, 0.0, 0.0)));
}

}  // namespace

int main() {
    detectors();
    mask();
    cursors();
    return testSummary("test_slint_lanek_spectrum_host");
}
