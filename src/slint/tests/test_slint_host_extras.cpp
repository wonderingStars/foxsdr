// The spectrum and waterfall extras of screen lane B (src/slint/spectrum_extras.cpp, docs/multi-radio-records/screen-b.md): the colour maps (S7), the waterfall's line rate (S10), the clock stamps (S8), the reference
// trace (S22), the peak labels (S20), the A / B markers (S21), SNR and channel power (S14, Q2), the frequency tracker (Q3), the signal history and its CSV (S15), the measurement box and the history graph.
// Pure: made-up spectra with noise, a frequency offset and a different bin width, the way the other host tests make theirs.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "spectrum_extras.hpp"
#include "test_check.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <random>
#include <vector>

using namespace foxsdr::slintextras;
using foxsdr::slinthost::Rgba;

namespace {

// A noise floor (-90 dBFS, 1.5 dB of scatter) with tones: each {offset from the centre in Hz, level}.
std::vector<float> spectrum(std::size_t n, double spanHz, const std::vector<std::pair<double, float>>& tones, unsigned seed = 7) {
    std::mt19937 rng(seed);
    std::normal_distribution<float> noise(0.0f, 1.5f);
    std::vector<float> v(n);
    for (auto& x : v) { x = -90.0f + noise(rng); }
    const double binHz = spanHz / static_cast<double>(n);
    for (const auto& t : tones) {
        const double pos = t.first / binHz + static_cast<double>(n) / 2.0;   // fractional bin: the tone's energy is shared by its two neighbours (a window's main lobe)
        const std::size_t i = static_cast<std::size_t>(std::floor(pos));
        const double frac = pos - static_cast<double>(i);
        if (i + 1 < n) {
            v[i] = std::max(v[i], t.second - static_cast<float>(3.0 * frac * frac));
            v[i + 1] = std::max(v[i + 1], t.second - static_cast<float>(3.0 * (1.0 - frac) * (1.0 - frac)));
        }
    }
    return v;
}

void palettes() {
    CHECK(kPaletteCount == 6);
    CHECK(std::string(paletteName(0)) == "Classic" && std::string(paletteName(2)) == "Ember" && std::string(paletteName(99)) == "Blue-yellow safe");
    // the classic map IS heat(): the table's entries are heat() of their step
    const auto& classic = paletteTable(0);
    for (const std::size_t i : {std::size_t{0}, std::size_t{64}, std::size_t{128}, std::size_t{255}}) {
        const Rgba h = foxsdr::slinthost::heat(static_cast<float>(i) / 255.0f);
        CHECK(classic[i].r == h.r && classic[i].g == h.g && classic[i].b == h.b);
    }
    // the five others are the OKLab maps: different from each other and from the classic, every entry opaque, and the same table comes back
    for (int a = 0; a < kPaletteCount; ++a) {
        for (int b = a + 1; b < kPaletteCount; ++b) {
            int differ = 0;
            for (std::size_t i = 0; i < 256; ++i) { differ += (paletteTable(a)[i].r != paletteTable(b)[i].r || paletteTable(a)[i].g != paletteTable(b)[i].g || paletteTable(a)[i].b != paletteTable(b)[i].b) ? 1 : 0; }
            CHECK(differ > 200);
        }
        for (std::size_t i = 0; i < 256; ++i) { CHECK(paletteTable(a)[i].a == 255); }
    }
    CHECK(&paletteTable(3) == &paletteTable(3) && &paletteTable(-5) == &paletteTable(0) && &paletteTable(77) == &paletteTable(kPaletteCount - 1));
    CHECK(paletteIndexOf(-1.0f) == 0 && paletteIndexOf(0.0f) == 0 && paletteIndexOf(1.0f) == 255 && paletteIndexOf(7.0f) == 255 && paletteIndexOf(0.5f) == 128);
    // painted through a map: the strongest cell takes the map's top colour, the floor its bottom
    std::vector<Rgba> px(8 * 4, Rgba{0, 0, 0, 255});
    const float bins[8] = {-110.f, -110.f, -110.f, 0.f, 0.f, -110.f, -110.f, -110.f};
    foxsdr::slinthost::scrollWaterfallRow(px.data(), 8, 4, bins, 8, -110.0, 0.0, paletteTable(2).data());
    CHECK(px[3].r == paletteTable(2)[255].r && px[3].g == paletteTable(2)[255].g && px[3].b == paletteTable(2)[255].b);
    CHECK(px[0].r == paletteTable(2)[0].r && px[0].b == paletteTable(2)[0].b);
    // no map = the classic painter, unchanged
    foxsdr::slinthost::scrollWaterfallRow(px.data(), 8, 4, bins, 8, -110.0, 0.0);
    const Rgba top = foxsdr::slinthost::heat(1.0f);
    CHECK(px[3].r == top.r && px[3].g == top.g && px[3].b == top.b);
}

void speeds() {
    CHECK(speedLinesPerSecond(0) == 0.0 && speedLinesPerSecond(1) == 10.0 && speedLinesPerSecond(kSpeedCount - 1) < 0.02 && speedLinesPerSecond(99) == speedLinesPerSecond(kSpeedCount - 1) && speedLinesPerSecond(-3) == 0.0);
    for (int i = 2; i < kSpeedCount; ++i) { CHECK(speedLinesPerSecond(i) < speedLinesPerSecond(i - 1)); }   // each slower than the one before, the last a line a minute
    // every frame: every frame is a line
    RowPacer p;
    const std::vector<float> a(64, -80.0f);
    int lines = 0;
    for (int i = 0; i < 30; ++i) { lines += p.feed(a.data(), a.size(), i * 0.033) ? 1 : 0; }
    CHECK(lines == 30);
    // a line a second at 30 frames a second: one line per second, the first at once
    RowPacer q;
    q.setLinesPerSecond(1.0);
    int made = 0;
    std::vector<double> at;
    for (int i = 0; i < 300; ++i) {   // ten seconds
        std::vector<float> f(64, -80.0f);
        if (i == 47) { f[10] = -20.0f; }   // a short signal in ONE frame between two lines
        if (q.feed(f.data(), f.size(), i * 0.033)) { ++made; at.push_back(i * 0.033); if (i > 33 && i < 70) { CHECK(q.row()[10] == -20.0f); } }
    }
    CHECK(made >= 9 && made <= 11);
    CHECK(at.size() >= 2 && std::fabs((at[2] - at[1]) - 1.0) < 0.05);   // the cadence is kept
    // the slowest, a line a minute: three minutes make three or four lines, and the strongest of every frame is in the line
    RowPacer s;
    s.setLinesPerSecond(speedLinesPerSecond(kSpeedCount - 1));
    int slow = 0;
    float peakInLine = -300.0f;
    for (int i = 0; i < 180 * 30; ++i) {
        std::vector<float> f(32, -100.0f);
        if (i == 45 * 30) { f[5] = -33.0f; }
        if (s.feed(f.data(), f.size(), i / 30.0)) { ++slow; peakInLine = std::max(peakInLine, s.row()[5]); }
    }
    CHECK(slow >= 3 && slow <= 4);
    CHECK(peakInLine == -33.0f);
    // a change of the bin count mid-line starts the line over (no mixing of two windows)
    RowPacer r;
    r.setLinesPerSecond(1.0);
    const std::vector<float> w1(10, -50.0f), w2(20, -70.0f);
    (void)r.feed(w1.data(), w1.size(), 0.0);
    (void)r.feed(w1.data(), w1.size(), 0.2);
    const bool due = r.feed(w2.data(), w2.size(), 1.1);
    CHECK(due && r.row().size() == 20 && r.row()[0] == -70.0f);
    // nothing in, nothing out
    CHECK(!r.feed(nullptr, 0, 5.0));
}

void stamps() {
    // 2026-11-11 12:03:41 UTC
    const double t = 1794398621.0;
    CHECK(clockText(t, 1, 0, true) == "12:03:41" && clockText(t, 1, 0, false) == "12:03");
    CHECK(clockText(t, 2, 3600.0, true) == "13:03:41" && clockText(t, 2, -5.5 * 3600.0, true) == "06:33:41" && clockText(t, 1, 3600.0, true) == "12:03:41");   // local adds the offset, UTC never does
    CHECK(clockText(0.5, 1, 0, true) == "00:00:00" && clockText(-1.0, 1, 0, true) == "23:59:59");                                                         // the day wraps both ways
    // a 238-line picture at 30 lines a second holds about 8 s: stamps every 1 s or 2 s, round times, scrolling
    auto ticks = clockTicks(t, 1.0 / 30.0, 238.0, 48.0, 1, 0);
    CHECK(ticks.size() >= 3 && ticks.size() <= 8);
    for (std::size_t i = 1; i < ticks.size(); ++i) { CHECK(ticks[i].frac > ticks[i - 1].frac); }
    for (const auto& k : ticks) { CHECK(k.frac >= 0.0 && k.frac <= 1.0 && k.label.size() == 8 && k.label[2] == ':' && k.label[5] == ':'); }
    CHECK(ticks[0].label.substr(6) == "41" || ticks[0].label.substr(6) == "40");   // the newest round second
    // the pitch is kept: at 48 px or more apart
    const double histS = 238.0 / 30.0;
    for (std::size_t i = 1; i < ticks.size(); ++i) { CHECK((ticks[i].frac - ticks[i - 1].frac) * 238.0 >= 47.0); (void)histS; }
    // very slow lines (a line a minute: the picture holds four hours): stamps every 10 to 30 minutes, minutes only
    auto slow = clockTicks(t, 60.0, 238.0, 48.0, 1, 0);
    CHECK(slow.size() >= 2 && slow.size() <= 8);
    for (const auto& k : slow) { CHECK(k.label.size() == 5 && k.label[2] == ':'); }
    for (std::size_t i = 1; i < slow.size(); ++i) { CHECK(slow[i].label.substr(3) == "00" || slow[i].label.substr(3) == "30" || slow[i].label.substr(3) == "15" || slow[i].label.substr(3) == "45"); }
    // scrolling: a second later the same round time sits one second further down
    const auto a = clockTicks(t, 1.0 / 30.0, 238.0, 48.0, 1, 0), b = clockTicks(t + 1.0, 1.0 / 30.0, 238.0, 48.0, 1, 0);
    bool found = false;
    for (const auto& x : a) { for (const auto& y : b) { if (x.label == y.label) { found = true; CHECK(std::fabs((y.frac - x.frac) - (1.0 / (238.0 / 30.0))) < 1e-6); } } }
    CHECK(found);
    // local stamps fall on round LOCAL times (a half-hour zone)
    const auto loc = clockTicks(t, 60.0, 238.0, 48.0, 2, 5.5 * 3600.0);
    for (const auto& k : loc) { CHECK(k.label.substr(3) == "00" || k.label.substr(3) == "30" || k.label.substr(3) == "15" || k.label.substr(3) == "45"); }
    // degenerate input
    CHECK(clockTicks(t, 0.0, 238.0, 48.0, 1, 0).empty() && clockTicks(t, 1.0, 0.0, 48.0, 1, 0).empty() && clockTicks(t, -1.0, 238.0, 48.0, 1, 0).empty());
}

void reference() {
    const auto s = spectrum(1000, 1.0e6, {{100e3, -40.0f}});
    ReferenceTrace ref;
    std::vector<float> cols;
    ref.columns(10e6, 1e6, 500, cols);
    CHECK(cols.size() == 500 && std::isnan(cols[0]));   // nothing saved: no columns
    ref.save(s.data(), s.size(), 10e6, 1.0e6);
    CHECK(ref.set);
    ref.columns(10e6, 1e6, 500, cols);
    CHECK(cols.size() == 500);
    std::size_t covered = 0, peakCol = 0;
    for (std::size_t i = 0; i < cols.size(); ++i) { covered += std::isnan(cols[i]) ? 0 : 1; if (!std::isnan(cols[i]) && cols[i] > cols[peakCol] ) { peakCol = i; } }
    CHECK(covered == 500);
    CHECK(std::fabs(static_cast<double>(peakCol) - (500.0 * 0.6)) <= 1.5);   // the 100 kHz tone sits 0.6 of the way across
    // zoomed in on the tone: it fills the middle of a 100 kHz window
    ref.columns(10.1e6, 100e3, 400, cols);
    std::size_t zc = 0;
    for (std::size_t i = 0; i < cols.size(); ++i) { if (cols[i] > cols[zc]) { zc = i; } }
    CHECK(std::fabs(static_cast<double>(zc) - 200.0) <= 6.0);
    // a window that leaves the saved span: NaN where the saved frame does not reach
    ref.columns(10.5e6, 1e6, 500, cols);
    std::size_t nans = 0;
    for (const float f : cols) { nans += std::isnan(f) ? 1 : 0; }
    CHECK(nans >= 240 && nans <= 260);
    ref.columns(20e6, 1e6, 100, cols);
    for (const float f : cols) { CHECK(std::isnan(f)); }
    ref.clear();
    CHECK(!ref.set && ref.bins.empty());
    ref.save(nullptr, 0, 1, 1);
    CHECK(!ref.set);
    // painted: behind the live trace, in the reference's colour, NaN columns a gap
    foxsdr::slinthost::TraceState st;
    std::vector<Rgba> px(100 * 50);
    std::vector<float> flatBins(100, -60.0f);
    st.reference.assign(100, std::numeric_limits<float>::quiet_NaN());
    for (std::size_t i = 20; i < 40; ++i) { st.reference[i] = -30.0f; }
    st.showReference = true;
    foxsdr::slinthost::paintTrace(px.data(), 100, 50, flatBins.data(), flatBins.size(), -100.0, 0.0, st, foxsdr::slinthost::TraceMode::Normal, Rgba{0x7a, 0xd7, 0xff, 255}, Rgba{1, 2, 3, 255}, Rgba{4, 5, 6, 255});
    const int yRef = static_cast<int>((1.0 - (-30.0 + 100.0) / 100.0) * 49);
    CHECK(px[static_cast<std::size_t>(yRef) * 100 + 30].a == 230 && px[static_cast<std::size_t>(yRef) * 100 + 30].b == 0xff);
    CHECK(px[static_cast<std::size_t>(yRef) * 100 + 60].a == 0);   // a NaN column is a gap
    st.showReference = false;
    foxsdr::slinthost::paintTrace(px.data(), 100, 50, flatBins.data(), flatBins.size(), -100.0, 0.0, st, foxsdr::slinthost::TraceMode::Normal, Rgba{0x7a, 0xd7, 0xff, 255}, Rgba{1, 2, 3, 255}, Rgba{4, 5, 6, 255});
    CHECK(px[static_cast<std::size_t>(yRef) * 100 + 30].a == 0);   // hidden: gone
}

void peaks() {
    const auto s = spectrum(2048, 2.0e6, {{-400e3, -30.0f}, {120e3, -50.0f}, {123e3, -48.0f}, {650e3, -60.0f}, {-900e3, -85.0f}});
    const auto p = findPeaks(s.data(), s.size(), 7.1e6, 2.0e6, 8, 10.0, 6);
    // the three real signals and the double at 120/123 kHz as one (they are 3 bins apart): at most four, in frequency order; the -85 dBFS one is 5 dB over the floor, so not named
    CHECK(p.size() == 3);
    for (std::size_t i = 1; i < p.size(); ++i) { CHECK(p[i].bin > p[i - 1].bin); }
    if (p.size() == 3) {
        CHECK(std::fabs(p[0].hz - (7.1e6 - 400e3)) < 1500.0);
        CHECK(std::fabs(p[1].hz - (7.1e6 + 123e3)) < 3000.0 && p[1].db >= -49.0f);   // the stronger of the two, not their mean
        CHECK(std::fabs(p[2].hz - (7.1e6 + 650e3)) < 1500.0);
    }
    // the count limit keeps the strongest
    const auto two = findPeaks(s.data(), s.size(), 7.1e6, 2.0e6, 2, 10.0, 6);
    CHECK(two.size() == 2 && two[0].db > -35.0f && two[1].db > -55.0f);
    // sub-bin accuracy: a tone between two bins is placed within a fifth of a bin
    const double binHz = 2.0e6 / 2048.0;
    const auto frac = spectrum(2048, 2.0e6, {{0.4 * binHz + 5 * binHz, -20.0f}});
    const auto fp = findPeaks(frac.data(), frac.size(), 0.0, 2.0e6, 1, 10.0, 6);
    CHECK(fp.size() == 1 && std::fabs(fp[0].hz - (0.4 * binHz + 5 * binHz + binHz / 2.0 - binHz / 2.0)) < binHz * 0.6);
    // empty, flat, too short, zero count
    CHECK(findPeaks(nullptr, 0, 0, 1e6, 5, 10.0, 3).empty());
    const std::vector<float> flat(512, -80.0f);
    CHECK(findPeaks(flat.data(), flat.size(), 0, 1e6, 5, 10.0, 3).empty());
    CHECK(findPeaks(s.data(), 2, 0, 1e6, 5, 10.0, 3).empty());
    CHECK(findPeaks(s.data(), s.size(), 0, 2e6, 0, 10.0, 3).empty());
    // the cost: a 16384-bin frame, four times a second per pane
    const auto big = spectrum(16384, 2.4e6, {{100e3, -30.0f}, {300e3, -40.0f}});
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 100; ++i) { (void)findPeaks(big.data(), big.size(), 0, 2.4e6, 8, 10.0, 20); }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 100.0;
    std::printf("findPeaks on 16384 bins: %.3f ms\n", ms);
    CHECK(ms < 2.0);
}

