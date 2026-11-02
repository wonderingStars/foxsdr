// The tabs family (ui/tabs.slint): Tab, WindowChrome, TornWindow and Splitter, rendered headlessly from the specimen
// sheet and probed against the artboards (Workbench.dc.html's tab row and splitter, Workbench-PluginKit.dc.html's
// torn-off window). Geometry and colours are asserted; the text is in a fallback typeface on this machine, so for
// text only "ink in this region" is. The pointer is dispatched the way the OS backend would deliver it.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "render_harness.hpp"
#include "test_check.hpp"

#include "tabs_specimen.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cmath>
#include <string>
#include <string_view>

using namespace foxsdr::slinttest;

namespace {

// The Workbench sheet's tokens (the <helmet><style> blocks of the two artboards).
constexpr Rgb kBg = rgb(0x15171b);          // the specimen window
constexpr Rgb kPanel = rgb(0x1d2026);       // .win ground, the splitter
constexpr Rgb kMenu = rgb(0x262a32);        // .wt ground, a tooltip
constexpr Rgb kMenuHot = rgb(0x363b45);     // a hovered key
constexpr Rgb kLine = rgb(0x2b2f37);        // the splitter's hairlines
constexpr Rgb kLineStrong = rgb(0x363b45);  // .wt border-bottom
constexpr Rgb kKnobRing = rgb(0x434955);    // .win border, the splitter grip
constexpr Rgb kFg = rgb(0xe6e8ec);
constexpr Rgb kFgSoft = rgb(0xc9ced6);      // the chrome icons
constexpr Rgb kMuted = rgb(0x9aa3ad);       // an off tab's label
constexpr Rgb kDim = rgb(0x7d8694);         // the ghost tab's label
constexpr Rgb kDisabled = rgb(0x8a93a0);
constexpr Rgb kAccent = rgb(0xf5a524);      // the underline, the lit grip
constexpr Rgb kLampAmber = rgb(0xf5a524);   // the torn-off dot
constexpr Rgb kNudge = rgb(0x6c7480);       // the resize grip's hatch
constexpr Rgb kCard = rgb(0x20242b);
constexpr Rgb kField = rgb(0x0f1114);

// The specimen's table (the header of tabs_specimen.slint).
constexpr int kTabY = 16, kTabH = 32;
constexpr int kOnX = 16, kOffX = 146, kTornX = 276, kCloseX = 406, kGhostX = 536;
constexpr int kChromeX = 16, kChromeW = 300, kChromeY = 72, kMaxY = 112, kRolledY = 152, kLongChromeY = 232, kLongChromeW = 220;
constexpr int kSplitX = 16, kSplitY = 200, kSplitW = 400;
constexpr int kWinX = 440, kWinY = 72, kWinW = 300, kWinH = 200, kRolledWinY = 320;
constexpr int kW = 800, kH = 440;

struct Box {
    int x0 = 0, y0 = 0, x1 = -1, y1 = -1, n = 0;   // inclusive bounds and the pixel count
    int w() const { return n > 0 ? x1 - x0 + 1 : 0; }
    int h() const { return n > 0 ? y1 - y0 + 1 : 0; }
    double cx() const { return (x0 + x1) / 2.0; }
    double cy() const { return (y0 + y1) / 2.0; }
};

void grow(Box& b, int x, int y) {
    if (b.n == 0) { b.x0 = b.x1 = x; b.y0 = b.y1 = y; }
    b.x0 = std::min(b.x0, x); b.x1 = std::max(b.x1, x);
    b.y0 = std::min(b.y0, y); b.y1 = std::max(b.y1, y);
    ++b.n;
}

// The bounding box of everything in [x0,x1) x [y0,y1) that differs from `bg`.
Box inkBox(const Frame& f, int x0, int y0, int x1, int y1, Rgb bg, int tol = 24) {
    Box b;
    for (int y = y0; y < y1; ++y) { for (int x = x0; x < x1; ++x) { if (!f.near(x, y, bg, tol)) { grow(b, x, y); } } }
    return b;
}

// The bounding box of the pixels near `colour`.
Box colourBox(const Frame& f, int x0, int y0, int x1, int y1, Rgb colour, int tol = 10) {
    Box b;
    for (int y = y0; y < y1; ++y) { for (int x = x0; x < x1; ++x) { if (f.near(x, y, colour, tol)) { grow(b, x, y); } } }
    return b;
}

// The brightest pixel of a region: a light glyph on a dark ground reaches its own colour somewhere in a stem.
Rgb brightest(const Frame& f, int x0, int y0, int x1, int y1) {
    Rgb best{};
    int bestLum = -1;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const Rgb p = f.at(x, y);
            const int lum = p.r + p.g + p.b;
            if (lum > bestLum) { bestLum = lum; best = p; }
        }
    }
    return best;
}

// How far a pixel is from `bg` towards `ink`, 0..1 (a thin anti-aliased stroke reaches only part of the way).
double strength(Rgb p, Rgb ink, Rgb bg) {
    const auto lum = [](Rgb c) { return 0.299 * c.r + 0.587 * c.g + 0.114 * c.b; };
    const double span = lum(ink) - lum(bg);
    return span == 0 ? 0 : (lum(p) - lum(bg)) / span;
}

std::string lastClick(const TabsSpecimen& ui) { return std::string(std::string_view(ui.get_last_click())); }
void ms(int n) { settle(std::chrono::milliseconds(n)); }

// A press at (x, y), moves through `steps` (absolute positions), a release at the last one.
void dragThrough(slint::Window& win, int x, int y, std::initializer_list<std::pair<int, int>> steps) {
    moveTo(win, static_cast<float>(x), static_cast<float>(y));
    press(win, static_cast<float>(x), static_cast<float>(y));
    int lx = x, ly = y;
    for (const auto& s : steps) {
        moveTo(win, static_cast<float>(s.first), static_cast<float>(s.second));
        lx = s.first;
        ly = s.second;
    }
    release(win, static_cast<float>(lx), static_cast<float>(ly));
}

}  // namespace

