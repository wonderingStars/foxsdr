// The scope modes' UI family (ui/scope_modes.slint) rendered headless: the DEMOD SCOPE window's keys, steppers and display latches reach the host through the ScopeEvents global with the
// values the ImGui page's own buttons produce, the disabled states are real (a spectrum greys the time base and the attenuator; AUTO greys the attenuator's steppers), the multiplex
// key is absent unless the host says the multiplex exists, the tube reports the size its picture must be painted at, and the dead tube says its sentence. Text is a fallback typeface
// on this machine: it is asserted as ink in a region, never as glyph shapes or widths. Every callback is driven by a real pointer event at a position found by scanning the row.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "render_harness.hpp"
#include "test_check.hpp"

#include "scope_modes_specimen.h"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using namespace foxsdr::slinttest;

namespace {

constexpr int kW = 800, kH = 580;

struct Seen {
    std::vector<int> signal, display, timebase, gain;
    std::vector<bool> autos;
    std::vector<std::pair<int, int>> sized;
    int closed = 0;
};

DemodModel baseModel() {
    DemodModel m;
    m.shown = true;
    m.live = true;
    m.signal = 0;
    m.mpx_available = false;
    m.spectral = false;
    m.baseband = false;
    m.timebase_text = "10 ms/DIV";
    m.gain_text = "100 mV/DIV";
    m.auto_gain = true;
    m.display = 0;
    m.caption = "DEMODULATED AUDIO";
    m.centre = "10 ms/DIV";
    m.gain_read = "100 mV/DIV";
    return m;
}

// A picture the size of the glass, so the tube has something to show: a bright diagonal on dark.
slint::Image picture(int w, int h) {
    slint::SharedPixelBuffer<slint::Rgba8Pixel> buf(static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h));
    auto* p = buf.begin();
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const bool on = std::abs(x * h / std::max(1, w) - y) < 2;
            p[y * w + x] = on ? slint::Rgba8Pixel{180, 255, 195, 255} : slint::Rgba8Pixel{9, 18, 11, 255};
        }
    }
    return slint::Image(buf);
}

// Click along a row and return the distinct values the callback reported, in the order the keys come left to right.
template <class Vec>
Vec scanRow(slint::Window& win, int y, int x0, int x1, Vec& log) {
    Vec order;
    for (int x = x0; x < x1; x += 3) {
        const std::size_t before = log.size();
        click(win, static_cast<float>(x), static_cast<float>(y));
        for (std::size_t i = before; i < log.size(); ++i) {
            if (order.empty() || order.back() != log[i]) { order.push_back(log[i]); }
        }
    }
    return order;
}


// ======================================================================================================================================================================
// THE RADAR SCOPE: every callback of the view through a real pointer event, found by scanning, and the picture's pieces by ink.
// ======================================================================================================================================================================
struct RadarSeen {
    std::vector<int> range, wheel, tab, knob, option;
    std::vector<std::string> contact;
    std::vector<std::pair<float, float>> drags, released;
    std::vector<bool> releasedDragged;
    std::vector<std::pair<int, int>> sized;
    int exit = 0, power = 0, doubles = 0;
};

RadarScopeModel radarModel() {
    RadarScopeModel m;
    m.shown = true;
    m.has_rx = true;
    m.range_text = "200 NM";
    m.can_down = true;
    m.can_up = true;
    m.plotted_text = "2 PLOTTED";
    m.mode_text = "MODE ADS-B";
    m.rx_text = "51.5N 000.1W";
    m.range_corner = "200 NM";
    m.sweep_on = false;
    m.sig_lit = 9;
    m.sig_text = "-50";
    m.alt_lit = 5;
    m.alt_text = "FL350";
    m.drum_caption = "SET RANGE NM";
    m.drum_range = "0200";
    m.drum_tracks = "002";
    m.knob_dead = false;
    m.knob_frac = 0.5f;
    m.knob_readout = "30 dB";
    m.knob_note = "LNA";
    m.power_on = true;
    m.lcd_shown = true;
    m.lcd_screen = 1;
    m.flight_title = "NOTHING SELECTED";
    m.flight_body = "Click an aircraft on the scope to see its details.";
    return m;
}

