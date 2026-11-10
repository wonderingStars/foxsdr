// The radio pane (docs/multi-radio-records/screen-s1-s2.md, S2): six panes on one sheet fed with fake data, probed pixel by pixel in both skins - the radio colours by
// position (R5 wears R1's, R6 R2's), the selection outline, the compact spectrum (no 76 px header: the plot starts under the ribbon), the waterfall under the splitter -
// then read through the element tree by accessible label (every pane control names its radio and no label is shared between panes) and driven with real pointer events
// (a click in pane 2 reports radio 2, a wheel in pane 4 radio 4, STOP in pane 2 radio 2, the tag of pane 3 selects 3). The caption rule (lamps lose their words below the
// set width) and the header's overflow rule (the device name elides, the frequency keeps its width) are checked on the narrow panes.
// Everything here is FAKE DATA: no radio, no engine, no host.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "render_harness.hpp"
#include "test_check.hpp"

#include "radio_pane_specimen.h"

#ifndef SLINT_FEATURE_EXPERIMENTAL
#define SLINT_FEATURE_EXPERIMENTAL 1
#endif
#include <slint-testing.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <optional>
#include <vector>

using namespace foxsdr::slinttest;
using std::chrono::milliseconds;

namespace {

#define CHECKV(cond, ...)                         \
    do {                                          \
        const bool ok_ = static_cast<bool>(cond); \
        CHECK(ok_);                               \
        if (!ok_) {                               \
            std::printf("      ");                \
            std::printf(__VA_ARGS__);             \
            std::printf("\n");                    \
        }                                         \
    } while (0)

constexpr int kPaneW = 556, kPaneH = 353;
constexpr int kX[6] = {0, 562, 0, 562, 1124, 1124};
constexpr int kY[6] = {0, 0, 359, 359, 0, 359};
constexpr int kW[6] = {556, 556, 556, 556, 420, 420};

slint::SharedString ss(const char* s) { return slint::SharedString(s); }
Rgb rgbOf(const slint::Color& c) { return Rgb{c.red(), c.green(), c.blue()}; }

slint::Image solid(std::uint32_t w, std::uint32_t h, std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a) {
    slint::SharedPixelBuffer<slint::Rgba8Pixel> buf(w, h);
    for (auto* p = buf.begin(); p != buf.end(); ++p) { *p = slint::Rgba8Pixel{r, g, b, a}; }
    return slint::Image(buf);
}

RadioPaneData makePane(int radio, const char* device, const char* freq, const char* mode, bool running, bool decoding, bool muted, bool failed, const char* decoder) {
    RadioPaneData d;
    d.radio = radio;
    const std::string n = std::to_string(radio);
    d.prefix = ss(("Radio " + n).c_str());
    d.select_label = ss(("Select Radio " + n).c_str());
    d.stop_label = ss(((running ? "Stop Radio " : "Start Radio ") + n).c_str());
    d.spectrum_label = ss(("Radio " + n + " spectrum").c_str());
    d.waterfall_label = ss(("Radio " + n + " waterfall").c_str());
    d.resize_label = ss(("Resize Radio " + n).c_str());
    d.device = ss(device);
    d.frequency = ss(freq);
    d.mode = ss(mode);
    d.running = running; d.decoding = decoding; d.muted = muted; d.failed = failed;
    d.gain = ss("32 dB"); d.squelch = ss("-60"); d.volume = ss("70%"); d.rate = ss("2.400 MS/s");
    d.bias_tee = radio == 3;
    d.decoder = ss(decoder);
    d.peak = ss("-41.2 dBFS");
    d.span = ss("SPAN 2.400 MHz");
    d.trace = solid(64, 32, 255, 0, 255, 255);          // magenta: the trace image fills the plot
    d.picture = solid(64, 32, 0, 0, 255, 255);          // blue: the waterfall picture
    d.trace_mode = 0;
    d.axis_unit = ss("MHz");
    d.freq_ticks = std::make_shared<slint::VectorModel<AxisTick>>(std::vector<AxisTick>{{0.25f, ss("145.2")}, {0.75f, ss("145.8")}});
    d.db_ticks = std::make_shared<slint::VectorModel<DbTick>>(std::vector<DbTick>{{0.2f, ss("-20")}, {0.6f, ss("-60")}});
    d.age_ticks = std::make_shared<slint::VectorModel<AgeTick>>(std::vector<AgeTick>{{0.1f, ss("now")}, {0.6f, ss("10 s")}});
    d.bands = std::make_shared<slint::VectorModel<BandSegment>>(std::vector<BandSegment>{{0.1f, 0.4f, ss("2 m"), slint::Color::from_argb_uint8(0, 0, 0, 0)}});
    d.bookmarks = std::make_shared<slint::VectorModel<BookmarkMark>>(std::vector<BookmarkMark>{});
    d.marks = std::make_shared<slint::VectorModel<PluginMark>>(std::vector<PluginMark>{});
    d.wf_marks = std::make_shared<slint::VectorModel<PluginMark>>(std::vector<PluginMark>{});
    d.markers = std::make_shared<slint::VectorModel<UserMarker>>(std::vector<UserMarker>{});
    d.vfo_pos = 0.5f; d.vfo_half = 0.01f; d.show_vfo = true;
    d.foot_left = ss("24 lines/s"); d.foot_tuned = ss(freq); d.foot_rest = ss(" . NFM");
    return d;
}

}  // namespace