int main() {
    installHeadlessPlatform();
    auto ui = TabsSpecimen::create();
    slint::Window& win = ui->window();

    Frame f = render(ui, kW, kH);
    CHECK(f.width == kW && f.height == kH);
    f.savePng(shotDir() + "tabs-specimen.png");

    // --- a second filling installed through Theme.skin restyles the tabs with no component change ------------------
    {
        Skin probe = ui->global<Workbench>().get_value();
        probe.accent = slint::Color::from_rgb_uint8(0x00, 0xc0, 0x40);
        probe.menu = slint::Color::from_rgb_uint8(0x30, 0x10, 0x50);
        probe.knob_ring = slint::Color::from_rgb_uint8(0xff, 0x00, 0xff);
        probe.panel = slint::Color::from_rgb_uint8(0x40, 0x40, 0x40);
        probe.line_strong = slint::Color::from_rgb_uint8(0xff, 0xff, 0x00);
        probe.fg = slint::Color::from_rgb_uint8(0x00, 0xff, 0xff);
        ui->global<Theme>().set_skin(probe);
        Frame g0 = render(ui, kW, kH);
        CHECK(g0.rowNear(kTabY + kTabH - 2, kOnX, kOnX + 96, rgb(0x00c040)));                  // the underline is Theme.accent
        CHECK(g0.rowNear(kChromeY, kChromeX, kChromeX + kChromeW, rgb(0x301050)));             // the chrome ground is Theme.menu
        CHECK(g0.rowNear(kChromeY + 29, kChromeX, kChromeX + kChromeW, rgb(0xffff00)));        // its rule is Theme.line-strong
        CHECK(g0.rowNear(kWinY, kWinX + 9, kWinX + kWinW - 9, rgb(0xff00ff)));                 // the window border is Theme.knob-ring
        CHECK(g0.near(kWinX + 5, kWinY + 32, rgb(0x404040), 3));                               // its ground is Theme.panel
        CHECK(g0.rowNear(kSplitY + 1, kSplitX, kSplitX + 170, rgb(0x404040)));                 // the splitter's ground is Theme.panel
        CHECK(g0.rowNear(kSplitY + 2, kSplitX + 178, kSplitX + 222, rgb(0xff00ff)));           // its grip is Theme.knob-ring
        CHECK(nearColour(brightest(g0, kOnX, kTabY + 2, kOnX + 96, kTabY + kTabH - 3), rgb(0x00ffff), 40));   // the on label is Theme.fg
        ui->global<Theme>().set_skin(ui->global<Workbench>().get_value());       // and back: the first picture exactly
        Frame g1 = render(ui, kW, kH);
        int differing = 0;
        for (std::size_t i = 0; i < g1.px.size(); ++i) {
            if (g1.px[i].r != f.px[i].r || g1.px[i].g != f.px[i].g || g1.px[i].b != f.px[i].b) { ++differing; }
        }
        CHECK(g1.px.size() == f.px.size() && differing == 0);
    }

    // ======================================================================================= Tab ==============
    // --- geometry: 32 px high, 96 px at least -----------------------------------------------------------------
    CHECK_NEAR(ui->get_h_on(), 32, 0.01);
    CHECK(ui->get_w_on() >= 96 && ui->get_w_off() >= 96 && ui->get_w_torn() >= 96 && ui->get_w_close() >= 96);
    CHECK_NEAR(ui->get_w_off(), 96, 0.01);                             // "PATCH" is short: the 96 px minimum applies
    CHECK(ui->get_w_ghost() < 96);                                     // the "+ Window" tab has no minimum width
    const int onW = static_cast<int>(std::lround(ui->get_w_on()));
    const int offW = static_cast<int>(std::lround(ui->get_w_off()));
    const int closeW = static_cast<int>(std::lround(ui->get_w_close()));
    const int ghostW = static_cast<int>(std::lround(ui->get_w_ghost()));

    // --- the 2 px accent underline: on has it, off has not -----------------------------------------------------
    CHECK(f.rowNear(kTabY + kTabH - 2, kOnX, kOnX + onW, kAccent));
    CHECK(f.rowNear(kTabY + kTabH - 1, kOnX, kOnX + onW, kAccent));
    CHECK(f.rowNear(kTabY + kTabH - 3, kOnX, kOnX + onW, kBg));        // exactly two rows
    CHECK(f.near(kOnX - 1, kTabY + kTabH - 1, kBg) && f.near(kOnX + onW, kTabY + kTabH - 1, kBg));   // exactly the tab's width
    CHECK(f.rowNear(kTabY + kTabH - 1, kOffX, kOffX + offW, kBg));     // off: nothing under the label
    CHECK(f.rowNear(kTabY + kTabH - 2, kOffX, kOffX + offW, kBg));
    CHECK(f.countNear(kOffX, kTabY, kOffX + offW, kTabY + kTabH, kAccent) == 0);
    CHECK(f.rowNear(kTabY, kOnX, kOnX + onW, kBg));                    // the tab itself is transparent

    // --- label ink: fg when on, muted when off, dim when ghost ---------------------------------------------------
    {
        const Box on = inkBox(f, kOnX, kTabY + 2, kOnX + onW, kTabY + kTabH - 3, kBg);
        CHECK(on.n > 40);
        CHECK(nearColour(brightest(f, kOnX, kTabY + 2, kOnX + onW, kTabY + kTabH - 3), kFg, 40));
        CHECK(on.cx() > kOnX + onW / 2 - 4 && on.cx() < kOnX + onW / 2 + 4);          // centred in the tab
        CHECK(on.cy() >= kTabY + 13 && on.cy() <= kTabY + 17);                        // centred above the underline
        CHECK(on.x0 >= kOnX + 16 && on.x1 < kOnX + onW - 16 + 2);                     // inside the 16 px padding
        const Box off = inkBox(f, kOffX, kTabY + 2, kOffX + offW, kTabY + kTabH - 3, kBg);
        CHECK(off.n > 30);
        CHECK(nearColour(brightest(f, kOffX, kTabY + 2, kOffX + offW, kTabY + kTabH - 3), kMuted, 40));
        CHECK(!nearColour(brightest(f, kOffX, kTabY + 2, kOffX + offW, kTabY + kTabH - 3), kFg, 40));
        const Box ghost = inkBox(f, kGhostX, kTabY + 2, kGhostX + ghostW, kTabY + kTabH - 3, kBg);
        CHECK(ghost.n > 30);
        CHECK(nearColour(brightest(f, kGhostX, kTabY + 2, kGhostX + ghostW, kTabY + kTabH - 3), kDim, 40));
        CHECK(ghost.x0 >= kGhostX + 16 && ghost.x1 < kGhostX + ghostW - 16 + 2);      // padding 0 16, no minimum
    }

    // --- torn off: an amber dot before the label -----------------------------------------------------------------
    {
        const int tornW = static_cast<int>(std::lround(ui->get_w_torn()));
        const Box core = colourBox(f, kTornX, kTabY, kTornX + tornW, kTabY + kTabH, kLampAmber, 6);
        CHECK(core.n >= 30);                                                          // the lit core of an 8 px disc
        const Box dot = inkBox(f, core.x0 - 2, core.y0 - 2, core.x1 + 3, core.y1 + 3, kBg, 12);   // ...with its rim
        CHECK(dot.w() == 8 && dot.h() == 8);                                          // Theme.lamp-dot-sm
        CHECK(dot.cy() >= kTabY + 13 && dot.cy() <= kTabY + 16);                      // centred on the label
        const Box label = inkBox(f, static_cast<int>(dot.x1) + 3, kTabY + 2, kTornX + tornW, kTabY + kTabH - 3, kBg);
        CHECK(label.n > 30);
        CHECK(label.x0 >= dot.x1 + 5 && label.x0 <= dot.x1 + 9);                      // the 6 px gap
        CHECK(f.countNear(kTornX, kTabY + kTabH - 2, kTornX + tornW, kTabY + kTabH, kAccent) == 0);   // not on: no underline
        // the dot and the label together are centred in the tab
        const Box all = inkBox(f, kTornX, kTabY + 2, kTornX + tornW, kTabY + kTabH - 3, kBg);
        const int leftMargin = all.x0 - kTornX, rightMargin = (kTornX + tornW - 1) - all.x1;
        CHECK(std::abs(leftMargin - rightMargin) <= 2);                               // equal air each side (glyph bearings aside)
        CHECK(std::abs(all.cx() - (kTornX + tornW / 2.0)) <= 3);
        // the other tabs have no dot
        CHECK(f.countNear(kOnX, kTabY, kOnX + onW, kTabY + kTabH - 3, kLampAmber, 6) == 0);
        CHECK(f.countNear(kOffX, kTabY, kOffX + offW, kTabY + kTabH, kLampAmber, 6) == 0);
    }

    // --- closable: the 12 px close icon after the label, in the muted colour ---------------------------------------
    int closeCx = 0, closeCy = 0;
    {
        CHECK(closeW > 96);                                                           // label + 6 + key + 32 is over the minimum
        const Box icon = inkBox(f, kCloseX + closeW - 34, kTabY + 4, kCloseX + closeW - 14, kTabY + kTabH - 4, kBg);
        CHECK(icon.n > 10);
        CHECK(icon.w() >= 9 && icon.w() <= 13 && icon.h() >= 9 && icon.h() <= 13);    // a 12 px icon (strokes inset a little)
        CHECK(nearColour(brightest(f, icon.x0, icon.y0, icon.x1 + 1, icon.y1 + 1), kMuted, 40));
        CHECK(!f.near(static_cast<int>(icon.cx()), static_cast<int>(icon.cy()), kBg, 10));   // an X: ink at its centre
        CHECK(std::abs(icon.cx() - (kCloseX + closeW - 24)) <= 2);                    // key right edge is the 16 px padding
        closeCx = static_cast<int>(std::lround(icon.cx()));
        closeCy = static_cast<int>(std::lround(icon.cy()));
        const Box label = inkBox(f, kCloseX + 8, kTabY + 2, icon.x0 - 3, kTabY + kTabH - 3, kBg);
        CHECK(label.n > 30);
        CHECK(label.x1 < icon.x0 - 4);                                                // the 6 px gap before the key
        CHECK(label.x0 >= kCloseX + 16 - 1);
    }

    // --- interaction: clicked, and drag-out only after more than 24 px DOWN ------------------------------------------
    CHECK(ui->get_clicks() == 0 && ui->get_drag_outs() == 0 && ui->get_close_clicks() == 0);
    click(win, kOnX + 48, kTabY + 16);
    CHECK(ui->get_clicks() == 1 && lastClick(*ui) == "on");
    click(win, kOffX + 48, kTabY + 16);
    CHECK(ui->get_clicks() == 2 && lastClick(*ui) == "off");
    click(win, kTornX + 48, kTabY + 16);
    CHECK(ui->get_clicks() == 3 && lastClick(*ui) == "torn");
    click(win, kGhostX + ghostW / 2, kTabY + 16);
    CHECK(ui->get_clicks() == 4 && lastClick(*ui) == "ghost");
    click(win, kCloseX + 30, kTabY + 16);                                  // the label of the closable tab
    CHECK(ui->get_clicks() == 5 && lastClick(*ui) == "close");
    CHECK(ui->get_close_clicks() == 0);

    // the close key: close-clicked, and clicked does NOT fire for that click
    click(win, closeCx, closeCy);
    CHECK(ui->get_close_clicks() == 1);
    CHECK(ui->get_clicks() == 5);
    click(win, kCloseX + closeW - 24, kTabY + 14);                         // the key's own box, off the strokes
    CHECK(ui->get_close_clicks() == 2 && ui->get_clicks() == 5);

    // a 10 px pull is still a click
    ui->set_clicks(0);
    const int pulls0 = ui->get_drag_outs();                                // counted relatively from here on
    dragThrough(win, kOnX + 48, kTabY + 10, {{kOnX + 48, kTabY + 20}});
    CHECK(ui->get_drag_outs() == pulls0);
    CHECK(ui->get_clicks() == 1);                                          // released inside the tab: a click
    // a 30 px pull down: drag-out, once, and the release is not a click
    ui->set_clicks(0);
    moveTo(win, kOnX + 48, kTabY + 10);
    press(win, kOnX + 48, kTabY + 10);
    moveTo(win, kOnX + 48, kTabY + 40);
    CHECK(ui->get_drag_outs() == pulls0 + 1);
    CHECK_NEAR(ui->get_pull_dy(), 30, 0.01);
    CHECK_NEAR(ui->get_pull_dx(), 0, 0.01);
    moveTo(win, kOnX + 52, kTabY + 90);                                    // still pulling: no second drag-out
    moveTo(win, kOnX + 60, kTabY + 120);
    CHECK(ui->get_drag_outs() == pulls0 + 1);
    release(win, kOnX + 60, kTabY + 120);
    CHECK(ui->get_drag_outs() == pulls0 + 1);
    CHECK(ui->get_clicks() == 0);                                          // a tear-off is not a click
    // pull, change your mind, let go back over the tab: the tear-off fired, and the release is still no click
    moveTo(win, kOnX + 48, kTabY + 10);
    press(win, kOnX + 48, kTabY + 10);
    moveTo(win, kOnX + 48, kTabY + 40);
    CHECK(ui->get_drag_outs() == pulls0 + 2);
    moveTo(win, kOnX + 48, kTabY + 16);
    release(win, kOnX + 48, kTabY + 16);
    CHECK(ui->get_drag_outs() == pulls0 + 2);
    CHECK(ui->get_clicks() == 0);
    // a pull in small steps fires when the offset passes 24 (not at 20)
    moveTo(win, kOffX + 48, kTabY + 10);
    press(win, kOffX + 48, kTabY + 10);
    moveTo(win, kOffX + 51, kTabY + 20);
    moveTo(win, kOffX + 55, kTabY + 30);                                   // offset 20
    CHECK(ui->get_drag_outs() == pulls0 + 2);
    moveTo(win, kOffX + 58, kTabY + 36);                                   // offset 26
    CHECK(ui->get_drag_outs() == pulls0 + 3);
    CHECK_NEAR(ui->get_pull_dy(), 26, 0.01);
    CHECK_NEAR(ui->get_pull_dx(), 10, 0.01);                               // the offset carries the sideways travel too
    release(win, kOffX + 58, kTabY + 36);
    // exactly 24 px is not "more than 24"
    moveTo(win, kOnX + 48, kTabY + 10);
    press(win, kOnX + 48, kTabY + 10);
    moveTo(win, kOnX + 48, kTabY + 34);
    CHECK(ui->get_drag_outs() == pulls0 + 3);
    release(win, kOnX + 48, kTabY + 34);
    // upward, and sideways: never
    dragThrough(win, kOnX + 48, kTabY + 40, {{kOnX + 48, kTabY + 14}, {kOnX + 48, kTabY + 2}});
    CHECK(ui->get_drag_outs() == pulls0 + 3);
    dragThrough(win, kOnX + 10, kTabY + 10, {{kOnX + 60, kTabY + 14}, {kOnX + 90, kTabY + 20}});
    CHECK(ui->get_drag_outs() == pulls0 + 3);
    // a new press starts afresh: a further tear-off fires again
    dragThrough(win, kOnX + 48, kTabY + 10, {{kOnX + 48, kTabY + 45}});
    CHECK(ui->get_drag_outs() == pulls0 + 4);

    // --- hover: the tooltip opens under the tab after the delay -------------------------------------------------------
    {
        const int tipY = kTabY + kTabH + 6;
        moveTo(win, 700, 430);
        ms(20);
        Frame before = render(ui, kW, kH);
        CHECK(before.countInk(kOnX, tipY, kOnX + 100, tipY + 14, kBg) == 0);
        moveTo(win, kOnX + 48, kTabY + 16);
        Frame early = render(ui, kW, kH);
        CHECK(early.countInk(kOnX, tipY, kOnX + 100, tipY + 14, kBg) == 0);             // not before 600 ms
        ms(750);
        Frame shown = render(ui, kW, kH);
        CHECK(shown.countInk(kOnX, tipY, kOnX + 100, tipY + 14, kBg) > 200);            // the tip's ground and text
        CHECK(shown.near(kOnX + 3, tipY + 6, kMenu));                                   // the menu ground
        moveTo(win, 700, 430);
        ms(20);
        Frame gone = render(ui, kW, kH);
        CHECK(gone.countInk(kOnX, tipY, kOnX + 100, tipY + 14, kBg) == 0);
    }

    // ================================================================================ WindowChrome ==============
    CHECK_NEAR(ui->get_h_chrome(), 30, 0.01);
    {
        // ground: the menu colour, a 1 px line-strong rule at the bottom, 30 px in all
        CHECK(f.rowNear(kChromeY, kChromeX, kChromeX + kChromeW, kMenu));
        CHECK(f.rowNear(kChromeY + 28, kChromeX, kChromeX + kChromeW, kMenu));
        CHECK(f.rowNear(kChromeY + 29, kChromeX, kChromeX + kChromeW, kLineStrong));
        CHECK(f.rowNear(kChromeY + 30, kChromeX, kChromeX + kChromeW, kBg));
        CHECK(f.rowNear(kChromeY - 1, kChromeX, kChromeX + kChromeW, kBg));
        CHECK(f.near(kChromeX - 1, kChromeY + 10, kBg) && f.near(kChromeX + kChromeW, kChromeY + 10, kBg));
        // the title: 12 px in from the left, upper-case (no descender), in the foreground colour
        CHECK(f.countInk(kChromeX, kChromeY, kChromeX + 12, kChromeY + 29, kMenu) == 0);
        const Box title = inkBox(f, kChromeX + 8, kChromeY + 2, kChromeX + 150, kChromeY + 28, kMenu);
        CHECK(title.n > 50);
        CHECK(title.x0 >= kChromeX + 12 && title.x0 <= kChromeX + 15);
        CHECK(title.h() <= 9);                                             // "FRAME LOG": caps only, so no descender of the g
        CHECK(title.cy() >= kChromeY + 13 && title.cy() <= kChromeY + 16);
        CHECK(nearColour(brightest(f, title.x0, title.y0, title.x1 + 1, title.y1 + 1), kFg, 40));
        // the keys: 26 x 22, 4 px apart, 4 px from the right end: close, maximise, roll up
        const int rollX = kChromeX + kChromeW - 4 - 26 - 4 - 26 - 4 - 26;      // 226
        const int maxX = rollX + 30, closeX = maxX + 30;
        const Box roll = inkBox(f, rollX, kChromeY + 4, rollX + 26, kChromeY + 26, kMenu);
        const Box maxi = inkBox(f, maxX, kChromeY + 4, maxX + 26, kChromeY + 26, kMenu);
        const Box close = inkBox(f, closeX, kChromeY + 4, closeX + 26, kChromeY + 26, kMenu);
        CHECK(roll.n > 6 && maxi.n > 15 && close.n > 15);
        CHECK(roll.w() >= 8 && roll.h() <= 3);                             // a horizontal bar
        CHECK(maxi.w() >= 9 && maxi.w() <= 11 && maxi.h() >= 9 && maxi.h() <= 11);   // a square outline
        CHECK(f.near(static_cast<int>(maxi.cx()), static_cast<int>(maxi.cy()), kMenu, 8));   // ...hollow
        CHECK(close.w() >= 9 && close.w() <= 11 && close.h() >= 9 && close.h() <= 11);       // an X
        CHECK(!f.near(static_cast<int>(close.cx()), static_cast<int>(close.cy()), kMenu, 8)); // ...with ink at its middle
        CHECK(std::abs(maxi.cx() - roll.cx() - 30) <= 1);                  // 26 px keys + the 4 px gap
        CHECK(std::abs(close.cx() - maxi.cx() - 30) <= 1);
        CHECK(std::abs(close.cx() - (kChromeX + kChromeW - 4 - 13)) <= 1); // the last key ends 4 px from the right
        CHECK(std::abs(roll.cy() - (kChromeY + 15)) <= 4 && std::abs(close.cy() - (kChromeY + 15)) <= 1);
        CHECK(strength(brightest(f, rollX, kChromeY + 4, rollX + 26, kChromeY + 26), kFgSoft, kMenu) > 0.6);     // the icons are fg-soft
        CHECK(strength(brightest(f, maxX, kChromeY + 4, maxX + 26, kChromeY + 26), kFgSoft, kMenu) > 0.8);
        CHECK(strength(brightest(f, closeX, kChromeY + 4, closeX + 26, kChromeY + 26), kFgSoft, kMenu) > 0.8);
        CHECK(!nearColour(brightest(f, closeX, kChromeY + 4, closeX + 26, kChromeY + 26), kFg, 10));              // not the foreground white
        CHECK(f.countInk(rollX + 26, kChromeY + 2, maxX, kChromeY + 28, kMenu) == 0);      // the 4 px gaps are empty
        CHECK(f.countInk(maxX + 26, kChromeY + 2, closeX, kChromeY + 28, kMenu) == 0);
        CHECK(f.countInk(closeX + 26, kChromeY + 2, kChromeX + kChromeW, kChromeY + 28, kMenu) == 0);   // and so is the right padding
        CHECK(f.countInk(kChromeX + 150, kChromeY + 2, rollX, kChromeY + 28, kMenu) == 0); // nothing between the title and the keys

        // --- each key fires its own callback (and nothing else) -------------------------------------------------------
        CHECK(ui->get_roll_ups() == 0 && ui->get_maximises() == 0 && ui->get_closes() == 0);
        click(win, static_cast<float>(roll.cx()), kChromeY + 15);
        CHECK(ui->get_roll_ups() == 1 && ui->get_maximises() == 0 && ui->get_closes() == 0 && lastClick(*ui) == "chrome");
        click(win, static_cast<float>(maxi.cx()), kChromeY + 15);
        CHECK(ui->get_roll_ups() == 1 && ui->get_maximises() == 1 && ui->get_closes() == 0);
        click(win, static_cast<float>(close.cx()), kChromeY + 15);
        CHECK(ui->get_roll_ups() == 1 && ui->get_maximises() == 1 && ui->get_closes() == 1);
        click(win, rollX + 2, kChromeY + 5);                               // the key's corner is the key
        CHECK(ui->get_roll_ups() == 2);
        click(win, rollX + 28, kChromeY + 15);                             // the gap between two keys is neither
        CHECK(ui->get_roll_ups() == 2 && ui->get_maximises() == 1);

        // --- drag(dx, dy): the pointer's offset from the press, on a press-move on the strip ---------------------------
        CHECK_NEAR(ui->get_drag_dx(), 0, 0.01);
        ms(600);                                                           // clear any earlier click's double-click window
        const int dbl0 = ui->get_double_clicks();
        click(win, kChromeX + 120, kChromeY + 15);                         // a plain click on the strip moves nothing
        CHECK_NEAR(ui->get_drag_dx(), 0, 0.01);
        CHECK_NEAR(ui->get_drag_dy(), 0, 0.01);
        CHECK(ui->get_double_clicks() == dbl0);
        // a drag begun right after a click, at the same spot, is a drag and not the second half of a double click
        dragThrough(win, kChromeX + 120, kChromeY + 15, {{kChromeX + 140, kChromeY + 25}});
        CHECK_NEAR(ui->get_drag_dx(), 20, 0.01);
        CHECK_NEAR(ui->get_drag_dy(), 10, 0.01);
        CHECK(ui->get_double_clicks() == dbl0);
        ms(600);
        dragThrough(win, kChromeX + 120, kChromeY + 15, {{kChromeX + 112, kChromeY + 10}});
        CHECK_NEAR(ui->get_drag_dx(), 12, 0.01);                           // 20 - 8: dragging left is negative
        CHECK_NEAR(ui->get_drag_dy(), 5, 0.01);                            // 10 - 5
        // moves in steps: every call carries the offset from the PRESS (main.slint's title-drag idiom), so a host that moves
        // nothing sees it grow - 10, then 20, then 15 here - while one that moves the thing by each offset (the torn window,
        // the grip and the splitter below) sees only the new travel
        ui->set_drag_dx(0);
        ui->set_drag_dy(0);
        ms(600);
        moveTo(win, kChromeX + 120, kChromeY + 15);
        press(win, kChromeX + 120, kChromeY + 15);
        moveTo(win, kChromeX + 130, kChromeY + 18);
        CHECK_NEAR(ui->get_drag_dx(), 10, 0.01);
        CHECK_NEAR(ui->get_drag_dy(), 3, 0.01);
        moveTo(win, kChromeX + 140, kChromeY + 21);                        // 20 from the press: the call carries 20
        CHECK_NEAR(ui->get_drag_dx(), 30, 0.01);                           // 10 + 20
        CHECK_NEAR(ui->get_drag_dy(), 9, 0.01);                            // 3 + 6
        moveTo(win, kChromeX + 135, kChromeY + 15);                        // 15 and 0 from the press
        release(win, kChromeX + 135, kChromeY + 15);
        CHECK_NEAR(ui->get_drag_dx(), 45, 0.01);                           // 30 + 15
        CHECK_NEAR(ui->get_drag_dy(), 9, 0.01);                            // 9 + 0
        ui->set_drag_dx(12);
        ui->set_drag_dy(5);
        // pressing a key and moving does not drag the window
        ms(600);
        dragThrough(win, static_cast<int>(roll.cx()), kChromeY + 15, {{static_cast<int>(roll.cx()) + 40, kChromeY + 40}});
        CHECK_NEAR(ui->get_drag_dx(), 12, 0.01);
        // double-clicked: two quick clicks on the strip, not one, not on a key
        ms(600);
        click(win, kChromeX + 120, kChromeY + 15);
        CHECK(ui->get_double_clicks() == dbl0);
        click(win, kChromeX + 120, kChromeY + 15);
        CHECK(ui->get_double_clicks() == dbl0 + 1 && lastClick(*ui) == "chrome");
        ms(600);
        const int max0 = ui->get_maximises();
        click(win, static_cast<float>(maxi.cx()), kChromeY + 15);
        click(win, static_cast<float>(maxi.cx()), kChromeY + 15);
        CHECK(ui->get_double_clicks() == dbl0 + 1);                        // a double click on a key is two key clicks
        CHECK(ui->get_maximises() == max0 + 2);
    }

    // --- maximised: the middle key is Restore (a different, busier icon) --------------------------------------------------
    {
        const int maxX = kChromeX + kChromeW - 4 - 26 - 4 - 26;
        const Box restore = inkBox(f, maxX, kMaxY + 4, maxX + 26, kMaxY + 26, kMenu);
        const Box maximise = inkBox(f, maxX, kChromeY + 4, maxX + 26, kChromeY + 26, kMenu);
        CHECK(restore.n > maximise.n + 8);                                 // two overlapping squares draw more than one
        CHECK(restore.w() >= 9 && restore.w() <= 12 && restore.h() >= 9 && restore.h() <= 12);
        CHECK(f.rowNear(kMaxY + 29, kChromeX, kChromeX + kChromeW, kLineStrong));
        CHECK(f.rowNear(kMaxY, kChromeX, kChromeX + kChromeW, kMenu));
        const int max0 = ui->get_maximises();
        click(win, static_cast<float>(restore.cx()), kMaxY + 15);
        CHECK(lastClick(*ui) == "chrome-max");                             // the Restore key is the maximise callback
        CHECK(ui->get_maximises() == max0 + 1);
    }

    // --- rolled up: the title and one Restore key; no rule ----------------------------------------------------------------
    {
        const int keyX = kChromeX + kChromeW - 4 - 26;
        CHECK(f.rowNear(kRolledY, kChromeX, kChromeX + kChromeW, kMenu));
        CHECK(f.rowNear(kRolledY + 29, kChromeX, kChromeX + kChromeW, kMenu));   // border: 0
        CHECK(f.rowNear(kRolledY + 30, kChromeX, kChromeX + kChromeW, kBg));
        const Box restore = inkBox(f, keyX, kRolledY + 4, keyX + 26, kRolledY + 26, kMenu);
        CHECK(restore.n > 20 && restore.w() >= 9);
        CHECK(f.countInk(kChromeX + 150, kRolledY + 2, keyX - 4, kRolledY + 28, kMenu) == 0);   // no other keys
        CHECK(inkBox(f, kChromeX + 8, kRolledY + 2, kChromeX + 150, kRolledY + 28, kMenu).n > 50);   // the title
        const int rolls = ui->get_roll_ups(), maxes = ui->get_maximises();
        click(win, static_cast<float>(restore.cx()), kRolledY + 15);
        CHECK(ui->get_roll_ups() == rolls + 1 && lastClick(*ui) == "chrome-rolled");
        CHECK(ui->get_closes() == 1);
        // the keys that are gone do nothing
        click(win, keyX - 30, kRolledY + 15);
        click(win, keyX - 60, kRolledY + 15);
        CHECK(ui->get_roll_ups() == rolls + 1 && ui->get_maximises() == maxes && ui->get_closes() == 1);
    }

    // --- a title too long for the strip elides before the keys ---------------------------------------------------------------
    {
        const int y = kLongChromeY, w = kLongChromeW;
        const int keysLeft = kChromeX + w - 4 - 3 * 26 - 2 * 4;               // 142: where the first key starts
        CHECK(f.rowNear(y, kChromeX, kChromeX + w, kMenu));
        CHECK(f.rowNear(y + 29, kChromeX, kChromeX + w, kLineStrong));
        const Box title = inkBox(f, kChromeX + 8, y + 2, keysLeft - 4, y + 28, kMenu);
        CHECK(title.n > 150);                                                 // the title fills the room it has
        CHECK(title.x1 >= keysLeft - 4 - 14);                                 // ...right up to the limit
        CHECK(f.countInk(keysLeft - 3, y + 2, keysLeft, y + 28, kMenu) == 0); // and stops 4 px short of the first key
        CHECK(f.countInk(kChromeX + w - 4, y + 2, kChromeX + w, y + 28, kMenu) == 0);
    }

    // --- the chrome's keys: a hovered key lights its ground, and carries a tooltip -------------------------------------------
    {
        const int rollX = kChromeX + kChromeW - 4 - 26 - 4 - 26 - 4 - 26;
        const int maxX = rollX + 30, closeX = maxX + 30;
        moveTo(win, 700, 430);
        ms(20);
        Frame cold = render(ui, kW, kH);
        CHECK(cold.near(closeX + 2, kChromeY + 9, kMenu));
        moveTo(win, closeX + 13, kChromeY + 15);
        Frame hot = render(ui, kW, kH);
        CHECK(hot.near(closeX + 2, kChromeY + 9, kMenuHot));               // the hovered key: menu-hot
        CHECK(hot.near(maxX + 2, kChromeY + 9, kMenu));                    // the neighbour stays
        // tooltips: the tip opens 6 px under the key; "Maximise" is longer than "Close", "Restore" than "Close"
        auto tipWidth = [&](float x, float y, int keyLeft, int keyTop) {
            moveTo(win, 700, 430);
            ms(20);
            moveTo(win, x, y);
            ms(750);
            Frame fr = render(ui, kW, kH);
            const int top = keyTop + 22 + 6;
            const Box tip = colourBox(fr, keyLeft - 2, top, keyLeft + 140, top + 4, kMenu, 3);
            return tip.n > 0 ? tip.x1 - tip.x0 + 1 : 0;
        };
        const int wRoll = tipWidth(static_cast<float>(rollX + 13), kChromeY + 15, rollX, kChromeY + 4);
        const int wMax = tipWidth(static_cast<float>(maxX + 13), kChromeY + 15, maxX, kChromeY + 4);
        const int wClose = tipWidth(static_cast<float>(closeX + 13), kChromeY + 15, closeX, kChromeY + 4);
        CHECK(wRoll > 20 && wMax > 20 && wClose > 20);                     // a tip opened under each key
        CHECK(wMax > wClose);                                              // "Maximise" is wider than "Close"
        CHECK(wRoll > wClose);                                             // "Roll up" is wider than "Close"
        const int maxedX = kChromeX + kChromeW - 4 - 26 - 4 - 26;
        const int wRestore = tipWidth(static_cast<float>(maxedX + 13), kMaxY + 15, maxedX, kMaxY + 4);
        CHECK(wRestore > 20 && wRestore < wMax);                           // "Restore" where the normal chrome says "Maximise"
        // the strip itself carries the tear-off hint, 6 px under the chrome
        const int stripTip = kChromeY + 30 + 6;
        moveTo(win, 700, 430);
        ms(20);
        Frame bare = render(ui, kW, kH);
        CHECK(bare.countInk(kChromeX, stripTip, kChromeX + 120, stripTip + 3, kBg) == 0);
        moveTo(win, kChromeX + 150, kChromeY + 15);
        ms(750);
        Frame tip = render(ui, kW, kH);
        CHECK(tip.countInk(kChromeX, stripTip, kChromeX + 120, stripTip + 3, kBg, 8) > 300);
        CHECK(tip.near(kChromeX + 4, stripTip + 2, kMenu));
        moveTo(win, 700, 430);
        ms(20);
    }

    // ============================================================================== TornWindow ===================
    {
        CHECK_NEAR(ui->get_frame_h(), 200, 0.01);
        const int x0 = kWinX, y0 = kWinY, x1 = kWinX + kWinW - 1, y1 = kWinY + kWinH - 1;     // the frame's outer pixels
        Frame t = render(ui, kW, kH);
        // the border: 1 px knob-ring on every side; the window is `radius-panel` round
        CHECK(t.rowNear(y0, x0 + 9, x1 - 8, kKnobRing));
        CHECK(t.rowNear(y1, x0 + 9, x1 - 8, kKnobRing));
        CHECK(t.colNear(x0, y0 + 9, y1 - 8, kKnobRing));
        CHECK(t.colNear(x1, y0 + 9, y1 - 8, kKnobRing));
        CHECK(t.near(x0, y0, kBg, 4) && t.near(x1, y0, kBg, 4));           // the corner pixels show the window ground
        CHECK(t.near(x0, y1, kBg, 4) && t.near(x1, y1, kBg, 4));
        CHECK(t.near(x0 + 1, y0 + 1, kBg, 12));                            // one pixel in, the arc is still outside
        CHECK(!t.near(x0 + 5, y0 + 5, kBg, 12));                           // well inside the arc: window
        CHECK(t.rowNear(y0 - 1, x0, x1 + 1, kBg));                         // nothing outside the border
        CHECK(t.colNear(x0 - 1, y0, y1 + 1, kBg));
        CHECK(t.rowNear(y1 + 1, x0, x1 + 1, kBg));
        // the chrome inside the border: menu ground, a 1 px line-strong rule, then the content
        CHECK(t.rowNear(y0 + 1, x0 + 9, x1 - 8, kMenu));
        CHECK(t.rowNear(y0 + 29, x0 + 1, x1, kMenu));
        CHECK(t.rowNear(y0 + 30, x0 + 1, x1, kLineStrong));                // 1 (border) + 30 (chrome) - 1
        CHECK(t.rowNear(y0 + 31, x0 + 1, x1, kPanel));
        CHECK(t.near(x0 + 5, y0 + 32, kPanel));                            // the window ground below the chrome
        const Box title = inkBox(t, x0 + 6, y0 + 3, x0 + 160, y0 + 30, kMenu);
        CHECK(title.n > 50 && title.x0 >= x0 + 1 + 12 && title.x0 <= x0 + 1 + 15);
        CHECK(nearColour(brightest(t, title.x0, title.y0, title.x1 + 1, title.y1 + 1), kFg, 40));
        // the content (@children): the preset bar and the field, inside the 10 px padding, under the chrome
        CHECK(t.near(x0 + 30, y0 + 32 + 10 + 20, kCard));
        CHECK(t.near(x0 + 30, y0 + 32 + 10 + 40 + 8 + 40, kField));
        CHECK(t.near(x0 + 5, y0 + 32 + 10 + 20, kPanel));                  // the padding is plain window
        CHECK(t.near(x0 + 30, y1 - 5, kPanel));
        // the grip: a 135 degree hatch of nudge lines in the 12 px at right 3, bottom 3
        {
            const int gx0 = x1 + 1 - 3 - 12, gy0 = y1 + 1 - 3 - 12;
            const int hatch = t.countNear(gx0, gy0, gx0 + 12, gy0 + 12, kNudge, 12);
            CHECK(hatch >= 18 && hatch <= 60);                             // lines, not a fill
            CHECK(t.countNear(gx0 + 6, gy0 + 6, gx0 + 12, gy0 + 12, kNudge, 12) > 0);
            CHECK(t.countNear(gx0 - 6, gy0 - 6, gx0 - 1, gy0 + 20, kNudge, 12) == 0);       // nothing outside the 12 px
            CHECK(t.countNear(gx0, gy0 + 13, gx0 + 12, gy0 + 20, kNudge, 12) == 0);
            // a hatch: the pattern repeats along the diagonal, so a row has gaps
            int runs = 0;
            for (int y = gy0; y < gy0 + 12; ++y) {
                bool inRun = false;
                for (int x = gx0; x < gx0 + 12; ++x) {
                    const bool on = t.near(x, y, kNudge, 12);
                    if (on && !inRun) { ++runs; }
                    inRun = on;
                }
            }
            CHECK(runs >= 18);                                             // 4 diagonal bars crossing 12 rows
        }
        // the chrome's keys are the chrome's: 26 x 22 at the right end of the inner width
        const int rollX = x0 + 1 + 298 - 4 - 26 - 4 - 26 - 4 - 26;         // 569
        const int maxX = rollX + 30, closeX = maxX + 30;
        CHECK(inkBox(t, rollX, y0 + 1 + 4, rollX + 26, y0 + 1 + 26, kMenu).n > 6);
        CHECK(inkBox(t, maxX, y0 + 1 + 4, maxX + 26, y0 + 1 + 26, kMenu).n > 15);
        CHECK(inkBox(t, closeX, y0 + 1 + 4, closeX + 26, y0 + 1 + 26, kMenu).n > 15);
        CHECK(t.countInk(closeX + 26, y0 + 9, x1, y0 + 29, kMenu) == 0);      // the 4 px right padding is empty

        // --- the keys are forwarded -------------------------------------------------------------------------------------
        const int roll0 = ui->get_roll_ups(), max0 = ui->get_maximises(), close0 = ui->get_closes();
        click(win, rollX + 13, y0 + 1 + 15);
        CHECK(ui->get_roll_ups() == roll0 + 1 && lastClick(*ui) == "torn");
        click(win, maxX + 13, y0 + 1 + 15);
        CHECK(ui->get_maximises() == max0 + 1 && lastClick(*ui) == "torn");
        click(win, closeX + 13, y0 + 1 + 15);
        CHECK(ui->get_closes() == close0 + 1 && lastClick(*ui) == "torn");
        // a click in the content reaches nothing of the chrome
        click(win, x0 + 100, y0 + 32 + 60);
        CHECK(ui->get_roll_ups() == roll0 + 1 && ui->get_maximises() == max0 + 1 && ui->get_closes() == close0 + 1);

        // --- double-click on the title strip ------------------------------------------------------------------------------
        const int dbl0 = ui->get_double_clicks();
        ms(700);
        click(win, x0 + 200, y0 + 16);
        click(win, x0 + 200, y0 + 16);
        CHECK(ui->get_double_clicks() == dbl0 + 1);

        // --- maximised: the grip is gone and the middle key is Restore ---------------------------------------------------
        {
            const int gx0 = x1 + 1 - 3 - 12, gy0 = y1 + 1 - 3 - 12;
            const Box before = inkBox(t, maxX, y0 + 1 + 4, maxX + 26, y0 + 1 + 26, kMenu);
            ui->set_torn_maximised(true);
            Frame m = render(ui, kW, kH);
            CHECK(m.countNear(gx0, gy0, gx0 + 12, gy0 + 12, kNudge, 12) == 0);
            const Box after = inkBox(m, maxX, y0 + 1 + 4, maxX + 26, y0 + 1 + 26, kMenu);
            CHECK(after.n > before.n + 8);
            dragThrough(win, gx0 + 6, gy0 + 6, {{gx0 + 26, gy0 + 16}});
            CHECK_NEAR(ui->get_resize_dx(), 0, 0.01);                      // pressing where the grip was resizes nothing
            ui->set_torn_maximised(false);
            Frame back = render(ui, kW, kH);
            CHECK(back.countNear(gx0, gy0, gx0 + 12, gy0 + 12, kNudge, 12) >= 18);
        }

        // --- the window follows its chrome: drag(dx, dy) is the offset from the press; the window follows it ---
        CHECK_NEAR(ui->get_win_dx(), 0, 0.01);
        moveTo(win, x0 + 60, y0 + 16);
        press(win, x0 + 60, y0 + 16);
        moveTo(win, x0 + 70, y0 + 20);
        moveTo(win, x0 + 80, y0 + 24);
        moveTo(win, x0 + 90, y0 + 28);
        release(win, x0 + 90, y0 + 28);
        CHECK_NEAR(ui->get_win_dx(), 30, 0.01);
        CHECK_NEAR(ui->get_win_dy(), 12, 0.01);
        Frame moved = render(ui, kW, kH);
        CHECK(moved.rowNear(y0 + 12, x0 + 30 + 9, x1 + 30 - 8, kKnobRing));          // the frame has followed: +30, +12
        CHECK(moved.colNear(x0 + 30, y0 + 12 + 9, y1 + 12 - 8, kKnobRing));
        CHECK(moved.near(x0, y0 + 40, kBg));                                         // ...and left its old place
        // and back up and left, past the start: negative offsets
        moveTo(win, x0 + 30 + 60, y0 + 12 + 16);
        press(win, x0 + 30 + 60, y0 + 12 + 16);
        moveTo(win, x0 + 30 + 50, y0 + 12 + 6);
        moveTo(win, x0 + 30 + 20, y0 + 12 + 1);
        release(win, x0 + 30 + 20, y0 + 12 + 1);
        CHECK_NEAR(ui->get_win_dx(), -10, 0.01);                                      // 30 - 40
        CHECK_NEAR(ui->get_win_dy(), -3, 0.01);                                       // 12 - 15
        ui->set_win_dx(0);
        ui->set_win_dy(0);

        // --- resize(dx, dy): the grip, same semantics ---------------------------------------------------------------------
        const int gx = x1 + 1 - 3 - 6, gy = y1 + 1 - 3 - 6;                           // the grip's centre
        CHECK_NEAR(ui->get_resize_dx(), 0, 0.01);
        moveTo(win, static_cast<float>(gx), static_cast<float>(gy));
        press(win, static_cast<float>(gx), static_cast<float>(gy));
        moveTo(win, static_cast<float>(gx + 20), static_cast<float>(gy + 10));
        moveTo(win, static_cast<float>(gx + 10), static_cast<float>(gy + 7));
        release(win, static_cast<float>(gx + 10), static_cast<float>(gy + 7));
        CHECK_NEAR(ui->get_resize_dx(), 10, 0.01);                                    // 20 - 10: the pointer's net travel
        CHECK_NEAR(ui->get_resize_dy(), 7, 0.01);
        CHECK_NEAR(ui->get_frame_h(), 207, 0.01);
        Frame grown = render(ui, kW, kH);
        CHECK(grown.colNear(x1 + 10, y0 + 9, y1 + 7 - 8, kKnobRing));                 // the right border moved by 10
        CHECK(grown.rowNear(y1 + 7, x0 + 9, x1 + 10 - 8, kKnobRing));                 // the bottom border by 7
        CHECK(grown.near(x0 + 30, y1 + 7 - 12, kField));                              // ...and the field's bottom with it
        CHECK(grown.near(x1 + 10 - 12, y0 + 100, kField));                           // the content stretched with the window
        CHECK(grown.countNear(x1 + 10 - 14, y1 + 7 - 14, x1 + 10 - 2, y1 + 7 - 2, kNudge, 12) >= 18);   // the grip went with the corner
        CHECK(grown.countNear(gx - 6, gy - 6, gx + 6, gy + 6, kNudge, 12) == 0);                       // ...away from the old one
        // shrinking: a negative offset
        const int hx = x1 + 1 + 10 - 3 - 6, hy = y1 + 1 + 7 - 3 - 6;
        dragThrough(win, hx, hy, {{hx - 30, hy - 20}});
        CHECK_NEAR(ui->get_resize_dx(), -20, 0.01);
        CHECK_NEAR(ui->get_resize_dy(), -13, 0.01);
        ui->set_resize_dx(0);
        ui->set_resize_dy(0);
        Frame restored = render(ui, kW, kH);
        CHECK(restored.colNear(x1, y0 + 9, y1 - 8, kKnobRing));
    }

    // --- rolled up: the chrome alone, 30 px --------------------------------------------------------------------------------
    {
        CHECK_NEAR(ui->get_rolled_frame_h(), 30, 0.01);
        const int x0 = kWinX, x1 = kWinX + kWinW - 1, y0 = kRolledWinY;
        Frame r = render(ui, kW, kH);
        CHECK(r.rowNear(y0, x0 + 9, x1 - 8, kKnobRing));                              // the top border
        CHECK(r.rowNear(y0 + 29, x0 + 9, x1 - 8, kKnobRing));                         // the bottom border: the frame is 30 px
        CHECK(r.rowNear(y0 + 30, x0, x1 + 1, kBg));                                   // nothing below it
        CHECK(r.countInk(x0 - 20, y0 + 30, x1 + 20, kH, kBg) == 0);                   // not even the content of its 100 px
        CHECK(r.near(x0, y0, kBg, 4) && r.near(x1, y0, kBg, 4));                      // round corners
        CHECK(r.near(x0, y0 + 29, kBg, 4) && r.near(x1, y0 + 29, kBg, 4));            // ...at the bottom too
        CHECK(r.rowNear(y0 + 1, x0 + 9, x1 - 8, kMenu));                              // the chrome fills it
        CHECK(r.rowNear(y0 + 28, x0 + 9, x1 - 8, kMenu));                             // no rule: border 0
        CHECK(inkBox(r, x0 + 8, y0 + 3, x0 + 160, y0 + 27, kMenu).n > 50);            // the title
        const int keyX = x1 + 1 - 1 - 4 - 26;
        const Box restore = inkBox(r, keyX, y0 + 4, keyX + 26, y0 + 27, kMenu);
        CHECK(restore.n > 20);
        CHECK(r.countInk(x0 + 160, y0 + 3, keyX - 2, y0 + 27, kMenu) == 0);           // a single key
        const int rolls = ui->get_roll_ups();
        click(win, static_cast<float>(restore.cx()), y0 + 16);
        CHECK(ui->get_roll_ups() == rolls + 1 && lastClick(*ui) == "rolled");
        click(win, keyX + 23, y0 + 22);                                               // the key's lower right: no grip lies over it
        CHECK(ui->get_roll_ups() == rolls + 2);
        const float rdx = ui->get_resize_dx();
        dragThrough(win, x1 + 1 - 8, y0 + 22, {{x1 + 21, y0 + 40}});                  // where a grip would be: none, nothing resizes
        CHECK_NEAR(ui->get_resize_dx(), rdx, 0.01);
        click(win, x0 + 100, y0 + 60);                                                // where the hidden content would be
        CHECK(ui->get_roll_ups() == rolls + 2);
    }

    // ================================================================================== Splitter =====================
    {
        CHECK_NEAR(ui->get_h_split(), 6, 0.01);
        Frame s = render(ui, kW, kH);
        CHECK(s.rowNear(kSplitY, kSplitX, kSplitX + kSplitW, kLine));                  // border-top: 1 px line
        CHECK(s.rowNear(kSplitY + 5, kSplitX, kSplitX + kSplitW, kLine));              // border-bottom: the 6th pixel row
        CHECK(s.rowNear(kSplitY + 1, kSplitX, kSplitX + 190, kPanel));                 // the panel ground between
        CHECK(s.rowNear(kSplitY + 4, kSplitX, kSplitX + 190, kPanel));
        CHECK(s.rowNear(kSplitY - 1, kSplitX, kSplitX + kSplitW, kBg));
        CHECK(s.rowNear(kSplitY + 6, kSplitX, kSplitX + kSplitW, kBg));                // 6 px in all
        CHECK(s.near(kSplitX - 1, kSplitY + 2, kBg) && s.near(kSplitX + kSplitW, kSplitY + 2, kBg));
        // the grip: 48 x 2, knob-ring, centred
        const int gx0 = kSplitX + (kSplitW - 48) / 2;                                  // 192
        CHECK(s.rowNear(kSplitY + 2, gx0 + 2, gx0 + 46, kKnobRing));
        CHECK(s.rowNear(kSplitY + 3, gx0 + 2, gx0 + 46, kKnobRing));
        CHECK(s.rowNear(kSplitY + 1, gx0, gx0 + 48, kPanel));                          // 2 px tall, no more
        CHECK(s.rowNear(kSplitY + 4, gx0, gx0 + 48, kPanel));
        CHECK(s.near(gx0 - 2, kSplitY + 2, kPanel) && s.near(gx0 + 49, kSplitY + 2, kPanel));
        const Box grip = inkBox(s, kSplitX, kSplitY + 1, kSplitX + kSplitW, kSplitY + 5, kPanel, 6);
        CHECK(grip.w() == 48 && grip.h() == 2);                                        // 48 x 2, round ends included
        CHECK(grip.x0 == gx0 && grip.y0 == kSplitY + 2);
        CHECK(colourBox(s, kSplitX, kSplitY + 1, kSplitX + kSplitW, kSplitY + 5, kKnobRing, 6).n >= 90);   // in knob-ring


        // hover and press light the grip in the accent
        moveTo(win, 700, 430);
        Frame cold = render(ui, kW, kH);
        CHECK(cold.rowNear(kSplitY + 2, gx0 + 2, gx0 + 46, kKnobRing));
        moveTo(win, kSplitX + 40, kSplitY + 3);                                        // anywhere on the bar, not only on the grip
        Frame hot = render(ui, kW, kH);
        CHECK(hot.rowNear(kSplitY + 2, gx0 + 2, gx0 + 46, kAccent));
        CHECK(hot.rowNear(kSplitY + 3, gx0 + 2, gx0 + 46, kAccent));
        CHECK(hot.rowNear(kSplitY, kSplitX, kSplitX + kSplitW, kLine));                // the hairlines do not change
        moveTo(win, 700, 430);
        Frame cold2 = render(ui, kW, kH);
        CHECK(cold2.rowNear(kSplitY + 2, gx0 + 2, gx0 + 46, kKnobRing));

        // moved(dy): the offset from the press; the bar follows it, so the calls sum to the pointer's travel
        CHECK_NEAR(ui->get_splitter_dy(), 0, 0.01);
        moveTo(win, 216, kSplitY + 3);
        press(win, 216, kSplitY + 3);
        moveTo(win, 216, kSplitY + 11);                                                // 8 px DOWN: positive
        CHECK_NEAR(ui->get_splitter_dy(), 8, 0.01);
        Frame dragging = render(ui, kW, kH);
        CHECK(dragging.rowNear(kSplitY + 8, kSplitX, kSplitX + kSplitW, kLine));        // the bar has followed
        CHECK(dragging.rowNear(kSplitY + 8 + 2, gx0 + 2, gx0 + 46, kAccent));           // lit while pulled
        moveTo(win, 400, kSplitY + 6);                                                 // away from the bar, still pulling
        CHECK_NEAR(ui->get_splitter_dy(), 3, 0.01);                                    // net travel: 6 - 3
        Frame away = render(ui, kW, kH);
        CHECK(away.rowNear(kSplitY + 3 + 2, gx0 + 2, gx0 + 46, kAccent));               // still lit: it is pressed
        release(win, 400, kSplitY + 6);
        Frame released = render(ui, kW, kH);
        CHECK(released.rowNear(kSplitY + 3 + 2, gx0 + 2, gx0 + 46, kAccent));           // released over the bar: still hovered
        moveTo(win, 700, 430);
        Frame left = render(ui, kW, kH);
        CHECK(left.rowNear(kSplitY + 3 + 2, gx0 + 2, gx0 + 46, kKnobRing));             // neither hovered nor pressed
        // up: negative
        moveTo(win, 216, kSplitY + 3 + 3);
        press(win, 216, kSplitY + 3 + 3);
        moveTo(win, 216, kSplitY + 3 + 3 - 10);
        CHECK_NEAR(ui->get_splitter_dy(), -7, 0.01);                                   // 3 - 10
        release(win, 216, kSplitY + 3 + 3 - 10);
        // a click without moving reports nothing
        const float dy = ui->get_splitter_dy();
        click(win, 216, kSplitY - 7 + 3);
        CHECK_NEAR(ui->get_splitter_dy(), dy, 0.01);
        ui->set_splitter_dy(0);
    }

    // ============================================== only the left button clicks, tears off, drags and resizes =========
    {
        using slint::PointerEventButton;
        const auto rightDrag = [&](int x, int y, int tx, int ty) {
            moveTo(win, static_cast<float>(x), static_cast<float>(y));
            press(win, static_cast<float>(x), static_cast<float>(y), PointerEventButton::Right);
            moveTo(win, static_cast<float>(tx), static_cast<float>(ty));
            release(win, static_cast<float>(tx), static_cast<float>(ty), PointerEventButton::Right);
        };
        const int clicks = ui->get_clicks(), pulls = ui->get_drag_outs(), rolls = ui->get_roll_ups();
        const float dx = ui->get_drag_dx(), wdx = ui->get_win_dx(), rdx = ui->get_resize_dx(), sdy = ui->get_splitter_dy();
        click(win, kOnX + 48, kTabY + 16, PointerEventButton::Right);
        rightDrag(kOnX + 48, kTabY + 10, kOnX + 48, kTabY + 60);                    // a right-button pull is no tear-off
        click(win, kChromeX + 120, kChromeY + 15, PointerEventButton::Right);
        rightDrag(kChromeX + 120, kChromeY + 15, kChromeX + 160, kChromeY + 40);    // ...no window drag
        const int rollX = kChromeX + kChromeW - 4 - 26 - 4 - 26 - 4 - 26;
        click(win, rollX + 13, kChromeY + 15, PointerEventButton::Right);           // ...no key press
        rightDrag(kWinX + kWinW - 9, kWinY + kWinH - 9, kWinX + kWinW + 20, kWinY + kWinH + 20);   // ...no resize
        rightDrag(216, kSplitY + 3, 216, kSplitY + 40);                             // ...no splitter move
        click(win, kOnX + 48, kTabY + 16, PointerEventButton::Middle);
        CHECK(ui->get_clicks() == clicks && ui->get_drag_outs() == pulls && ui->get_roll_ups() == rolls);
        CHECK_NEAR(ui->get_drag_dx(), dx, 0.01);
        CHECK_NEAR(ui->get_win_dx(), wdx, 0.01);
        CHECK_NEAR(ui->get_resize_dx(), rdx, 0.01);
        CHECK_NEAR(ui->get_splitter_dy(), sdy, 0.01);
    }

    // ============================================================ a disabled component never acts ====================
    {
        ui->set_locked(true);
        Frame d = render(ui, kW, kH);
        // looks: the off tab's label is the disabled colour, not muted
        CHECK(nearColour(brightest(d, kOffX, kTabY + 2, kOffX + offW, kTabY + kTabH - 3), kDisabled, 40));
        CHECK(!nearColour(brightest(d, kOffX, kTabY + 2, kOffX + offW, kTabY + kTabH - 3), kMuted, 12));
        const int clicks = ui->get_clicks(), pulls = ui->get_drag_outs(), closes = ui->get_close_clicks();
        click(win, kOffX + 48, kTabY + 16);
        dragThrough(win, kOffX + 48, kTabY + 10, {{kOffX + 48, kTabY + 60}});
        click(win, kCloseX + closeW - 24, kTabY + 14);
        CHECK(ui->get_clicks() == clicks && ui->get_drag_outs() == pulls && ui->get_close_clicks() == closes);
        const int rollX = kChromeX + kChromeW - 4 - 26 - 4 - 26 - 4 - 26;
        const int roll0 = ui->get_roll_ups(), max0 = ui->get_maximises(), close0 = ui->get_closes(), dbl0 = ui->get_double_clicks();
        const float dx0 = ui->get_drag_dx();
        click(win, rollX + 13, kChromeY + 15);
        click(win, rollX + 43, kChromeY + 15);
        click(win, rollX + 73, kChromeY + 15);
        dragThrough(win, kChromeX + 120, kChromeY + 15, {{kChromeX + 150, kChromeY + 25}});
        ms(700);
        click(win, kChromeX + 120, kChromeY + 15);
        click(win, kChromeX + 120, kChromeY + 15);
        CHECK(ui->get_roll_ups() == roll0 && ui->get_maximises() == max0 && ui->get_closes() == close0);
        CHECK(ui->get_double_clicks() == dbl0);
        CHECK_NEAR(ui->get_drag_dx(), dx0, 0.01);
        // the torn window: no drag, no resize, no keys
        const float wdx = ui->get_win_dx(), rdx = ui->get_resize_dx();
        dragThrough(win, kWinX + 60, kWinY + 16, {{kWinX + 100, kWinY + 36}});
        dragThrough(win, kWinX + kWinW - 10, kWinY + kWinH - 10, {{kWinX + kWinW + 20, kWinY + kWinH + 20}});
        click(win, kWinX + 1 + 298 - 4 - 13, kWinY + 16);
        CHECK_NEAR(ui->get_win_dx(), wdx, 0.01);
        CHECK_NEAR(ui->get_resize_dx(), rdx, 0.01);
        CHECK(ui->get_closes() == close0);
        // the splitter does not move
        dragThrough(win, 216, kSplitY + 3, {{216, kSplitY + 30}});
        CHECK_NEAR(ui->get_splitter_dy(), 0, 0.01);
        moveTo(win, kSplitX + 40, kSplitY + 3);
        Frame d3 = render(ui, kW, kH);
        CHECK(d3.rowNear(kSplitY + 2, kSplitX + 178, kSplitX + 222, kKnobRing));        // a disabled splitter does not light
        ui->set_locked(false);
        moveTo(win, 700, 430);
    }

    return testSummary("test_slint_tabs");
}