void radarFamily() {
    auto ui = RadarSpecimen::create();
    slint::Window& win = ui->window();
    RadarSeen r;
    auto& ev = ui->global<ScopeEvents>();
    ev.on_radar_exit([&] { ++r.exit; });
    ev.on_radar_range([&](int d) { r.range.push_back(d); });
    ev.on_radar_wheel([&](int d) { r.wheel.push_back(d); });
    ev.on_radar_drag([&](float dx, float dy) { r.drags.push_back({dx, dy}); });
    ev.on_radar_released([&](float x, float y, bool dragged) { r.released.push_back({x, y}); r.releasedDragged.push_back(dragged); });
    ev.on_radar_double_clicked([&] { ++r.doubles; });
    ev.on_radar_resized([&](int w, int h) { r.sized.push_back({w, h}); });
    ev.on_radar_lcd_tab([&](int i) { r.tab.push_back(i); });
    ev.on_radar_contact([&](slint::SharedString id) { r.contact.push_back(std::string(id)); });
    ev.on_radar_option([&](int i) { r.option.push_back(i); });
    ev.on_radar_knob([&](int d) { r.knob.push_back(d); });
    ev.on_radar_power([&] { ++r.power; });

    // ---- the cabinet draws, the tube reports its square ----
    RadarScopeModel m = radarModel();
    ui->set_radar(m);
    Frame f = render(ui, 1240, 700);
    CHECK(!r.sized.empty());
    int side = 0;
    if (!r.sized.empty()) {
        side = r.sized.back().first;
        std::printf("  the radar tube is %d x %d\n", r.sized.back().first, r.sized.back().second);
        CHECK(r.sized.back().first == r.sized.back().second);
        CHECK(side > 400 && side < 600);
    }
    f.savePng(shotDir() + "scope-modes-radar.png");

    // ---- the tube's own frame: where it is, from where a click lands ----
    click(win, 400.0f, 300.0f);
    CHECK(r.released.size() == 1 && r.releasedDragged.size() == 1);
    float ox = 0, oy = 0;
    if (!r.released.empty()) {
        CHECK(!r.releasedDragged[0]);   // a click that stayed put is not a drag
        ox = 400.0f - r.released[0].first;
        oy = 300.0f - r.released[0].second;
        std::printf("  the tube starts at %.0f, %.0f\n", ox, oy);
        CHECK(ox > 150 && ox < 260 && oy > 40 && oy < 70);
    }
    // a one-pixel wobble is still a click; a press moved over 2 px is a drag, reported as the travel since the last report, and its release says so
    r.released.clear(); r.releasedDragged.clear(); r.drags.clear();
    press(win, 400.0f, 300.0f);
    moveTo(win, 401.0f, 300.0f);
    release(win, 401.0f, 300.0f);
    CHECK(r.drags.empty() && r.releasedDragged.size() == 1 && !r.releasedDragged[0]);
    r.released.clear(); r.releasedDragged.clear();
    press(win, 400.0f, 300.0f);
    moveTo(win, 410.0f, 305.0f);
    moveTo(win, 415.0f, 305.0f);
    release(win, 415.0f, 305.0f);
    CHECK(r.drags.size() == 2);
    if (r.drags.size() == 2) {
        CHECK_NEAR(r.drags[0].first, 10.0, 0.6);
        CHECK_NEAR(r.drags[0].second, 5.0, 0.6);
        CHECK_NEAR(r.drags[1].first, 5.0, 0.6);
        CHECK_NEAR(r.drags[1].second, 0.0, 0.6);
    }
    CHECK(r.releasedDragged.size() == 1 && r.releasedDragged[0]);
    // the wheel: up is +1, down is -1
    scroll(win, 400.0f, 300.0f, 0.0f, 120.0f);
    scroll(win, 400.0f, 300.0f, 0.0f, -120.0f);
    CHECK((r.wheel == std::vector<int>{1, -1}));
    // a double click
    click(win, 420.0f, 320.0f);
    click(win, 420.0f, 320.0f);
    CHECK(r.doubles >= 1);

    // ---- the bar: EXIT SCOPE, then the range keys (each disabled at its own end) ----
    std::vector<int> order;
    for (int x = 8; x < 420; x += 3) {
        const int before = r.exit;
        const std::size_t rb = r.range.size();
        click(win, static_cast<float>(x), 22.0f);
        if (r.exit != before && (order.empty() || order.back() != 0)) { order.push_back(0); }
        for (std::size_t i = rb; i < r.range.size(); ++i) { if (order.empty() || order.back() != r.range[i]) { order.push_back(r.range[i]); } }
    }
    CHECK((order == std::vector<int>{0, -1, 1}));
    m.can_down = false;
    ui->set_radar(m);
    render(ui, 1240, 700);
    r.range.clear();
    for (int x = 8; x < 420; x += 3) { click(win, static_cast<float>(x), 22.0f); }
    {
        std::vector<int> dedup;
        for (const int v : r.range) { if (dedup.empty() || dedup.back() != v) { dedup.push_back(v); } }
        CHECK((dedup == std::vector<int>{1}));   // "-" is dead at the bottom of the ladder
    }
    m.can_down = true;
    ui->set_radar(m);
    render(ui, 1240, 700);

    // ---- the LCD: three tabs, a list of contacts, four options ----
    r.tab.clear();
    std::vector<int> tabs;
    for (int x = 800; x < 1130; x += 3) {
        const std::size_t b = r.tab.size();
        click(win, static_cast<float>(x), 72.0f);
        for (std::size_t i = b; i < r.tab.size(); ++i) { if (tabs.empty() || tabs.back() != r.tab[i]) { tabs.push_back(r.tab[i]); } }
    }
    CHECK((tabs == std::vector<int>{0, 1, 2}));
    m.lcd_screen = 0;
    m.home_count = "2";
    m.home_sub = "of 2 tracked   200 NM";
    std::vector<RadarContactItem> cs;
    cs.push_back(RadarContactItem{"4D2025", "FOXSDR1", "FL350", "12 NM   045 deg", slint::Color::from_argb_uint8(255, 255, 136, 0), false});
    cs.push_back(RadarContactItem{"4D2026", "G-FOXS", "FL100", "40 NM   270 deg", slint::Color::from_argb_uint8(255, 0, 200, 255), false});
    m.contacts = std::make_shared<slint::VectorModel<RadarContactItem>>(cs);
    ui->set_radar(m);
    render(ui, 1240, 700).savePng(shotDir() + "scope-modes-radar-home.png");
    r.contact.clear();
    std::vector<std::string> ids;
    for (int y = 130; y < 520; y += 4) {
        const std::size_t b = r.contact.size();
        click(win, 900.0f, static_cast<float>(y));
        for (std::size_t i = b; i < r.contact.size(); ++i) { if (ids.empty() || ids.back() != r.contact[i]) { ids.push_back(r.contact[i]); } }
    }
    std::printf("  contacts clicked:");
    for (const std::string& i : ids) { std::printf(" %s", i.c_str()); }
    std::printf(" (raw %zu)\n", r.contact.size());
    CHECK((ids == std::vector<std::string>{"4D2025", "4D2026"}));
    m.lcd_screen = 2;
    m.options = std::make_shared<slint::VectorModel<RadarOptionItem>>(std::vector<RadarOptionItem>{{"PHOSPHOR", "ON"}, {"SWEEP", "ON"}, {"TRAILS", "LINE"}, {"FILTER", "ALL"}});
    ui->set_radar(m);
    render(ui, 1240, 700);
    r.option.clear();
    std::vector<int> opts;
    for (int y = 90; y < 400; y += 3) {
        const std::size_t b = r.option.size();
        click(win, 900.0f, static_cast<float>(y));
        for (std::size_t i = b; i < r.option.size(); ++i) { if (opts.empty() || opts.back() != r.option[i]) { opts.push_back(r.option[i]); } }
    }
    CHECK((opts == std::vector<int>{0, 1, 2, 3}));

    // ---- the deck: the knob turns on the wheel and answers nothing when dead; POWER is a key ----
    r.knob.clear();
    for (int y = 590; y < 695; y += 6) { for (int x = 300; x < 1000; x += 6) { scroll(win, static_cast<float>(x), static_cast<float>(y), 0.0f, 120.0f); } }
    CHECK(!r.knob.empty());
    for (const int d : r.knob) { CHECK(d == 1); }
    m.knob_dead = true;
    ui->set_radar(m);
    render(ui, 1240, 700);
    r.knob.clear();
    for (int y = 590; y < 695; y += 6) { for (int x = 300; x < 1000; x += 6) { scroll(win, static_cast<float>(x), static_cast<float>(y), 0.0f, 120.0f); } }
    CHECK(r.knob.empty());
    r.power = 0;
    for (int y = 590; y < 695; y += 6) { for (int x = 300; x < 1000; x += 6) { click(win, static_cast<float>(x), static_cast<float>(y)); } }
    CHECK(r.power >= 1);

    // ---- the aircraft: a marker in its colour at its place, its label beside it ----
    m = radarModel();
    std::vector<RadarTargetItem> ts;
    ts.push_back(RadarTargetItem{"4D2025", 200.0f, 200.0f, 90.0f, slint::Color::from_argb_uint8(255, 255, 136, 0), 18.0f, "FOXSDR1", false, true, true});
    ts.push_back(RadarTargetItem{"4D2026", 300.0f, 320.0f, 0.0f, slint::Color::from_argb_uint8(255, 0, 200, 255), 18.0f, "G-FOXS", true, false, true});
    m.targets = std::make_shared<slint::VectorModel<RadarTargetItem>>(ts);
    ui->set_radar(m);
    Frame t = render(ui, 1240, 700);
    t.savePng(shotDir() + "scope-modes-radar-targets.png");
    const int ax = static_cast<int>(ox) + 200, ay = static_cast<int>(oy) + 200;
    CHECK(t.countNear(ax - 14, ay - 14, ax + 14, ay + 14, rgb(0xff8800), 40) > 8);
    const int bx = static_cast<int>(ox) + 300, by = static_cast<int>(oy) + 320;
    CHECK(t.countNear(bx - 14, by - 14, bx + 14, by + 14, rgb(0x00c8ff), 40) > 8);
    CHECK(t.countInk(ax + 16, ay - 8, ax + 90, ay + 8, t.at(ax + 60, ay - 20), 40) > 10);   // the label beside it

    // ---- the empty state: no position, no tube, the sentence and the offers instead ----
    m = radarModel();
    m.has_rx = false;
    m.warning = "The scope needs the receiver's position.";
    m.intro = "Everything on a radar scope is a range and a bearing measured from the antenna.";
    ui->set_radar(m);
    Frame e = render(ui, 1240, 700);
    e.savePng(shotDir() + "scope-modes-radar-empty.png");
    const Rgb bg = e.at(1000, 400);
    CHECK(e.countInk(8, 50, 540, 110, bg, 40) > 100);
    CHECK(e.countInk(600, 150, 1200, 650, bg, 40) < 5);   // no cabinet
}

}  // namespace