int main() {
    installHeadlessPlatform();
    auto ui = RadioPaneSpecimen::create();
    auto& win = ui->window();

    std::vector<RadioPaneData> panes;
    panes.push_back(makePane(1, "RTL-SDR #0", "145.500000 MHz", "NFM", true, false, false, false, ""));
    panes.push_back(makePane(2, "SDRplay RSPduo", "98.800000 MHz", "WFM", false, false, false, true, ""));
    panes.push_back(makePane(3, "HackRF One", "7.074000 MHz", "USB", true, true, false, false, "FT8"));
    panes.push_back(makePane(4, "Airspy Mini", "162.550000 MHz", "NFM", true, false, true, false, "APRS"));
    panes.push_back(makePane(5, "A very long device name that cannot possibly fit beside the frequency of this short pane 0123456789", "145.500000 MHz", "NFM", true, true, false, false, "FT8"));
    panes.push_back(makePane(6, "I/Q file", "3.573000 MHz", "USB", false, false, false, false, ""));
    ui->set_panes(std::make_shared<slint::VectorModel<RadioPaneData>>(panes));
    ui->set_selected_radio(1);
    render(ui, 1560, 720);

    const auto run = [&](const char* skinName, const Skin& skin) {
        std::printf("-- skin %s\n", skinName);
        ui->global<Theme>().set_skin(skin);
        Frame f = render(ui, 1560, 720);
        const Rgb radioColour[6] = {rgbOf(skin.radio_1), rgbOf(skin.radio_2), rgbOf(skin.radio_3), rgbOf(skin.radio_4), rgbOf(skin.radio_1), rgbOf(skin.radio_2)};
        const Rgb line = rgbOf(skin.line);
        const Rgb panel = rgbOf(skin.panel);

        // the four colours are four colours (R1..R4), and 5 and 6 repeat 1 and 2 by position
        CHECK(!nearColour(radioColour[0], radioColour[1], 24));
        CHECK(!nearColour(radioColour[0], radioColour[2], 24));
        CHECK(!nearColour(radioColour[0], radioColour[3], 24));
        CHECK(!nearColour(radioColour[1], radioColour[2], 24));
        CHECK(!nearColour(radioColour[1], radioColour[3], 24));
        CHECK(!nearColour(radioColour[2], radioColour[3], 24));
        for (int i = 0; i < 6; ++i) {
            // the R-tag: the radio's colour (left of its number, inside the 30 x 18 plate at x 8, y 6)
            CHECKV(f.near(kX[i] + 11, kY[i] + 9, radioColour[static_cast<std::size_t>(i)], 6), "%s: pane %d tag colour", skinName, i + 1);
            CHECKV(f.near(kX[i] + 120, kY[i] + 2, panel, 6), "%s: pane %d header ground", skinName, i + 1);
            // the header is 30 px: its hairline is row 29
            CHECKV(f.rowNear(kY[i] + 29, kX[i] + 120, kX[i] + 124, line, 6), "%s: pane %d header hairline", skinName, i + 1);
        }
        // the selection: 2 px in the radio's colour on pane 1; a hairline on the others
        CHECK(f.near(0, 200, radioColour[0], 6));
        CHECK(f.near(1, 200, radioColour[0], 6));
        CHECK(!f.near(2, 200, radioColour[0], 24));
        CHECK(f.near(562, 200, line, 6));
        CHECK(!f.near(563, 200, radioColour[1], 24));

        // THE COMPACT SPECTRUM: the pane's spectrum well starts at y 58 (header 30 + readout row 28); the ribbon is 13 px and its names 15 more, so the plot - the
        // magenta trace image - starts at 58 + 28 = 86 in every pane, not at 58 + 76 = 134 as in the receiver view
        for (int i = 0; i < 6; ++i) {
            const int x = kX[i] + 44 + 250;
            const int top = f.firstRowNear(x, kY[i] + 58, kY[i] + 200, rgb(0xff00ff), 8);
            CHECKV(top >= kY[i] + 85 && top <= kY[i] + 87, "%s: pane %d plot top at %d (want 86)", skinName, i + 1, top - kY[i]);
            // the ribbon's segment "2 m" (0.1 .. 0.4 of the plot) is drawn in the band plan's colour at the well's top
            CHECKV(f.near(kX[i] + 44 + static_cast<int>(0.25 * (kW[i] - 44)), kY[i] + 58 + 4, rgbOf(skin.bandplan), 8), "%s: pane %d ribbon", skinName, i + 1);
            // no header of the receiver view: no caption, no peak block and no SPAN chip over the plot's rows 58..86 at the right edge
            CHECKV(f.near(kX[i] + kW[i] - 40, kY[i] + 58 + 30, rgbOf(skin.well), 8) || f.near(kX[i] + kW[i] - 40, kY[i] + 58 + 30, rgb(0xff00ff), 8), "%s: pane %d no peak block", skinName, i + 1);
            // the waterfall's picture (blue) sits under the splitter: well-deep above it is the age strip's ground
            CHECKV(f.countNear(kX[i] + 44, kY[i] + 200, kX[i] + kW[i], kY[i] + 300, rgb(0x0000ff), 8) > 1000, "%s: pane %d waterfall picture", skinName, i + 1);
        }
        f.savePng(shotDir() + "radio-pane-" + skinName + ".png");
    };
    run("workbench", ui->global<Workbench>().get_value());
    run("night", ui->global<NightWatch>().get_value());
    ui->global<Theme>().set_skin(ui->global<Workbench>().get_value());
    render(ui, 1560, 720);

    // ---- the ribbon off: the plot starts at the well's top (58) ----
    ui->set_ribbon_size(0);
    {
        Frame g = render(ui, 1560, 720);
        const int top = g.firstRowNear(44 + 250, 58, 200, rgb(0xff00ff), 8);
        CHECKV(top >= 57 && top <= 59, "ribbon off: plot top at %d (want 58)", top);
    }
    ui->set_ribbon_size(2);
    render(ui, 1560, 720);

    // ---- every pane control carries its radio; no label is shared between panes ----
    const auto count = [&](const std::string& label, bool nonText = true) {
        int n = 0;
        for (const auto& e : slint::testing::ElementHandle::find_by_accessible_label(ui, label)) {
            if (nonText && e.accessible_role() == slint::language::AccessibleRole::Text) { continue; }
            ++n;
        }
        return n;
    };
    const auto findOne = [&](const std::string& label, bool anyRole = false) -> std::optional<slint::testing::ElementHandle> {
        for (const auto& e : slint::testing::ElementHandle::find_by_accessible_label(ui, label)) {
            if (anyRole || e.accessible_role() != slint::language::AccessibleRole::Text) { return e; }
        }
        return std::nullopt;
    };
    const bool running[6] = {true, false, true, true, true, false};
    for (int n = 1; n <= 6; ++n) {
        const std::string stopLabel = std::string(running[n - 1] ? "Stop Radio " : "Start Radio ") + std::to_string(n);
        CHECKV(count(stopLabel) == 1, "label \"%s\" found %d times", stopLabel.c_str(), count(stopLabel));
        CHECKV(count(std::string(running[n - 1] ? "Start Radio " : "Stop Radio ") + std::to_string(n)) == 0, "radio %d offers the wrong key", n);
        CHECKV(count("Select Radio " + std::to_string(n)) == 1, "Select Radio %d", n);
        CHECKV(count("Radio " + std::to_string(n) + " spectrum") == 1, "Radio %d spectrum", n);
        CHECKV(count("Radio " + std::to_string(n) + " waterfall") == 1, "Radio %d waterfall", n);
        CHECKV(count("Resize Radio " + std::to_string(n)) == 1, "Resize Radio %d", n);
        CHECKV(count("Radio " + std::to_string(n) + ": GAIN 32 dB", false) == 1, "Radio %d gain readout", n);
    }
    CHECK(count("Stop Radio 7") == 0);
    // a label that was not qualified would be found six times: none of the bare ones is a control of a pane
    CHECK(count("STOP") == 0 && count("START") == 0 && count("Spectrum") == 0 && count("Waterfall") == 0);

    // ---- the caption rule and the overflow rule ----
    CHECK(ui->get_wide_captions());
    CHECK(!ui->get_narrow_captions());
    {
        const auto f1 = findOne("Radio 1: 145.500000 MHz", true);
        const auto f5 = findOne("Radio 5: 145.500000 MHz", true);
        const auto d1 = findOne("Radio 1: RTL-SDR #0", true);
        const auto d5 = findOne("Radio 5: A very long device name that cannot possibly fit beside the frequency of this short pane 0123456789", true);
        CHECK(f1.has_value() && f5.has_value() && d1.has_value() && d5.has_value());
        if (f1 && f5 && d5) {
            // the frequency keeps its width in the short pane; the long device name is the one that gave way
            CHECKV(std::fabs(f1->size().width - f5->size().width) < 1.0f, "frequency width %.1f vs %.1f", f1->size().width, f5->size().width);
            const auto mode5 = findOne("Radio 5: NFM", true);
            CHECK(mode5.has_value());
            if (mode5) { CHECKV(d5->absolute_position().x + d5->size().width <= f5->absolute_position().x + 0.5f, "device name overlaps the frequency"); }
            CHECKV(f5->absolute_position().x + f5->size().width <= static_cast<float>(kX[4] + 420), "frequency leaves the pane");
            CHECKV(d5->size().width < 200.0f, "device name width %.1f: it should have shrunk", d5->size().width);
        }
    }

    // ---- the readout row holds what fits: PEAK in a 556 px pane, not in a 420 px one; the decoder chip and SPAN in both ----
    {
        Frame r = render(ui, 1560, 720);
        const auto inkIn = [&](const std::string& label, int pane) {
            const auto e = findOne(label, true);
            if (!e) { return -1; }
            const auto p = e->absolute_position();
            const auto s = e->size();
            // the strip's ground is the row's colour; anything else inside the element's box is a figure
            return r.countInk(static_cast<int>(p.x), static_cast<int>(p.y) + 4, static_cast<int>(p.x + s.width), static_cast<int>(p.y + s.height) - 4,
                              rgbOf(ui->global<Workbench>().get_value().strip), 24) + 0 * pane;
        };
        CHECKV(inkIn("Radio 1: PEAK -41.2 dBFS", 1) > 30, "pane 1 shows PEAK (ink %d)", inkIn("Radio 1: PEAK -41.2 dBFS", 1));
        CHECKV(inkIn("Radio 5: PEAK -41.2 dBFS", 5) <= 0, "pane 5 drops PEAK (a hidden element leaves the tree: ink %d)", inkIn("Radio 5: PEAK -41.2 dBFS", 5));
        CHECKV(inkIn("Radio 3: FT8", 3) > 100, "pane 3 shows its decoder chip (ink %d)", inkIn("Radio 3: FT8", 3));
        CHECKV(inkIn("Radio 1: SPAN 2.400 MHz", 1) > 100 && inkIn("Radio 5: SPAN 2.400 MHz", 5) > 100, "SPAN stays in both widths");
        CHECKV(inkIn("Radio 5: GAIN 32 dB", 5) > 30, "pane 5 keeps GAIN");
    }

    // ---- real pointer events: each reports its own radio ----
    const auto clickCentre = [&](const slint::testing::ElementHandle& e) {
        const auto p = e.absolute_position();
        const auto s = e.size();
        click(win, p.x + s.width / 2, p.y + s.height / 2);
        settle(milliseconds(20));
    };
    {
        const auto stop2 = findOne("Start Radio 2");
        CHECK(stop2.has_value());
        if (stop2) { clickCentre(*stop2); CHECK(std::string(ui->get_last_event()) == "stop" && ui->get_last_radio() == 2); }
        const auto stop4 = findOne("Stop Radio 4");
        if (stop4) { clickCentre(*stop4); CHECK(std::string(ui->get_last_event()) == "stop" && ui->get_last_radio() == 4); }
        const auto tag3 = findOne("Select Radio 3");
        CHECK(tag3.has_value());
        if (tag3) { clickCentre(*tag3); CHECK(std::string(ui->get_last_event()) == "select" && ui->get_last_radio() == 3); }
        // the readout row's blank space selects its pane
        click(win, static_cast<float>(kX[3] + 300), static_cast<float>(kY[3] + 30 + 14));
        settle(milliseconds(20));
        CHECK(std::string(ui->get_last_event()) == "select" && ui->get_last_radio() == 4);
        // a click in pane 2's spectrum at a quarter of its plot: radio 2, x = 0.25
        const float sx = static_cast<float>(kX[1] + 44 + 0.25 * (kW[1] - 44)), sy = static_cast<float>(kY[1] + 58 + 28 + 40);
        click(win, sx, sy);
        settle(milliseconds(20));
        CHECK(std::string(ui->get_last_event()) == "spectrum-click" && ui->get_last_radio() == 2);
        CHECK(std::fabs(ui->get_last_x() - 0.25f) < 0.01f);
        // a wheel notch up in pane 4's spectrum: radio 4, one notch
        scroll(win, static_cast<float>(kX[3] + 44 + 100), static_cast<float>(kY[3] + 58 + 28 + 40), 0.f, 120.f);
        settle(milliseconds(20));
        CHECK(std::string(ui->get_last_event()) == "spectrum-wheel" && ui->get_last_radio() == 4 && ui->get_last_notches() == 1);
        // a click in pane 3's waterfall at three quarters of its picture: radio 3
        const float wx = static_cast<float>(kX[2] + 44 + 0.75 * (kW[2] - 44)), wy = static_cast<float>(kY[2] + 220);
        click(win, wx, wy);
        settle(milliseconds(40));
        CHECK(std::string(ui->get_last_event()) == "waterfall-click" && ui->get_last_radio() == 3);
        CHECK(std::fabs(ui->get_last_x() - 0.75f) < 0.01f);
        // the accessible default action of the key is the same click
        const auto stop1 = findOne("Stop Radio 1");
        if (stop1) { stop1->invoke_accessible_default_action(); CHECK(std::string(ui->get_last_event()) == "stop" && ui->get_last_radio() == 1); }
    }

    return testSummary("test_slint_radio_pane");
}
