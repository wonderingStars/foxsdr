// The radio workspace (docs/multi-radio-records/screen-s3.md, stage S3): the bar and the grid of panes on one sheet, fed with fake data for eight radios through the WorkspaceModel global,
// probed pixel by pixel in both skins - every layout (Side by side, 2 x 2), the page of four and the second page, the selected pane's outline, an empty slot, the gaps - then read through
// the element tree by accessible label (the layout keys, the page keys and the page shown, every pane control naming its radio) and driven with real pointer events (a click in pane 2's
// spectrum reports radio 2, a right-click reports the window's own coordinates, the splitter's state is kept for the radio across a page change). The readout row's drop order (PEAK, BIAS,
// RATE, VOL, SQL, the decoder chip, GAIN) is held at ten widths, and the R-tag's ink is measured against all four radio colours in every skin (at least 4.5 to 1).
// Everything here is FAKE DATA: no radio, no engine, no host. The small-window fallback is the host's rule (test_slint_workspace_host); here the notice it publishes is shown.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "render_harness.hpp"
#include "test_check.hpp"

#include "radio_workspace_specimen.h"

#ifndef SLINT_FEATURE_EXPERIMENTAL
#define SLINT_FEATURE_EXPERIMENTAL 1
#endif
#include <slint-testing.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
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

slint::SharedString ss(const std::string& s) { return slint::SharedString(s); }
Rgb rgbOf(const slint::Color& c) { return Rgb{c.red(), c.green(), c.blue()}; }

double linear(double c) { c /= 255.0; return c <= 0.03928 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); }
double luminance(const slint::Color& c) { return 0.2126 * linear(c.red()) + 0.7152 * linear(c.green()) + 0.0722 * linear(c.blue()); }
double contrast(const slint::Color& a, const slint::Color& b) {
    double la = luminance(a), lb = luminance(b);
    if (la < lb) { std::swap(la, lb); }
    return (la + 0.05) / (lb + 0.05);
}

slint::Image solid(std::uint32_t w, std::uint32_t h, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    slint::SharedPixelBuffer<slint::Rgba8Pixel> buf(w, h);
    for (auto* p = buf.begin(); p != buf.end(); ++p) { *p = slint::Rgba8Pixel{r, g, b, 255}; }
    return slint::Image(buf);
}

RadioPaneData makePane(int radio, const char* decoder) {
    static const char* devices[8] = {"RTL-SDR #0", "SDRplay RSPduo", "HackRF One", "Airspy Mini", "Test radio 5", "Test radio 6", "Test radio 7", "Test radio 8"};
    static const char* freqs[8] = {"145.500000 MHz", "98.800000 MHz", "7.074000 MHz", "162.550000 MHz", "3.573000 MHz", "14.074000 MHz", "27.185000 MHz", "433.920000 MHz"};
    RadioPaneData d;
    d.radio = radio;
    const std::string n = std::to_string(radio);
    d.prefix = ss("Radio " + n);
    d.select_label = ss("Select Radio " + n);
    d.stop_label = ss((radio % 3 == 0 ? "Start Radio " : "Stop Radio ") + n);
    d.spectrum_label = ss("Radio " + n + " spectrum");
    d.waterfall_label = ss("Radio " + n + " waterfall");
    d.resize_label = ss("Resize Radio " + n);
    d.removable = radio > 1; d.close_label = ss("Remove Radio " + n);   // stage S4: radios 2 and up have a close key
    d.link_shown = false; d.link = 0; d.link_label = ss("Link Radio " + n); d.link_tip = ss("Not linked: Link tuning never moves this radio");   // stage S5: the link key exists only while Link tuning is on
    d.device = ss(devices[(radio - 1) % 8]);
    d.frequency = ss(freqs[(radio - 1) % 8]);
    d.mode = ss("NFM");
    d.running = radio % 3 != 0;
    d.decoding = false; d.muted = false; d.failed = false; d.bias_tee = false;   // a generated struct is not value-initialised
    d.trace_mode = 0; d.cursor_visible = false; d.cursor_pos = -1; d.cursor_text = ss("");
    d.gain = ss("32 dB"); d.squelch = ss("-60 dB"); d.volume = ss("70%"); d.rate = ss("2.400 MS/s");
    d.decoder = ss(decoder);
    d.peak = ss("-41.2 dBFS");
    d.span = ss("SPAN 2.400 MHz");
    d.trace = solid(64, 32, 255, 0, 255);                // magenta: the trace image fills the plot
    d.picture = solid(64, 32, 0, 0, 255);                // blue: the waterfall picture
    d.axis_unit = ss("MHz");
    d.freq_ticks = std::make_shared<slint::VectorModel<AxisTick>>(std::vector<AxisTick>{{0.25f, ss("145.2")}, {0.75f, ss("145.8")}});
    d.db_ticks = std::make_shared<slint::VectorModel<DbTick>>(std::vector<DbTick>{{0.2f, ss("-20")}, {0.6f, ss("-60")}});
    d.age_ticks = std::make_shared<slint::VectorModel<AgeTick>>(std::vector<AgeTick>{{0.1f, ss("now")}});
    d.bands = std::make_shared<slint::VectorModel<BandSegment>>(std::vector<BandSegment>{});
    d.bookmarks = std::make_shared<slint::VectorModel<BookmarkMark>>(std::vector<BookmarkMark>{});
    d.marks = std::make_shared<slint::VectorModel<PluginMark>>(std::vector<PluginMark>{});
    d.wf_marks = std::make_shared<slint::VectorModel<PluginMark>>(std::vector<PluginMark>{});
    d.markers = std::make_shared<slint::VectorModel<UserMarker>>(std::vector<UserMarker>{});
    d.vfo_pos = 0.5f; d.vfo_half = 0.01f; d.show_vfo = true;
    d.foot_left = ss("24 lines/s"); d.foot_tuned = d.frequency; d.foot_rest = ss(" . NFM");
    return d;
}

