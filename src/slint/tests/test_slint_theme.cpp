// The theme is one struct with many fillings: the Workbench tokens reach the pixels, text renders headlessly,
// and a SECOND filling installed through Theme.skin changes the picture with no component change.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "render_harness.hpp"
#include "test_check.hpp"

#include "theme_specimen.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

using namespace foxsdr::slinttest;

namespace {

// WCAG relative luminance and contrast ratio, the measure the classic window's test_theme holds its presets to (4.5:1 for a reading).
double channel(std::uint8_t v) { const double c = v / 255.0; return c <= 0.03928 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); }
double luminance(const slint::Color& c) { return 0.2126 * channel(c.red()) + 0.7152 * channel(c.green()) + 0.0722 * channel(c.blue()); }
double contrast(const slint::Color& a, const slint::Color& b) {
    const double la = luminance(a), lb = luminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}
// Night Watch keeps a night-adapted eye: every colour is red-led (green and blue at most nine tenths of red), so none is white.
bool redLed(const slint::Color& c) { return c.green() <= 0.9 * c.red() && c.blue() <= 0.9 * c.red(); }

}  // namespace

int main() {
    installHeadlessPlatform();
    auto ui = ThemeSpecimen::create();

    // --- Workbench: the sheet's tokens -------------------------------------------------------------------
    Frame f = render(ui, 240, 120);
    CHECK(f.width == 240 && f.height == 120);
    CHECK(f.near(230, 5, rgb(0x15171b)));          // ground
    CHECK(f.near(50, 25, rgb(0xf5a524)));          // accent key fill
    CHECK(f.near(150, 25, rgb(0x1d2026)));         // panel fill
    CHECK(f.near(110, 25, rgb(0x363b45)));         // its 1px strong border, left edge
    CHECK(f.near(109, 25, rgb(0x15171b)));         // ...and the ground just outside it
    CHECK(f.rowNear(100, 200, 220, rgb(0x2b2f37)));// the hairline
    const int ink = f.countInk(10, 60, 230, 100, rgb(0x15171b));
    CHECK(ink > 40);                               // text was drawn (fallback typeface: shape not asserted)
    f.savePng(shotDir() + "theme-workbench.png");

    // --- a second, trivial filling: made in C++ from the Workbench value, installed with one assignment ----
    Skin probe = ui->global<Workbench>().get_value();
    probe.bg = slint::Color::from_rgb_uint8(0x10, 0x20, 0x30);
    probe.accent = slint::Color::from_rgb_uint8(0x00, 0xc0, 0x40);
    probe.panel = slint::Color::from_rgb_uint8(0x40, 0x40, 0x40);
    probe.line_strong = slint::Color::from_rgb_uint8(0xff, 0x00, 0xff);
    probe.line = slint::Color::from_rgb_uint8(0xff, 0xff, 0x00);
    probe.hairline = 3.0f;
    probe.digit_face = DigitFace::Nixie;
    ui->global<Theme>().set_skin(probe);
    Frame g = render(ui, 240, 120);
    CHECK(g.near(230, 5, rgb(0x102030)));
    CHECK(g.near(50, 25, rgb(0x00c040)));
    CHECK(g.near(150, 25, rgb(0x404040)));
    CHECK(g.near(110, 25, rgb(0xff00ff)));
    CHECK(g.rowNear(100, 200, 220, rgb(0xffff00)));
    CHECK(g.rowNear(102, 200, 220, rgb(0xffff00)));   // the hairline is 3px now
    CHECK(ui->global<Theme>().get_digit_face() == DigitFace::Nixie);
    CHECK(g.countInk(10, 60, 230, 100, rgb(0x102030)) > 40);
    g.savePng(shotDir() + "theme-probe-filling.png");

    // --- and back: the Workbench value restores the first picture exactly --------------------------------
    ui->global<Theme>().set_skin(ui->global<Workbench>().get_value());
    Frame h = render(ui, 240, 120);
    CHECK(h.px.size() == f.px.size());
    int differing = 0;
    for (std::size_t i = 0; i < h.px.size(); ++i) {
        if (h.px[i].r != f.px[i].r || h.px[i].g != f.px[i].g || h.px[i].b != f.px[i].b) { ++differing; }
    }
    CHECK(differing == 0);

    // --- Night Watch (docs 6.61): the classic's red-black look as a second filling, ported end to end --------------------------------------
    const Skin night = ui->global<NightWatch>().get_value();
    const Skin wb = ui->global<Workbench>().get_value();
    ui->global<Theme>().set_skin(night);
    Frame n = render(ui, 240, 120);
    CHECK(n.near(230, 5, rgb(0x070303)));          // ground
    CHECK(n.near(50, 25, rgb(0xff6a4c)));          // accent key fill
    CHECK(n.near(150, 25, rgb(0x100505)));         // panel fill
    CHECK(n.near(110, 25, rgb(0x4a1a14)));         // its 1px strong border
    CHECK(n.rowNear(100, 200, 220, rgb(0x3a1410)));// the hairline
    CHECK(n.countInk(10, 60, 230, 100, rgb(0x070303)) > 40);   // text drawn
    CHECK(!n.near(230, 5, rgb(0x15171b)));         // not the Workbench ground
    // the corner of the accent key is cut by a 2 px radius (Workbench rounds 6 px): the key's very corner is still ground in Workbench, filled in Night Watch
    CHECK(n.near(10, 10, rgb(0xff6a4c)) || n.near(11, 11, rgb(0xff6a4c)));
    CHECK(f.near(10, 10, rgb(0x15171b)));
    n.savePng(shotDir() + "theme-night.png");
    CHECK(ui->global<Theme>().get_digit_face() == DigitFace::Drum);   // neon tubes
    CHECK(!ui->global<Theme>().get_shadows());                         // flat keys
    CHECK(ui->global<Theme>().get_font_ui() == slint::SharedString("JetBrains Mono"));
    CHECK(ui->global<Theme>().get_radius() == 2.0f);
    // the reading pairs the classic's test holds to 4.5:1, on the surfaces they sit on
    CHECK(contrast(night.fg, night.bg) >= 4.5);
    CHECK(contrast(night.fg, night.panel) >= 4.5);
    CHECK(contrast(night.fg_soft, night.panel) >= 4.5);
    CHECK(contrast(night.muted, night.panel) >= 4.5);
    CHECK(contrast(night.digit, night.field) >= 4.5);
    CHECK(contrast(night.accent_ink, night.accent) >= 4.5);
    CHECK(contrast(night.key_fg, night.key_bg) >= 4.5);
    CHECK(contrast(night.chip_fg, night.chip_bg) >= 4.5);
    CHECK(contrast(night.chip_good_fg, night.chip_good_bg) >= 4.5);
    CHECK(contrast(night.chip_warn_fg, night.chip_warn_bg) >= 4.5);
    CHECK(contrast(night.chip_err_fg, night.chip_err_bg) >= 4.5);
    CHECK(contrast(night.danger_fg, night.danger_bg) >= 4.5);
    CHECK(contrast(night.accent_soft, night.panel) >= 4.5);
    CHECK(contrast(night.fg, night.menu) >= 4.5);
    CHECK(contrast(night.fg, night.card) >= 4.5);
    CHECK(contrast(night.meter_ink, night.meter_face) >= 4.5);
    CHECK(contrast(night.dim, night.panel) >= 3.0);          // hints and axis labels: the quiet ink
    CHECK(contrast(night.axis_fg, night.well) >= 3.0);
    // no white, no green, no blue anywhere in the look
    for (const slint::Color& c : {night.bg, night.strip, night.panel, night.card, night.well, night.field, night.menu, night.line, night.fg, night.fg_soft, night.muted,
                                   night.dim, night.accent, night.accent_ink, night.digit, night.key_bg, night.key_fg, night.chip_fg, night.lamp_green, night.lamp_red,
                                   night.trace_live, night.trace_peak, night.trace_average, night.vfo, night.wf_floor, night.wf_mid, night.wf_high, night.wf_hot,
                                   night.wf_peak, night.meter_needle, night.meter_led_low, night.meter_led_mid, night.meter_led_high, night.level_bar}) {
        CHECK(redLed(c));
    }
    // and Workbench still is what it was: the second filling touched nothing of the first
    CHECK(wb.bg == slint::Color::from_rgb_uint8(0x15, 0x17, 0x1b));
    CHECK(wb.accent == slint::Color::from_rgb_uint8(0xf5, 0xa5, 0x24));
    ui->global<Theme>().set_skin(wb);
    Frame w2 = render(ui, 240, 120);
    int diff2 = 0;
    for (std::size_t i = 0; i < w2.px.size(); ++i) { if (w2.px[i].r != f.px[i].r || w2.px[i].g != f.px[i].g || w2.px[i].b != f.px[i].b) { ++diff2; } }
    CHECK(diff2 == 0);   // back to Workbench: pixel-identical to the first picture

    return testSummary("test_slint_theme");
}