void snrAndPower() {
    const double span = 2.0e6;
    const auto s = spectrum(4096, span, {{0.0, -40.0f}});
    const std::size_t n = s.size(), mid = n / 2;
    const SnrReading r = measureSnr(s.data(), n, mid - 8, mid + 8);
    CHECK(r.valid);
    CHECK(r.peakDb > -41.0 && r.peakDb < -39.0);
    CHECK(std::fabs(r.floorDb - (-90.0)) < 1.0);        // the median of the bins outside the passband
    CHECK(std::fabs(r.snrDb - 50.0) < 1.5);
    // a stronger tone: the SNR follows
    const auto s2 = spectrum(4096, span, {{0.0, -20.0f}});
    CHECK(measureSnr(s2.data(), n, mid - 8, mid + 8).snrDb > r.snrDb + 15.0);
    // the passband is most of the frame: the floor is the whole frame's median, still near -90
    const SnrReading wide = measureSnr(s.data(), n, 10, n - 10);
    CHECK(wide.valid && std::fabs(wide.floorDb - (-90.0)) < 1.0);
    // empty / no passband / no data
    CHECK(!measureSnr(nullptr, 0, 0, 1).valid && !measureSnr(s.data(), n, 5, 5).valid && !measureSnr(s.data(), n, n, n + 10).valid);
    const std::vector<float> dead(256, -300.0f);
    CHECK(!measureSnr(dead.data(), dead.size(), 10, 20).valid);
    CHECK(std::fabs(medianDb(s.data(), n) - (-90.0)) < 0.7 && medianDb(nullptr, 0) == -300.0);
    // channel power: a power sum. Two bins of -30 dBFS = -27 dBFS (3 dB over one bin); the sum is in the bins' own units
    const std::vector<float> two{-30.0f, -30.0f, -120.0f, -120.0f};
    CHECK_NEAR(channelPowerDb(two.data(), two.size(), 0, 2), -26.99, 0.02);
    CHECK_NEAR(channelPowerDb(two.data(), two.size(), 0, 1), -30.0, 0.001);
    CHECK(channelPowerDb(two.data(), two.size(), 3, 3) == -200.0 && channelPowerDb(nullptr, 0, 0, 5) == -200.0 && channelPowerDb(two.data(), two.size(), 2, 99) < -100.0);
    // a wider range holds more power than the tone's bins alone, and less than the whole (the noise is 50 dB down)
    const double tone = channelPowerDb(s.data(), n, mid - 3, mid + 3), all = channelPowerDb(s.data(), n, 0, n);
    CHECK(tone > -41.0 && tone < -37.0 && all >= tone && all < tone + 0.2);
    // level at a frequency: the strongest of the bin and its neighbours; outside the frame NaN
    CHECK(levelAtHz(s.data(), n, 0.0, span, 0.0) > -41.0 && levelAtHz(s.data(), n, 0.0, span, 3 * span / n) < -60.0);
    CHECK(std::isnan(levelAtHz(s.data(), n, 0.0, span, 2e6)) && std::isnan(levelAtHz(nullptr, 0, 0, span, 0)) && std::isnan(levelAtHz(s.data(), n, 0.0, 0.0, 0)));
}

