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
    return testSummary("test_slint_scope_modes");
}