std::vector<WorkspaceCell> makeCells(int occupiedCount) {
    std::vector<WorkspaceCell> cells;
    for (int i = 1; i <= 8; ++i) {
        WorkspaceCell c;
        c.occupied = i <= occupiedCount;
        c.label = ss("Radio " + std::to_string(i));
        c.pane = makePane(i, i == 2 ? "APRS" : "");
        cells.push_back(c);
    }
    return cells;
}

}  // namespace

int main() {
    installHeadlessPlatform();
    auto ui = RadioWorkspaceSpecimen::create();
    auto& win = ui->window();
    auto& g = ui->global<WorkspaceModel>();

    std::string lastEvent;
    int lastRadio = 0, lastValue = -1, events = 0;
    float lastX = 0, lastMenuX = 0, lastMenuY = 0;
    // The host decides the layout and the page; here a stand-in does: it echoes the choice (a refused or clamped one would not).
    g.on_layout_selected([&](int l) { lastEvent = "layout"; lastValue = l; ++events; g.set_layout(l); g.set_shown_layout(l); });
    g.on_page_selected([&](int p) { lastEvent = "page"; lastValue = p; ++events; g.set_page(p); });
    g.on_select_requested([&](int r) { lastEvent = "select"; lastRadio = r; ++events; });
    g.on_stop_clicked([&](int r) { lastEvent = "stop"; lastRadio = r; ++events; });
    g.on_remove_requested([&](int r) { lastEvent = "remove"; lastRadio = r; ++events; });
    g.on_pane_link_clicked([&](int r) { lastEvent = "pane-link"; lastRadio = r; ++events; });
    g.on_spectrum_clicked([&](int r, float x, bool) { lastEvent = "spectrum-click"; lastRadio = r; lastX = x; ++events; });
    g.on_spectrum_wheel([&](int r, float, int n, bool) { lastEvent = "spectrum-wheel"; lastRadio = r; lastValue = n; ++events; });
    g.on_waterfall_clicked([&](int r, float x, bool) { lastEvent = "waterfall-click"; lastRadio = r; lastX = x; ++events; });
    g.on_spectrum_menu_requested([&](int r, float, float px, float py) { lastEvent = "spectrum-menu"; lastRadio = r; lastMenuX = px; lastMenuY = py; ++events; });
    g.on_waterfall_menu_requested([&](int r, float, float px, float py) { lastEvent = "waterfall-menu"; lastRadio = r; lastMenuX = px; lastMenuY = py; ++events; });

    auto model = std::make_shared<slint::VectorModel<WorkspaceCell>>(makeCells(8));
    g.set_cells(model);
    g.set_radio_count(8);
    g.set_layout(2);
    g.set_shown_layout(2);
    g.set_page(0);
    g.set_selected_radio(1);
    g.set_ribbon_size(0);
    g.set_page_labels(std::make_shared<slint::VectorModel<slint::SharedString>>(std::vector<slint::SharedString>{ss("Radios 1-4"), ss("Radios 5-8")}));
    g.set_page_tips(std::make_shared<slint::VectorModel<slint::SharedString>>(std::vector<slint::SharedString>{ss("Show radios 1 to 4"), ss("Show radios 5 to 8")}));
    constexpr int W = 1160, H = 760, BAR = 30;
    render(ui, W, H);

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
    const auto clickCentre = [&](const slint::testing::ElementHandle& e, slint::PointerEventButton b = slint::PointerEventButton::Left) {
        const auto p = e.absolute_position();
        const auto s = e.size();
        click(win, p.x + s.width / 2, p.y + s.height / 2, b);
        settle(milliseconds(20));
    };
    const auto paneIs = [&](int radio, bool on) {
        const std::string n = std::to_string(radio);
        const int c = count("Select Radio " + n) + count("Radio " + n + " spectrum") + count("Radio " + n + " waterfall");
        return on ? c == 3 : c == 0;
    };

    // ---- the colours and the contrast of the tag's ink: every skin, all four radio colours ----
    const Skin workbench = ui->global<Workbench>().get_value();
    const Skin night = ui->global<NightWatch>().get_value();
    const auto skinRun = [&](const char* skinName, const Skin& skin) {
        std::printf("-- skin %s\n", skinName);
        ui->global<Theme>().set_skin(skin);
        g.set_layout(2); g.set_shown_layout(2); g.set_page(0); g.set_selected_radio(1);
        g.set_cells(model);
        const slint::Color radioColour[4] = {skin.radio_1, skin.radio_2, skin.radio_3, skin.radio_4};
        // THE R-TAG'S INK against each radio colour (decision 3 of the lane brief): at least 4.5 to 1; the numbers are in the record
        for (int i = 0; i < 4; ++i) {
            const double c = contrast(skin.radio_ink, radioColour[i]);
            std::printf("   contrast R%d tag: ink %s on %s = %.2f : 1\n", i + 1, hex(rgbOf(skin.radio_ink)).c_str(), hex(rgbOf(radioColour[i])).c_str(), c);
            CHECKV(c >= 4.5, "%s: the tag's ink on radio %d is only %.2f : 1", skinName, i + 1, c);
        }

        // ---- 2 x 2, page one: radios 1 to 4, the gaps, the sizes ----
        Frame f = render(ui, W, H);
        const int cw = (W - 6) / 2, ch = (H - BAR - 6) / 2;          // 577 x 362
        CHECKV(std::fabs(g.get_cell_w() - cw) < 1.5f && std::fabs(g.get_cell_h() - ch) < 1.5f, "%s: cell size %.1f x %.1f, want %d x %d", skinName, static_cast<double>(g.get_cell_w()), static_cast<double>(g.get_cell_h()), cw, ch);
        const int px[4] = {0, cw + 6, 0, cw + 6};
        const int py[4] = {BAR, BAR, BAR + ch + 6, BAR + ch + 6};
        for (int i = 0; i < 4; ++i) {
            CHECKV(paneIs(i + 1, true), "%s: radio %d's pane is on screen", skinName, i + 1);
            CHECKV(f.near(px[i] + 11, py[i] + 9, rgbOf(radioColour[i]), 6), "%s: pane %d tag colour", skinName, i + 1);
            // the tag's number is drawn in the ink (a few glyph pixels of exactly that colour inside the 30 x 18 plate)
            CHECKV(f.countNear(px[i] + 8, py[i] + 6, px[i] + 38, py[i] + 24, rgbOf(skin.radio_ink), 24) > 6, "%s: pane %d tag ink", skinName, i + 1);
            CHECKV(f.rowNear(py[i] + 29, px[i] + 120, px[i] + 124, rgbOf(skin.line), 6), "%s: pane %d header hairline", skinName, i + 1);
        }
        for (int i = 4; i < 8; ++i) { CHECKV(paneIs(i + 1, false), "%s: radio %d is on the other page", skinName, i + 1); }
        CHECKV(f.near(cw + 3, BAR + 100, rgbOf(skin.bg), 4) && f.near(100, BAR + ch + 3, rgbOf(skin.bg), 4), "%s: the 6 px gaps are the ground", skinName);
        // the selection: radio 1's outline is 2 px in its colour; the others have the hairline
        CHECK(f.near(0, BAR + 200, rgbOf(radioColour[0]), 6) && f.near(1, BAR + 200, rgbOf(radioColour[0]), 6) && !f.near(2, BAR + 200, rgbOf(radioColour[0]), 24));
        CHECK(f.near(cw + 6, BAR + 200, rgbOf(skin.line), 6));
        g.set_selected_radio(3);
        Frame s3 = render(ui, W, H);
        CHECKV(s3.near(0, py[2] + 100, rgbOf(radioColour[2]), 6) && s3.near(1, py[2] + 100, rgbOf(radioColour[2]), 6), "%s: pane 3 is marked", skinName);
        CHECKV(!s3.near(0, BAR + 200, rgbOf(radioColour[0]), 24), "%s: pane 1 is not marked any more", skinName);
        f = s3;
        f.savePng(shotDir() + "workspace-" + skinName + "-four-page1.png");
        g.set_selected_radio(1);

        // ---- the second page of four: radios 5 to 8 wear the colours of 1 to 4 by position ----
        g.set_page(1);
        Frame p2 = render(ui, W, H);
        for (int i = 0; i < 4; ++i) {
            CHECKV(paneIs(i + 5, true) && paneIs(i + 1, false), "%s: page two shows radio %d", skinName, i + 5);
            CHECKV(p2.near(px[i] + 11, py[i] + 9, rgbOf(radioColour[i]), 6), "%s: pane of radio %d wears colour %d", skinName, i + 5, i + 1);
        }
        p2.savePng(shotDir() + "workspace-" + skinName + "-four-page2.png");
        g.set_page(0);

        // ---- side by side: two panes, each the full height; the other radios are not on screen ----
        g.set_layout(1); g.set_shown_layout(1);
        Frame two = render(ui, W, H);
        const int th = H - BAR;
        CHECKV(std::fabs(g.get_cell_w() - cw) < 1.5f && std::fabs(g.get_cell_h() - th) < 1.5f, "%s: side by side cell %.1f x %.1f", skinName, static_cast<double>(g.get_cell_w()), static_cast<double>(g.get_cell_h()));
        CHECK(paneIs(1, true) && paneIs(2, true) && paneIs(3, false) && paneIs(4, false));
        CHECK(two.near(11, BAR + 9, rgbOf(radioColour[0]), 6) && two.near(cw + 6 + 11, BAR + 9, rgbOf(radioColour[1]), 6));
        CHECKV(two.near(cw + 3, BAR + 300, rgbOf(skin.bg), 4), "%s: the gap between the two", skinName);
        two.savePng(shotDir() + "workspace-" + skinName + "-two.png");
        g.set_page(3);                                                                // pairs: 7 and 8
        render(ui, W, H);
        CHECK(paneIs(7, true) && paneIs(8, true) && paneIs(1, false) && paneIs(2, false));
        g.set_page(0);

        // ---- an empty slot: radios 1 to 6 are open, the second page of four has two empty cells ----
        g.set_layout(2); g.set_shown_layout(2);
        g.set_cells(std::make_shared<slint::VectorModel<WorkspaceCell>>(makeCells(6)));
        g.set_page(1);
        Frame em = render(ui, W, H);
        CHECK(paneIs(5, true) && paneIs(6, true) && paneIs(7, false) && paneIs(8, false));
        CHECKV(count("Radio 7") == 1 && count("Radio 8") == 1, "%s: the empty slots are named", skinName);
        CHECKV(em.near(px[2] + 100, py[2] + 100, rgbOf(skin.well_deep), 6), "%s: an empty slot is a quiet well", skinName);
        em.savePng(shotDir() + "workspace-" + skinName + "-empty-slots.png");
        g.set_page(0);
        g.set_cells(model);
    };
    skinRun("workbench", workbench);
    skinRun("night", night);
    ui->global<Theme>().set_skin(workbench);
    g.set_layout(2); g.set_shown_layout(2); g.set_page(0); g.set_cells(model); g.set_selected_radio(1);
    render(ui, W, H);

    // ---- the keys: the layout keys, the page keys and the page shown ----
    {
        const auto one = findOne("One"), side = findOne("Side by side"), grid = findOne("2 x 2");
        CHECK(one.has_value() && side.has_value() && grid.has_value());
        if (one && side && grid) {
            CHECK(grid->accessible_checked().value_or(false) && !one->accessible_checked().value_or(true) && !side->accessible_checked().value_or(true));   // the chosen layout is lit
            clickCentre(*side);
            CHECK(lastEvent == "layout" && lastValue == 1 && g.get_layout() == 1);
            render(ui, W, H);
            CHECK(findOne("Side by side")->accessible_checked().value_or(false) && !findOne("2 x 2")->accessible_checked().value_or(true));
            clickCentre(*one);
            CHECK(lastEvent == "layout" && lastValue == 0);
            clickCentre(*grid);
            CHECK(lastEvent == "layout" && lastValue == 2);
        }
        render(ui, W, H);
        // THE PAGE INDICATOR: one key per page, the page shown is lit; a click asks for that page
        const auto p1 = findOne("Radios 1-4"), p2 = findOne("Radios 5-8");
        CHECK(p1.has_value() && p2.has_value());
        if (p1 && p2) {
            CHECK(p1->accessible_checked().value_or(false) && !p2->accessible_checked().value_or(true));
            clickCentre(*p2);
            CHECK(lastEvent == "page" && lastValue == 1 && g.get_page() == 1);
            render(ui, W, H);
            CHECK(!findOne("Radios 1-4")->accessible_checked().value_or(true) && findOne("Radios 5-8")->accessible_checked().value_or(false));
            CHECK(paneIs(5, true));
            clickCentre(*findOne("Radios 1-4"));
            CHECK(g.get_page() == 0);
        }
        // no radio beyond the first page: no page keys at all
        g.set_page_labels(std::make_shared<slint::VectorModel<slint::SharedString>>(std::vector<slint::SharedString>{}));
        render(ui, W, H);
        CHECK(count("Radios 1-4") == 0 && count("Radios 5-8") == 0);
        g.set_page_labels(std::make_shared<slint::VectorModel<slint::SharedString>>(std::vector<slint::SharedString>{ss("Radios 1-4"), ss("Radios 5-8")}));
        // THE SMALL-WINDOW NOTICE the host publishes is shown on the bar (the rule itself is the host's)
        CHECK(count("Several radios need a window of at least 1280 x 720. Showing one radio.", false) == 0);
        g.set_notice(ss("Several radios need a window of at least 1280 x 720. Showing one radio."));
        render(ui, W, H);
        CHECK(count("Several radios need a window of at least 1280 x 720. Showing one radio.", false) == 1);
        g.set_notice(ss(""));
        render(ui, W, H);
    }

    // ---- every pane control carries its radio; the labels are unique across the four panes on screen ----
    {
        g.set_layout(2); g.set_shown_layout(2); g.set_page(0);
        render(ui, W, H);
        const bool running[4] = {true, true, false, true};
        for (int n = 1; n <= 4; ++n) {
            const std::string stop = std::string(running[n - 1] ? "Stop Radio " : "Start Radio ") + std::to_string(n);
            CHECKV(count(stop) == 1, "label \"%s\" found %d times", stop.c_str(), count(stop));
            CHECKV(count("Select Radio " + std::to_string(n)) == 1 && count("Radio " + std::to_string(n) + " spectrum") == 1 && count("Radio " + std::to_string(n) + " waterfall") == 1 && count("Resize Radio " + std::to_string(n)) == 1, "radio %d's controls", n);
        }
        CHECK(count("STOP") == 0 && count("START") == 0 && count("Spectrum") == 0 && count("Waterfall") == 0);
        // STAGE S4: radios 2 and up carry a close key naming their radio; radio 1 has none (it is always there)
        CHECKV(count("Remove Radio 1") == 0 && count("Remove Radio 2") == 1 && count("Remove Radio 3") == 1 && count("Remove Radio 4") == 1, "the close keys");
        // radio 2 runs a decoder: its chip is in its pane and in no other
        CHECK(count("Radio 2: APRS", false) == 1 && count("Radio 1: APRS", false) == 0);
    }

    // ---- real pointer events: each reports its own radio, in whatever cell the pane is ----
    {
        render(ui, W, H);
        const auto tag4 = findOne("Select Radio 4");
        CHECK(tag4.has_value());
        if (tag4) { clickCentre(*tag4); CHECK(lastEvent == "select" && lastRadio == 4); }
        const auto close3 = findOne("Remove Radio 3");
        CHECK(close3.has_value());
        if (close3) { clickCentre(*close3); CHECK(lastEvent == "remove" && lastRadio == 3); }
        const auto stop3 = findOne("Start Radio 3");
        if (stop3) { clickCentre(*stop3); CHECK(lastEvent == "stop" && lastRadio == 3); }
        const int cw = (W - 6) / 2, ch = (H - BAR - 6) / 2;
        // a click in pane 2's spectrum, a quarter into its plot (the plot starts after the 44 px gutter; with no ribbon it starts at the well's top, y 58)
        const float sx = static_cast<float>(cw + 6 + 44 + 0.25 * (cw - 44)), sy = static_cast<float>(BAR + 58 + 40);
        click(win, sx, sy);
        settle(milliseconds(20));
        CHECK(lastEvent == "spectrum-click" && lastRadio == 2);
        CHECKV(std::fabs(lastX - 0.25f) < 0.01f, "x %.3f", static_cast<double>(lastX));
        // the same place in pane 3 (below pane 1) is radio 3's
        click(win, static_cast<float>(44 + 0.75 * (cw - 44)), static_cast<float>(BAR + ch + 6 + 58 + 40));
        settle(milliseconds(20));
        CHECK(lastEvent == "spectrum-click" && lastRadio == 3 && std::fabs(lastX - 0.75f) < 0.01f);
        scroll(win, static_cast<float>(cw + 6 + 44 + 100), static_cast<float>(BAR + ch + 6 + 58 + 40), 0.f, 120.f);
        settle(milliseconds(20));
        CHECK(lastEvent == "spectrum-wheel" && lastRadio == 4 && lastValue == 1);
        click(win, static_cast<float>(44 + 0.5 * (cw - 44)), static_cast<float>(BAR + 58 + 150));
        settle(milliseconds(40));
        CHECKV(lastEvent == "waterfall-click" && lastRadio == 1, "event %s radio %d", lastEvent.c_str(), lastRadio);
        // a right-click reports the pointer's own place in the WINDOW (the host opens its menu there), whichever pane it was in
        const float mx = static_cast<float>(cw + 6 + 200), my = static_cast<float>(BAR + ch + 6 + 58 + 60);
        click(win, mx, my, slint::PointerEventButton::Right);
        settle(milliseconds(20));
        CHECKV(lastEvent == "spectrum-menu" && lastRadio == 4 && std::fabs(lastMenuX - mx) < 2.0f && std::fabs(lastMenuY - my) < 2.0f, "menu %s radio %d at %.1f,%.1f want %.1f,%.1f", lastEvent.c_str(), lastRadio, static_cast<double>(lastMenuX), static_cast<double>(lastMenuY), static_cast<double>(mx), static_cast<double>(my));
        // and the waterfall's, whose well is lower in the pane
        const float wy = static_cast<float>(BAR + 58 + 180);
        click(win, 300.f, wy, slint::PointerEventButton::Right);
        settle(milliseconds(20));
        CHECKV(lastEvent == "waterfall-menu" && lastRadio == 1 && std::fabs(lastMenuX - 300.f) < 2.0f && std::fabs(lastMenuY - wy) < 2.0f, "wf menu %s radio %d at %.1f,%.1f", lastEvent.c_str(), lastRadio, static_cast<double>(lastMenuX), static_cast<double>(lastMenuY));
    }

    // ---- THE SPLITTER IS KEPT PER RADIO: dragged in pane 1, it is where it was left after the page of radios 5 to 8 and back ----
    {
        g.set_page(0);
        render(ui, W, H);
        const auto before = findOne("Resize Radio 1");
        CHECK(before.has_value());
        const float y0 = before->absolute_position().y;
        const float cx = before->absolute_position().x + before->size().width / 2, cy = y0 + before->size().height / 2;
        const float otherY0 = findOne("Resize Radio 2")->absolute_position().y;
        press(win, cx, cy);
        moveTo(win, cx, cy + 20);
        moveTo(win, cx, cy + 40);
        release(win, cx, cy + 40);
        settle(milliseconds(20));
        render(ui, W, H);
        const float y1 = findOne("Resize Radio 1")->absolute_position().y;
        CHECKV(y1 > y0 + 20.0f, "the splitter moved from %.1f to %.1f", static_cast<double>(y0), static_cast<double>(y1));
        CHECKV(std::fabs(findOne("Resize Radio 2")->absolute_position().y - otherY0) < 0.5f, "radio 2's splitter did not move");
        const auto splits = g.get_splits();
        CHECK(splits->row_data(0).value_or(0.f) > 0.45f && std::fabs(splits->row_data(1).value_or(0.f) - 0.45f) < 0.001f);
        g.set_page(1);
        render(ui, W, H);
        CHECK(!findOne("Resize Radio 1").has_value());
        g.set_page(0);
        render(ui, W, H);
        CHECKV(std::fabs(findOne("Resize Radio 1")->absolute_position().y - y1) < 0.5f, "radio 1's splitter came back at %.1f, was left at %.1f", static_cast<double>(findOne("Resize Radio 1")->absolute_position().y), static_cast<double>(y1));
    }

    // ---- THE READOUT ROW'S DROP ORDER: PEAK, BIAS, RATE, VOL, SQL, the decoder chip, GAIN - at ten widths, with and without a decoder ----
    {
        const int kGain = 1, kSql = 2, kVol = 4, kRate = 8, kBias = 16, kDeco = 32;
        const int priority[6] = {kGain, kDeco, kSql, kVol, kRate, kBias};          // the order they are kept in, the last given up first
        for (const bool withDecoder : {true, false}) {
            ui->set_probe_data(makePane(1, withDecoder ? "APRS" : ""));
            std::vector<int> m;
            for (int w = 640; w >= 220; w -= 20) {            // widths from a wide pane down to a very narrow one
                ui->set_probe_width(static_cast<float>(w));
                render(ui, W, H);
                m.push_back(ui->get_probe_mask());
            }
            std::printf("   readout masks (decoder %d), 640 px down to 220 px:", withDecoder ? 1 : 0);
            for (const int v : m) { std::printf(" %02x", v); }
            std::printf("\n");
            CHECK(m.size() == 22);
            for (std::size_t i = 0; i < m.size(); ++i) {
                // what is shown is a PREFIX of the priority order (the decoder chip only when there is one): never a figure while one that outlasts it is gone
                bool gone = false;
                for (const int bit : priority) {
                    const bool wanted = bit != kDeco || withDecoder;
                    if (!wanted) { CHECKV((m[i] & bit) == 0, "probe %zu: a chip with no decoder", i); continue; }
                    const bool shown = (m[i] & bit) != 0;
                    CHECKV(!(shown && gone), "width %d (mask %02x): bit %d shown after a figure that outlasts it was dropped", 640 - 20 * static_cast<int>(i), m[i], bit);
                    if (!shown) { gone = true; }
                }
                // a narrower pane never shows what a wider one dropped
                if (i > 0) { CHECKV((m[i] & ~m[i - 1]) == 0, "width %d shows more than width %d", 640 - 20 * static_cast<int>(i), 640 - 20 * static_cast<int>(i - 1)); }
            }
            CHECKV(m[0] == (withDecoder ? 63 : 31), "the widest pane shows everything: %02x", m[0]);
            // the decoder chip outlives BIAS, RATE, VOL and SQL: somewhere the chip is shown and BIAS is gone, and somewhere the chip is shown with only GAIN beside it
            if (withDecoder) {
                bool chipWithoutBias = false, chipAfterGain = false, gainAlone = false;
                for (const int v : m) {
                    if ((v & kDeco) && !(v & kBias)) { chipWithoutBias = true; }
                    if (v == (kGain | kDeco)) { chipAfterGain = true; }
                    if (v == kGain) { gainAlone = true; }
                }
                CHECK(chipWithoutBias);
                CHECK(chipAfterGain || gainAlone);
            }
        }
        // PEAK goes FIRST: in real panes of rising width, PEAK is only ever drawn beside a full row (every figure shown)
        g.set_layout(1); g.set_shown_layout(1); g.set_page(0);
        int peakSeen = 0, rowShort = 0;
        for (int w = 980; w <= 1500; w += 20) {
            const Frame fr = render(ui, static_cast<std::uint32_t>(w), H);
            // a figure that is not shown stays in the element tree (hidden), so "shown" is ink in its box on the strip's ground, as test_slint_radio_pane reads it
            const auto shown = [&](const std::string& label) {
                const auto e = findOne(label, true);
                if (!e) { return false; }
                const auto pos = e->absolute_position();
                const auto sz = e->size();
                return fr.countInk(static_cast<int>(pos.x), static_cast<int>(pos.y) + 4, static_cast<int>(pos.x + sz.width), static_cast<int>(pos.y + sz.height) - 4, rgbOf(workbench.strip), 24) > 30;
            };
            const bool peak = shown("Radio 2: PEAK IN PASSBAND -41.2 dBFS");
            const bool bias = shown("Radio 2: BIAS OFF");
            const bool deco = shown("Radio 2: APRS");
            const bool gain = shown("Radio 2: GAIN 32 dB");
            if (peak) { ++peakSeen; }
            if (!bias || !deco) { ++rowShort; }
            CHECKV(!peak || (bias && deco && gain), "at %d px PEAK is drawn while a figure that outlasts it is gone (bias %d deco %d gain %d)", w, bias, deco, gain);
            CHECKV(!bias || gain, "at %d px BIAS without GAIN", w);
        }
        CHECK(peakSeen > 0 && rowShort > 0);        // the sweep covers both a row that has PEAK and a row that has lost figures
        g.set_layout(2); g.set_shown_layout(2);
        render(ui, W, H);
    }

    // ---- STAGE S5: the pane's link key: only while Link tuning is on; lit when the radio is in the group, red when it could not follow, plain when left out; a click names its radio ----
    for (const char* skinName : {"workbench", "night"}) {
        ui->global<Theme>().set_skin(std::string(skinName) == "night" ? night : workbench);
        g.set_layout(2); g.set_shown_layout(2); g.set_page(0);
        std::vector<WorkspaceCell> cells = makeCells(4);
        g.set_cells(std::make_shared<slint::VectorModel<WorkspaceCell>>(cells));
        render(ui, W, H);
        CHECKV(count("Link Radio 1") == 0 && count("Link Radio 2") == 0, "%s: no link key while Link tuning is off", skinName);
        const int state[4] = {1, 0, 2, 1};
        for (int i = 0; i < 4; ++i) {
            cells[static_cast<std::size_t>(i)].pane.link_shown = true;
            cells[static_cast<std::size_t>(i)].pane.link = state[i];
            cells[static_cast<std::size_t>(i)].pane.link_tip = ss(state[i] == 2 ? "Radio 3 could not follow the link and stays at 144.500000 MHz." : state[i] == 1 ? "Linked: moves with the other linked radios" : "Not linked: Link tuning never moves this radio");
        }
        g.set_cells(std::make_shared<slint::VectorModel<WorkspaceCell>>(cells));
        Frame f = render(ui, W, H);
        for (int n = 1; n <= 4; ++n) {
            const auto k = findOne("Link Radio " + std::to_string(n));
            CHECKV(k.has_value(), "%s: Link Radio %d is exactly one control", skinName, n);
            if (!k) { continue; }
            CHECKV(k->accessible_checked().value_or(false) == (state[n - 1] != 0), "%s: Link Radio %d checked = %d", skinName, n, state[n - 1] != 0);
            const auto p = k->absolute_position();
            const auto sz = k->size();
            CHECKV(sz.width > 20.f && sz.height > 10.f, "%s: Link Radio %d is %.0f x %.0f", skinName, n, static_cast<double>(sz.width), static_cast<double>(sz.height));
        }
        // the key's ground tells the three states apart: lit (accent), red (danger), plain (key); the pixel under the key's left edge
        const auto groundAt = [&](int n) { const auto k = findOne("Link Radio " + std::to_string(n)); const auto p = k->absolute_position(); const auto sz = k->size(); return f.at(static_cast<int>(p.x + 4), static_cast<int>(p.y + sz.height / 2)); };
        const Skin& sk = std::string(skinName) == "night" ? night : workbench;
        CHECKV(rgbOf(sk.accent).r == groundAt(1).r && rgbOf(sk.accent).g == groundAt(1).g, "%s: a linked radio's key is lit", skinName);
        CHECKV(rgbOf(sk.danger_bg).r == groundAt(3).r && rgbOf(sk.danger_bg).g == groundAt(3).g, "%s: the radio that could not follow has the danger key", skinName);
        CHECKV(rgbOf(sk.key_bg).r == groundAt(2).r && rgbOf(sk.key_bg).g == groundAt(2).g, "%s: a radio left out has the plain key", skinName);
        CHECKV(std::string(findOne("Link Radio 3")->accessible_description().value_or(slint::SharedString())).find("could not follow") != std::string::npos, "%s: the tip says it could not follow", skinName);
        // a real click names its radio (and does not select the pane)
        for (const int n : {2, 4}) {
            const auto k = findOne("Link Radio " + std::to_string(n));
            lastEvent.clear(); lastRadio = 0;
            if (k) { clickCentre(*k); }
            CHECKV(lastEvent == "pane-link" && lastRadio == n, "%s: a click on Link Radio %d gave '%s' %d", skinName, n, lastEvent.c_str(), lastRadio);
        }
        f.savePng(shotDir() + "workspace-link-" + skinName + ".png");
        g.set_cells(std::make_shared<slint::VectorModel<WorkspaceCell>>(makeCells(4)));
    }
    ui->global<Theme>().set_skin(workbench);

    return testSummary("test_slint_radio_workspace");
}