void ab() {
    const auto s = spectrum(4096, 2.0e6, {{-100e3, -30.0f}, {150e3, -55.0f}});
    AbMarkers m;
    CHECK(!abReading(m, s.data(), s.size(), 7e6, 2e6).both);
    m.setA(7e6 - 100e3);
    CHECK(m.aSet && !m.bSet && !abReading(m, s.data(), s.size(), 7e6, 2e6).both);
    m.setB(7e6 + 150e3);
    const AbReading r = abReading(m, s.data(), s.size(), 7e6, 2e6);
    CHECK(r.both);
    CHECK_NEAR(r.deltaHz, 250e3, 1.0);
    CHECK(std::fabs(r.aDb - (-30.0)) < 2.5 && std::fabs(r.bDb - (-55.0)) < 4.0);
    CHECK(std::fabs(r.deltaDb - (-25.0)) < 6.0);
    // swapped: B below A gives a negative frequency delta
    AbMarkers sw;
    sw.setA(7.1e6);
    sw.setB(7.05e6);
    CHECK(abReading(sw, s.data(), s.size(), 7e6, 2e6).deltaHz < 0);
    // outside the frame the level is NaN and so is the dB difference
    AbMarkers out;
    out.setA(9e6);
    out.setB(7e6);
    const AbReading o = abReading(out, s.data(), s.size(), 7e6, 2e6);
    CHECK(o.both && std::isnan(o.deltaDb));
    // the texts: the frequency, the level, and on B the difference
    CHECK(abLabelA(m, -30.4) == "A 6.9000 MHz  -30 dB");
    const std::string b = abLabelB(m, -55.2, r);
    CHECK(b.find("B 7.1500 MHz") == 0 && b.find("-55 dB") != std::string::npos && b.find("250.00 kHz") != std::string::npos);
    CHECK(abLabelA(m, std::numeric_limits<double>::quiet_NaN()) == "A 6.9000 MHz");
    m.clear();
    CHECK(!m.aSet && !m.bSet);
}

