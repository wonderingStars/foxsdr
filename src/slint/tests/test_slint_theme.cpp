// The theme is one struct with many fillings: the Workbench tokens reach the pixels, text renders headlessly,
// and a SECOND filling installed through Theme.skin changes the picture with no component change.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "render_harness.hpp"
#include "test_check.hpp"

#include "theme_specimen.h"

using namespace foxsdr::slinttest;

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

    return testSummary("test_slint_theme");
}