int main() {
    installHeadlessPlatform();
    auto ui = ScopeModesSpecimen::create();
    slint::Window& win = ui->window();
    Seen seen;
    auto& ev = ui->global<ScopeEvents>();
    ev.on_demod_signal([&](int i) { seen.signal.push_back(i); });
    ev.on_demod_display([&](int i) { seen.display.push_back(i); });
    ev.on_demod_timebase([&](int d) { seen.timebase.push_back(d); });
    ev.on_demod_gain([&](int d) { seen.gain.push_back(d); });
    ev.on_demod_auto([&](bool v) { seen.autos.push_back(v); });
    ev.on_demod_resized([&](int w, int h) { seen.sized.push_back({w, h}); });
    ev.on_demod_closed([&] { ++seen.closed; });

    // ---- the window draws, and the tube reports the size its picture must be painted at ----
    ui->set_demod(baseModel());
    Frame f = render(ui, kW, kH);
    CHECK(f.width == kW && f.height == kH);
    CHECK(!seen.sized.empty());
    int gw = 0, gh = 0;
    if (!seen.sized.empty()) {
        gw = seen.sized.back().first;
        gh = seen.sized.back().second;
        std::printf("  the glass is %d x %d\n", gw, gh);
        CHECK(gw > 600 && gw < 780);
        CHECK(gh > 250 && gh < 450);
    }
    // the painted picture fills the glass
    DemodModel m = baseModel();
    m.picture = picture(gw > 0 ? gw : 400, gh > 0 ? gh : 300);
    ui->set_demod(m);
    f = render(ui, kW, kH);
    f.savePng(shotDir() + "scope-modes-demod.png");
    // the diagonal's bright pixels are on the screen, somewhere inside the tube
    CHECK(f.countNear(20, 150, 780, 560, rgb(0xb4ffc3), 40) > 200);

    // ---- the input selector: four keys, the fifth absent unless the multiplex exists ----
    Seen& s = seen;
    s.signal.clear();
    std::vector<int> order = scanRow(win, 53, 12, 500, s.signal);
    CHECK((order == std::vector<int>{0, 1, 2, 3}));
    m.mpx_available = true;
    ui->set_demod(m);
    render(ui, kW, kH);
    s.signal.clear();
    order = scanRow(win, 53, 12, 500, s.signal);
    CHECK((order == std::vector<int>{0, 1, 2, 3, 4}));
    m.mpx_available = false;
    ui->set_demod(m);
    render(ui, kW, kH);

    // ---- the time base and the attenuator ----
    s.timebase.clear();
    s.gain.clear();
    s.autos.clear();
    // AUTO ticked: the attenuator's steppers are dead, the time base's work
    order = scanRow(win, 83, 12, 500, s.timebase);
    CHECK(!s.timebase.empty());
    CHECK((order == std::vector<int>{-1, 1}));
    CHECK(s.gain.empty());
    CHECK(!s.autos.empty());   // the AUTO tick answered the scan
    if (!s.autos.empty()) { CHECK(s.autos.front() == false); }   // it asked for the opposite of what the model shows
    // AUTO off: the attenuator answers
    m.auto_gain = false;
    ui->set_demod(m);
    render(ui, kW, kH);
    s.timebase.clear();
    s.gain.clear();
    scanRow(win, 83, 12, 500, s.timebase);
    {
        std::vector<int> g;
        for (const int v : s.gain) { if (g.empty() || g.back() != v) { g.push_back(v); } }
        CHECK((g == std::vector<int>{-1, 1}));
    }
    // a spectrum greys both groups: the time base's steppers and the attenuator's and the tick answer nothing
    m.spectral = true;
    m.signal = 1;
    m.timebase_text = "-- (spectrum)";
    m.gain_text = "10 dB/DIV";
    ui->set_demod(m);
    render(ui, kW, kH);
    s.timebase.clear();
    s.gain.clear();
    s.autos.clear();
    scanRow(win, 83, 12, 500, s.timebase);
    CHECK(s.timebase.empty() && s.gain.empty() && s.autos.empty());
    m.spectral = false;
    m.signal = 0;
    m.timebase_text = "10 ms/DIV";
    m.gain_text = "100 mV/DIV";
    ui->set_demod(m);
    render(ui, kW, kH);

    // ---- the display latches ----
    s.display.clear();
    order = scanRow(win, 113, 12, 500, s.display);
    CHECK((order == std::vector<int>{0, 1, 2}));

    // ---- the dead tube's sentence is ink in the middle of the glass, and the readout row is ink along its foot ----
    m.picture = slint::Image();
    m.dead = "NO SAMPLES - RECEIVER STOPPED";
    m.centre = "";
    m.gain_read = "";
    m.caption = "";
    ui->set_demod(m);
    Frame dead = render(ui, kW, kH);
    dead.savePng(shotDir() + "scope-modes-demod-dead.png");
    const int midY = 40 + 90 + 11 + gh / 2;
    const Rgb glassCol = dead.at(60, midY - 40);
    CHECK(dead.countInk(60, midY - 12, 740, midY + 12, glassCol, 40) > 40);
    m.dead = "";
    ui->set_demod(m);
    Frame alive = render(ui, kW, kH);
    CHECK(alive.countInk(60, midY - 12, 740, midY + 12, glassCol, 40) < 5);   // the graticule is the picture's, not this family's
    // the foot's words
    m.caption = "DEMODULATED AUDIO";
    m.centre = "10 ms/DIV";
    m.gain_read = "100 mV/DIV";
    ui->set_demod(m);
    Frame foot = render(ui, kW, kH);
    CHECK(foot.countInk(60, 40 + 90 + 11 + gh - 22, 740, 40 + 90 + 11 + gh - 4, glassCol, 40) > 60);
    // a plugin's legend at the top left
    m.audio_from = "AUDIO FROM dab-radio";
    ui->set_demod(m);
    Frame legend = render(ui, kW, kH);
    CHECK(legend.countInk(60, 40 + 90 + 11 + 3, 300, 40 + 90 + 11 + 20, glassCol, 40) > 20);

    // ---- the multiplex's names appear only where they fit ----
    m.audio_from = "";
    m.signal = 4;
    m.mpx_available = true;
    std::vector<MpxMarkItem> marks;
    marks.push_back(MpxMarkItem{0.0f, 0.15f, 0.15f, "Mono L+R", false, false});       // a wide band: its name fits
    marks.push_back(MpxMarkItem{0.20f, 0.21f, 0.2f, "RDS", false, true});              // a band too narrow for its name: no label
    marks.push_back(MpxMarkItem{0.19f, 0.19f, 0.18f, "Pilot", true, false});           // a tone with room
    m.marks = std::make_shared<slint::VectorModel<MpxMarkItem>>(marks);
    ui->set_demod(m);
    Frame mpx = render(ui, kW, kH);
    mpx.savePng(shotDir() + "scope-modes-demod-mpx.png");
    const int top = 40 + 90 + 11;
    CHECK(mpx.countInk(14, top + 12, 14 + static_cast<int>(0.15 * gw), top + 26, glassCol, 40) > 15);       // Mono L+R under its bracket
    CHECK(mpx.countInk(14 + static_cast<int>(0.20 * gw), top + 12, 14 + static_cast<int>(0.215 * gw), top + 22, glassCol, 40) == 0);   // RDS does not fit
    CHECK(mpx.countInk(14 + static_cast<int>(0.19 * gw) + 3, top + 24, 14 + static_cast<int>(0.19 * gw) + 60, top + 38, glassCol, 40) > 8);   // Pilot beside its rule

    // ---- the window's chrome: close tells the host ----
    CHECK(s.closed == 0);
    click(win, 762.0f, 16.0f);
    CHECK(s.closed == 1);
    radarFamily();
    return testSummary("test_slint_scope_modes");
}