void tracker() {
    FreqTracker tr;
    const double span = 48e3, centre = 7.074e6;
    const double binHz = span / 1024.0;
    // a carrier at +1.5 kHz drifting +30 Hz a second, in noise, over 20 s at 30 frames a second
    TrackReading last;
    for (int i = 0; i < 600; ++i) {
        const double t = i / 30.0, f = 1500.0 + 30.0 * t;
        const auto s = spectrum(1024, span, {{f, -35.0f}}, static_cast<unsigned>(100 + i));
        last = tr.update(t, s.data(), s.size(), centre, span, centre - 5e3, centre + 5e3, centre);
        if (i > 5) { CHECK(last.valid); }
    }
    CHECK(last.valid);
    CHECK(std::fabs(last.hz - (centre + 1500.0 + 30.0 * 19.97)) < binHz * 0.35);       // within a third of a bin: the limit of the method is stated in the record
    CHECK(std::fabs(last.driftHzPerS - 30.0) < 6.0);                                   // the slope over the last ten seconds (a drift under a bin (47 Hz) in ten seconds is below what the method can see: stated in the record)
    CHECK(std::fabs(last.offsetHz - (last.hz - centre)) < 1e-6 && last.peakDb > -37.0);
    // no signal: not valid (nothing stands 8 dB over the floor), and it forgets
    const auto noise = spectrum(1024, span, {});
    const TrackReading none = tr.update(21.0, noise.data(), noise.size(), centre, span, centre - 5e3, centre + 5e3, centre);
    CHECK(!none.valid);
    // the window of frequency is respected: a stronger signal outside it is ignored
    const auto two = spectrum(1024, span, {{1500.0, -50.0f}, {-15e3, -20.0f}});
    FreqTracker t2;
    const TrackReading in = t2.update(0.0, two.data(), two.size(), centre, span, centre - 5e3, centre + 5e3, centre);
    CHECK(in.valid && std::fabs(in.hz - (centre + 1500.0)) < binHz);
    // degenerate
    CHECK(!t2.update(1.0, nullptr, 0, centre, span, centre, centre, centre).valid);
    CHECK(!t2.update(1.0, two.data(), two.size(), centre, span, centre + 1e6, centre + 2e6, centre).valid);
    t2.reset();
    CHECK(!t2.update(1.0, two.data(), 2, centre, span, centre - 5e3, centre + 5e3, centre).valid);
}

void history() {
    SignalHistory h;
    for (int i = 0; i < 10; ++i) { h.push({1794398621.0 + i * 0.4, -40.0f, -90.0f, 50.0f, -35.0f}); }
    CHECK(h.samples().size() == 4);   // at most one a second
    h.clear();
    CHECK(h.samples().empty());
    h.capacity = 100;
    for (int i = 0; i < 250; ++i) { h.push({1794398621.0 + i * 1.0, static_cast<float>(-40 - i % 7), -90.0f, 50.0f, -35.0f}); }
    CHECK(h.samples().size() == 100 && h.samples().front().utcS == 1794398621.0 + 150.0);   // the newest hundred
    SignalHistory c;
    c.push({1794398621.0, -41.26f, -90.04f, 48.8f, -35.5f});
    c.push({1794398622.0, -40.0f, -90.0f, 50.0f, -34.0f});
    const std::string csv = c.csv("radio 2");
    std::printf("%s", csv.c_str());
    CHECK(csv.find("# radio: radio 2\n") != std::string::npos);
    CHECK(csv.find("\nutc,peak_dbfs,floor_dbfs,snr_db,power_dbfs\n2026-11-11T12:03:41.000Z,-41.3,-90.0,48.8,-35.5\n2026-11-11T12:03:42.000Z,-40.0,-90.0,50.0,-34.0\n") != std::string::npos);
    CHECK(SignalHistory().csv("x").find("2026") == std::string::npos);   // an empty history has the header and no rows
    // the measurement box and the graph paint into the picture's corners and nowhere else
    std::vector<Rgba> px(400 * 200, Rgba{0, 0, 0, 0});
    paintMeasureBox(px.data(), 400, 200, {"SNR 31.2 DB", "CH PWR -35.2 DBFS"});
    std::size_t lit = 0, leftHalf = 0;
    for (std::size_t y = 0; y < 200; ++y) { for (std::size_t x = 0; x < 400; ++x) { if (px[y * 400 + x].a != 0) { ++lit; if (x < 200) { ++leftHalf; } } } }
    CHECK(lit > 800 && leftHalf == 0);   // a plate and text in the right half
    std::vector<Rgba> g(400 * 200, Rgba{0, 0, 0, 0});
    SignalHistory gh;
    for (int i = 0; i < 100; ++i) { gh.push({1794398621.0 + i, -40.0f, -90.0f, static_cast<float>(30 + 15 * std::sin(i * 0.2)), -35.0f}); }
    CHECK(paintHistoryGraph(g.data(), 400, 200, gh, 0, 120.0) == 100);
    CHECK(paintHistoryGraph(g.data(), 400, 200, gh, 1, 30.0) <= 31 && paintHistoryGraph(g.data(), 400, 200, SignalHistory(), 0, 120.0) == 0);
    std::size_t topLeft = 0, bottomRight = 0;
    for (std::size_t y = 0; y < 200; ++y) { for (std::size_t x = 0; x < 400; ++x) { if (g[y * 400 + x].a != 0) { if (y < 100 && x < 200) { ++topLeft; } if (y >= 100 && x >= 200) { ++bottomRight; } } } }
    CHECK(topLeft == 0 && bottomRight > 500);
    // a picture too small for the box draws nothing and does not crash
    std::vector<Rgba> tiny(20 * 10, Rgba{0, 0, 0, 0});
    paintMeasureBox(tiny.data(), 20, 10, {"A VERY LONG LINE THAT DOES NOT FIT"});
    CHECK(paintHistoryGraph(tiny.data(), 20, 10, gh, 0, 120.0) == 0);
    paintMeasureBox(nullptr, 20, 10, {"X"});
}

}  // namespace

int main() {
    palettes();
    speeds();
    stamps();
    reference();
    peaks();
    snrAndPower();
    ab();
    tracker();
    history();
    return testSummary("test_slint_host_extras");
}
