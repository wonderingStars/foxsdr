// The Output family (ui/output.slint): the decoder log (LogView), the picture pane (PicturePane) and the target details (TargetDetails)
// of the plugin kit, rendered headlessly from ui/specimens/output_specimen.slint and probed against Workbench-PluginKit.dc.html
// ("Decoder output tab / window", "Target details drawer") and the ImGui window's behaviour for wording and states
// (src/gui/app_window.cpp: the picture window, Target details; docs: decode-bank-plugin-host.txt).
//
// The sheet (1280 x 640): LogView A (10, 10) 520 x 150 with six lines; LogView B (10, 170) 520 x 70 empty; LogView C (10, 250) 520 x 150
// filled by the test; PicturePane A (550, 10) 360 x 230, B (550, 250) 360 x 120, C (550, 380) 360 x 70; TargetDetails A..D at x 930, 340
// wide, E (10, 420) unframed, F (370, 470) 296 wide with its keys in a row. The numbers below are the artboard's: a log bar of
// 5 + 20 + 5 + the 1 px rule = 31; a log line of 11 px mono text 17 px apart (the sheet's 3 px gap); padding 6 / 10; a target panel of
// 1 + 8 + 24 + 4 ... + 8 + 1 with a 120 px column of small keys.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "render_harness.hpp"
#include "test_check.hpp"

#include "output_specimen.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace foxsdr::slinttest;

namespace {

Rgb tok(slint::Color c) { return {c.red(), c.green(), c.blue()}; }

// Pixels of the rectangle that differ at all between two frames.
int differing(const Frame& a, const Frame& b, int x0, int y0, int x1, int y1) {
    int n = 0;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const Rgb p = a.at(x, y);
            const Rgb q = b.at(x, y);
            if (p.r != q.r || p.g != q.g || p.b != q.b) { ++n; }
        }
    }
    return n;
}

// The brightest pixel of a rectangle (the largest channel sum): light text on a dark ground is anti-aliased into blends, so the
// pixel a stroke covers fully is the one that reads the text's own colour (a count of "pixels near the colour" would depend on how
// heavy the face is).
Rgb brightest(const Frame& f, int x0, int y0, int x1, int y1) {
    Rgb best{};
    int sum = -1;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const Rgb p = f.at(x, y);
            const int s = int(p.r) + int(p.g) + int(p.b);
            if (s > sum) { sum = s; best = p; }
        }
    }
    return best;
}

int firstInkX(const Frame& f, int x0, int y0, int x1, int y1, Rgb bg) {
    for (int x = x0; x < x1; ++x) { if (f.countInk(x, y0, x + 1, y1, bg) > 0) { return x; } }
    return -1;
}
int lastInkX(const Frame& f, int x0, int y0, int x1, int y1, Rgb bg) {
    for (int x = x1 - 1; x >= x0; --x) { if (f.countInk(x, y0, x + 1, y1, bg) > 0) { return x; } }
    return -1;
}
int firstInkY(const Frame& f, int x0, int y0, int x1, int y1, Rgb bg) {
    for (int y = y0; y < y1; ++y) { if (f.countInk(x0, y, x1, y + 1, bg) > 0) { return y; } }
    return -1;
}
int lastInkY(const Frame& f, int x0, int y0, int x1, int y1, Rgb bg) {
    for (int y = y1 - 1; y >= y0; --y) { if (f.countInk(x0, y, x1, y + 1, bg) > 0) { return y; } }
    return -1;
}

// The first x in [x0, x1) of a run of four pixels all in `ground`: the left of a key, found without knowing how wide its words are.
int firstRun(const Frame& f, int y, int x0, int x1, Rgb ground) {
    for (int x = x0; x + 3 < x1; ++x) {
        if (f.near(x, y, ground, 0) && f.near(x + 1, y, ground, 0) && f.near(x + 2, y, ground, 0) && f.near(x + 3, y, ground, 0)) { return x; }
    }
    return -1;
}

// The last x in [x0, x1) whose pixel on row y is `colour`: the right border of a key, found on its middle row; -1 when none.
int lastOn(const Frame& f, int y, int x0, int x1, Rgb colour, int tol = 0) {
    for (int x = x1 - 1; x >= x0; --x) { if (f.near(x, y, colour, tol)) { return x; } }
    return -1;
}

// The first x in [x0, x1) that starts a run of at least `len` pixels of `colour` on row y (a key's top border, whatever its words):
// text anti-aliasing never draws a straight run of one colour; -1 when none.
int firstLongRun(const Frame& f, int y, int x0, int x1, Rgb colour, int len, int tol = 0) {
    int run = 0;
    for (int x = x0; x < x1; ++x) {
        run = f.near(x, y, colour, tol) ? run + 1 : 0;
        if (run >= len) { return x - len + 1; }
    }
    return -1;
}

// Whether any row of [y0, y1) holds a run of four pixels of `ground` in [x0, x1): a key (its ground) somewhere in the rectangle.
bool anyRun(const Frame& f, int x0, int y0, int x1, int y1, Rgb ground) {
    for (int y = y0; y < y1; ++y) { if (firstRun(f, y, x0, x1, ground) >= 0) { return true; } }
    return false;
}

// The start of each run of at least `len` pixels of `colour` along row y in [x0, x1): the keys of a row, left to right.
std::vector<int> runStarts(const Frame& f, int y, int x0, int x1, Rgb colour, int len, int tol = 0) {
    std::vector<int> out;
    int run = 0;
    for (int x = x0; x < x1; ++x) {
        if (f.near(x, y, colour, tol)) {
            ++run;
            if (run == len) { out.push_back(x - len + 1); }
        } else {
            run = 0;
        }
    }
    return out;
}

// Rows of [y0, y1) that are `want` across the whole of [x0, x1): the hairlines.
int rulesIn(const Frame& f, int x0, int x1, int y0, int y1, Rgb want, int tol = 2) {
    int n = 0;
    for (int y = y0; y < y1; ++y) { if (f.rowNear(y, x0, x1, want, tol)) { ++n; } }
    return n;
}

LogLine line(const char* plugin, const std::string& text, LineTone tone) {
    return LogLine{slint::SharedString(plugin), slint::SharedString(text), tone};
}

// A picture of w x h pixels: the left half a red, the right half a blue, neither a colour of the theme.
slint::Image makePicture(int w, int h) {
    slint::SharedPixelBuffer<slint::Rgba8Pixel> buf(static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h));
    slint::Rgba8Pixel* px = buf.begin();
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            px[y * w + x] = x < w / 2 ? slint::Rgba8Pixel{0xc0, 0x30, 0x30, 0xff} : slint::Rgba8Pixel{0x30, 0x60, 0xc0, 0xff};
        }
    }
    return slint::Image(buf);
}

constexpr int kW = 1280, kH = 640;
constexpr int kLx = 10, kLw = 520;                                  // the three logs
constexpr int kAy = 10, kAh = 150, kBy = 170, kBh = 70, kCy = 250, kCh = 150;
constexpr int kBarH = 31, kPitch = 17, kPad = 6, kTextX = kLx + 10;    // bar 31 (rule on its last row), lines 17 apart, 6 px above the first, text 10 in
constexpr double kGlyph = 6.6;                                       // JetBrains Mono at 11 px: 0.6 em
constexpr int kPx = 550, kPw = 360;                                  // the picture panes
constexpr int kPay = 10, kPah = 230, kPby = 250, kPbh = 120, kPcy = 380, kPch = 70;
constexpr int kTx = 930, kTw = 340;                                  // the target details
constexpr int kKeysW = 120;

int lineTop(int base, int i) { return base + kBarH + kPad + kPitch * i; }
int afterText(int chars) { return kTextX + static_cast<int>((chars + 1) * kGlyph + 0.5); }   // where the text begins after "<name> "

}  // namespace

int main() {
    installHeadlessPlatform();
    auto ui = OutputSpecimen::create();
    slint::Window& win = ui->window();

    // The tokens in force, read from the Theme itself: everything is asserted against whatever filling is installed.
    const auto& th = ui->global<Theme>();
    const Rgb bg = tok(th.get_bg());
    const Rgb panel = tok(th.get_panel());
    const Rgb field = tok(th.get_field());
    const Rgb line1 = tok(th.get_line());
    const Rgb lineStrong = tok(th.get_line_strong());
    const Rgb knobRing = tok(th.get_knob_ring());
    const Rgb fg = tok(th.get_fg());
    const Rgb dim = tok(th.get_dim());
    const Rgb accent = tok(th.get_accent());
    const Rgb warnFg = tok(th.get_chip_warn_fg());
    const Rgb errFg = tok(th.get_chip_err_fg());
    const Rgb keyBg = tok(th.get_key_bg());
    const Rgb keyLine = tok(th.get_key_line());
    const Rgb keyFg = tok(th.get_key_fg());
    const Rgb keyOffBg = tok(th.get_key_disabled_bg());
    const Rgb keyOffFg = tok(th.get_key_disabled_fg());
    const Rgb chipBg = tok(th.get_chip_bg());
    const Rgb chipGoodBg = tok(th.get_chip_good_bg());
    const Rgb chipWarnBg = tok(th.get_chip_warn_bg());
    const Rgb lampGreen = tok(th.get_lamp_green());
    const Rgb lampAmber = tok(th.get_lamp_amber());
    const Rgb lampOff = tok(th.get_lamp_off());
    const Rgb offBorder = over(keyLine, 0.5, panel);                       // a disabled key's border: Theme.key-line at half alpha over the panel
    const Rgb red = rgb(0xc03030);
    const Rgb blue = rgb(0x3060c0);
    // The artboard's own values, once, so a drift of the Workbench filling is caught here too.
    CHECK(nearColour(field, rgb(0x0f1114), 0) && nearColour(line1, rgb(0x2b2f37), 0) && nearColour(panel, rgb(0x1d2026), 0));
    CHECK(nearColour(warnFg, rgb(0xf5c66b), 0) && nearColour(errFg, rgb(0xff9b9b), 0) && nearColour(keyBg, rgb(0x262a32), 0) && nearColour(dim, rgb(0x7d8694), 0));

    const auto away = [&] { moveTo(win, 1276.0f, 556.0f); };               // outside every variant: nothing hovered
    const auto shot = [&] { return render(ui, kW, kH); };
    const bool trace = std::getenv("FOXSDR_TEST_TRACE") != nullptr;           // FOXSDR_TEST_TRACE=1 names each section on stderr as it starts
    const auto section = [&](const char* name) { if (trace) { std::fprintf(stderr, "@@ %s\n", name); } };
    // Frames that have stopped moving in a rectangle (a scroll glides, the follow corrects itself a tick after it jumps).
    const auto calm = [&](int x0, int y0, int x1, int y1) {
        const auto start = std::chrono::steady_clock::now();
        Frame prev = shot();
        int still = 0;
        while (std::chrono::steady_clock::now() - start < std::chrono::seconds(4)) {
            settle(std::chrono::milliseconds(40));
            Frame cur = shot();
            still = differing(prev, cur, x0, y0, x1, y1) == 0 ? still + 1 : 0;
            prev = std::move(cur);
            if (still >= 3) { break; }
        }
    };
    // What a tone looks like in a region: the brightest pixel is the text's own colour (some stroke covers a pixel fully).
    const auto inkIs = [&](const Frame& f, int x0, int y0, int x1, int y1, Rgb want, int tol = 14) { return nearColour(brightest(f, x0, y0, x1, y1), want, tol); };

    ui->set_picture(makePicture(200, 100));
    away();
    const Frame f0 = shot();
    CHECK(f0.width == kW && f0.height == kH);
    CHECK(f0.savePng(shotDir() + "output-specimen.png"));

    // ============================================================================================================================
    // LogView A: the bar and the body
    // ============================================================================================================================
    {
        section("LogView A static");
        const int top = kAy, bodyTop = kAy + kBarH;                                      // the bar is y 10..40, its rule on y 40, the body from y 41
        // --- geometry: 520 wide, 150 high; the bar over the window panel, a 1 px Theme.line rule on its last row, then Theme.field
        CHECK(f0.near(kLx, bodyTop + 10, field, 0) && f0.near(kLx + kLw - 1, bodyTop + 10, field, 0) && f0.near(kLx + kLw, bodyTop + 10, bg, 0));
        CHECK(f0.near(kLx - 1, bodyTop + 10, bg, 0));
        CHECK(f0.rowNear(bodyTop - 1, kLx, kLx + kLw, line1, 0));                        // y 40
        CHECK(f0.near(kLx + 300, bodyTop - 2, panel, 0) && f0.near(kLx + 300, top + 1, panel, 0));   // the bar itself has no ground of its own
        CHECK(f0.near(kLx + 300, bodyTop, field, 0));                                    // y 41: the body starts right under the rule
        CHECK(f0.near(kLx + 300, kAy + kAh - 1, field, 0) && f0.near(kLx + 300, kAy + kAh, bg, 0));   // and runs to the foot, y 159
        CHECK(f0.near(kLx + 4, bodyTop + 4, field, 0));                                  // the 6 px of padding above the first line is the body's

        // --- the Follow checkbox: a 14 px box at x 20, centred in the bar (y 18..31); on = the accent ground
        const int boxX = kLx + 10, boxY = top + 8;
        CHECK(f0.countNear(boxX, boxY, boxX + 14, boxY + 14, accent, 6) >= 100);         // follow is on: an accent box with a tick
        CHECK(f0.near(boxX + 2, boxY + 2, accent, 6) && f0.near(boxX - 1, boxY + 6, panel, 0) && f0.near(boxX + 14, boxY + 6, panel, 0));
        CHECK(f0.near(boxX + 6, boxY - 1, panel, 0) && f0.near(boxX + 6, boxY + 14, panel, 0));          // exactly 14 x 14
        // the word "Follow": 11 px in Theme.fg, 8 px after the box
        CHECK(f0.countInk(boxX + 22, top + 6, boxX + 22 + 50, top + 24, panel) > 40);
        CHECK(inkIs(f0, boxX + 22, top + 6, boxX + 22 + 34, top + 24, fg));
        CHECK(f0.countInk(boxX + 15, top + 6, boxX + 21, top + 24, panel) == 0);         // the 8 px gap: nothing between the box and the word

        // --- the Clear key: an xs key, 20 px high, centred in the bar (y 15..34), after the checkbox row
        const int clearX = firstRun(f0, top + 15, boxX + 40, kLx + kLw / 2, keyBg);
        CHECK(clearX > boxX + 40 && clearX < boxX + 140);
        CHECK(f0.near(clearX + 6, top + 5, keyLine, 0) && f0.near(clearX + 6, top + 4, panel, 0));       // top border on y 15, nothing above
        CHECK(f0.near(clearX + 6, top + 24, keyLine, 0) && f0.near(clearX + 6, top + 25, panel, 0));     // bottom border on y 34: 20 high
        CHECK(f0.colNear(clearX + 6, top + 6, top + 24, keyBg, 0));                      // Theme.key-bg between
        CHECK(f0.near(clearX - 1, top + 15, keyLine, 0) && f0.near(clearX - 2, top + 15, panel, 0));       // its left border
        CHECK(f0.countInk(clearX + 8, top + 7, clearX + 40, top + 23, keyBg) > 20 && inkIs(f0, clearX + 8, top + 7, clearX + 40, top + 23, keyFg));   // "Clear"
        // the gap between the checkbox row and the key is 8 px: the label's right edge is where the checkbox ends
        const int followEnd = lastInkX(f0, boxX + 22, top + 6, clearX - 4, top + 24, panel);
        CHECK(followEnd > 0 && clearX - 1 - followEnd >= 8 && clearX - 1 - followEnd <= 16);

        // --- the count, right-aligned 10 px from the edge, mono 11 px in Theme.dim
        CHECK(f0.countInk(kLx + kLw - 110, top + 6, kLx + kLw, top + 25, panel) > 40);
        CHECK(inkIs(f0, kLx + kLw - 110, top + 6, kLx + kLw, top + 25, dim));
        const int countRight = lastInkX(f0, kLx + kLw - 110, top + 6, kLx + kLw, top + 25, panel);
        CHECK(countRight >= kLx + kLw - 10 - 3 && countRight <= kLx + kLw - 10);
        CHECK(f0.countNear(kLx + kLw - 110, top + 6, kLx + kLw, top + 25, fg, 14) == 0);   // dim, not the text colour
        // centred in the bar: ink of the count sits in the middle of y 15..34
        CHECK(firstInkY(f0, kLx + kLw - 110, top + 1, kLx + kLw, top + 30, panel) >= top + 9 && lastInkY(f0, kLx + kLw - 110, top + 1, kLx + kLw, top + 30, panel) <= top + 21);

        // --- the lines: mono, 17 px apart from y 47, each its own tone; the plugin name is always dim
        const int nameEnd0 = kTextX + static_cast<int>(12 * kGlyph);                    // "aprs-decoder" is 12 characters
        const int t0 = lineTop(top, 0);
        CHECK(t0 == 47 && lineTop(top, 5) == 132);
        for (int i = 0; i < 6; ++i) {
            CHECK(f0.countInk(kTextX, lineTop(top, i), kLx + kLw - 20, lineTop(top, i) + kPitch, field) > 30);   // every line has ink
        }
        // line 0, normal: the name dim, the text Theme.fg
        CHECK(inkIs(f0, kTextX, t0, nameEnd0 - 4, t0 + kPitch, dim));
        CHECK(inkIs(f0, afterText(12) + 4, t0, kLx + kLw - 20, t0 + kPitch, fg));
        CHECK(f0.countNear(kTextX, t0, nameEnd0 - 4, t0 + kPitch, fg, 14) == 0);
        // line 1, warn: the name dim, the text Theme.chip-warn-fg
        const int t1 = lineTop(top, 1);
        CHECK(inkIs(f0, kTextX, t1, nameEnd0 - 4, t1 + kPitch, dim));
        CHECK(inkIs(f0, afterText(12) + 4, t1, kLx + kLw - 20, t1 + kPitch, warnFg));
        CHECK(f0.countNear(kTextX, t1, nameEnd0 - 4, t1 + kPitch, warnFg, 14) == 0);   // the name is not tinted
        // line 2, err: "pocsag" is 6 characters
        const int t2 = lineTop(top, 2);
        CHECK(inkIs(f0, kTextX, t2, kTextX + static_cast<int>(6 * kGlyph) - 4, t2 + kPitch, dim));
        CHECK(inkIs(f0, afterText(6) + 4, t2, kLx + kLw - 20, t2 + kPitch, errFg));
        CHECK(f0.countNear(afterText(6) + 4, t2, kLx + kLw - 20, t2 + kPitch, warnFg, 14) == 0);
        // lines 3 and 4, normal; line 5, muted and with no plugin name: the whole line dim, starting at the left padding
        CHECK(inkIs(f0, afterText(12) + 4, lineTop(top, 3), kLx + kLw - 20, lineTop(top, 3) + kPitch, fg));
        CHECK(inkIs(f0, afterText(12) + 4, lineTop(top, 4), kLx + kLw - 20, lineTop(top, 4) + kPitch, fg));
        CHECK(inkIs(f0, kTextX, lineTop(top, 5), kLx + kLw - 20, lineTop(top, 5) + kPitch, dim));
        CHECK(f0.countNear(kTextX, lineTop(top, 5), kLx + kLw - 20, lineTop(top, 5) + kPitch, fg, 14) == 0);
        const int mutedX = firstInkX(f0, kLx + 1, lineTop(top, 5), kLx + 60, lineTop(top, 5) + kPitch, field);
        CHECK(mutedX >= kTextX && mutedX <= kTextX + 3);                                // the text starts AT the padding when there is no name
        // the pitch: lines 1 and 0 carry the same name, so the same glyphs, and their first ink rows are exactly 17 apart
        const int y0 = firstInkY(f0, kTextX, t0, nameEnd0, t0 + kPitch, field);
        const int y1 = firstInkY(f0, kTextX, t1, nameEnd0, t1 + kPitch, field);
        CHECK(y0 >= t0 && y0 <= t0 + 6 && y1 - y0 == kPitch);
        const int y3 = firstInkY(f0, kTextX, lineTop(top, 3), nameEnd0, lineTop(top, 3) + kPitch, field);
        const int y4 = firstInkY(f0, kTextX, lineTop(top, 4), nameEnd0, lineTop(top, 4) + kPitch, field);
        CHECK(y3 >= lineTop(top, 3) && y4 - y3 == kPitch);
        // the left padding: the first ink of a line is 10 px in
        const int nameX = firstInkX(f0, kLx + 1, t0, kLx + 60, t0 + kPitch, field);
        CHECK(nameX >= kTextX && nameX <= kTextX + 3);
        // one space between the name and the text: the text begins about one mono advance after the name
        const int textX0 = firstInkX(f0, nameEnd0 + 1, t0, nameEnd0 + 30, t0 + kPitch, field);
        CHECK(textX0 >= nameEnd0 + 2 && textX0 <= nameEnd0 + 14);
        // nothing below the last line
        CHECK(f0.countInk(kLx + 1, lineTop(top, 6), kLx + kLw - 1, kAy + kAh - 1, field) == 0);
        // no scroll thumb: six lines fit
        CHECK(f0.countNear(kLx + kLw - 9, bodyTop, kLx + kLw - 3, kAy + kAh, knobRing, 2) == 0);
    }

    // ============================================================================================================================
    // LogView B: nothing yet - "Listening..." on the first line, in Theme.dim
    // ============================================================================================================================
    {
        section("LogView B static");
        const int bodyTop = kBy + kBarH;                                                  // 201
        CHECK(f0.rowNear(bodyTop - 1, kLx, kLx + kLw, line1, 0));                        // the same bar, its rule on y 200
        CHECK(f0.near(kLx + 300, bodyTop + 10, field, 0) && f0.near(kLx + 300, kBy + kBh - 1, field, 0) && f0.near(kLx + 300, kBy + kBh, bg, 0));
        const int t0 = lineTop(kBy, 0);
        CHECK(f0.countInk(kTextX, t0, kTextX + 120, t0 + kPitch, field) > 40);           // "Listening..."
        CHECK(inkIs(f0, kTextX, t0, kTextX + 120, t0 + kPitch, dim));
        CHECK(f0.countNear(kTextX, t0, kLx + kLw, t0 + kPitch, fg, 14) == 0);
        const int x0 = firstInkX(f0, kLx + 1, t0, kLx + 60, t0 + kPitch, field);
        CHECK(x0 >= kTextX && x0 <= kTextX + 3);
        CHECK(f0.countInk(kLx + 1, lineTop(kBy, 1), kLx + kLw - 1, kBy + kBh - 1, field) == 0);   // and nothing else
        // the count still reads "0 lines" on the right, and the bar is the same bar
        CHECK(f0.countInk(kLx + kLw - 110, kBy + 6, kLx + kLw, kBy + 25, panel) > 25 && inkIs(f0, kLx + kLw - 110, kBy + 6, kLx + kLw, kBy + 25, dim));
        CHECK(f0.countNear(kLx + 10, kBy + 8, kLx + 24, kBy + 22, accent, 6) >= 100);   // its Follow box too
    }

    // ============================================================================================================================
    // PicturePane A: status row, the key, the fitted picture
    // ============================================================================================================================
    {
        section("PicturePane static");
        const int top = kPay, rowH = 28;                                                  // the status row is y 10..38
        // --- the status row: 11 px text in Theme.fg from x 560; an xs key 20 high centred in the row (y 14..34); the note after it
        CHECK(f0.countInk(kPx + 10, top + 2, kPx + 190, top + rowH - 2, panel) > 80);
        CHECK(inkIs(f0, kPx + 10, top + 2, kPx + 180, top + rowH - 2, fg));
        const int sx = firstInkX(f0, kPx + 1, top + 2, kPx + 40, top + rowH - 2, panel);
        CHECK(sx >= kPx + 10 && sx <= kPx + 13);                                         // 10 px of padding
        const int keyX = firstRun(f0, top + 14, kPx + 120, kPx + kPw, keyBg);
        CHECK(keyX > kPx + 150 && keyX < kPx + 260);
        CHECK(f0.near(keyX + 6, top + 4, keyLine, 0) && f0.near(keyX + 6, top + 3, panel, 0));          // y 14 .. 33: 20 high
        CHECK(f0.near(keyX + 6, top + 23, keyLine, 0) && f0.near(keyX + 6, top + 24, panel, 0));
        CHECK(f0.colNear(keyX + 6, top + 5, top + 23, keyBg, 0));
        CHECK(f0.countInk(keyX + 8, top + 6, keyX + 70, top + 22, keyBg) > 40 && inkIs(f0, keyX + 8, top + 6, keyX + 70, top + 22, keyFg));   // "Save as BMP"
        // the save note, dim, 8 px after the key's right border
        const int keyRight = lastOn(f0, top + 14, keyX, kPx + kPw, keyLine);             // the key's right border, on its middle row
        CHECK(keyRight - keyX > 60 && keyRight - keyX < 110);
        const int noteX = firstInkX(f0, keyRight + 1, top + 4, kPx + kPw - 4, top + 24, panel);
        CHECK(noteX >= keyRight + 1 + 8 && noteX <= keyRight + 1 + 12);                   // 8 px after the key
        CHECK(f0.countInk(noteX, top + 4, kPx + kPw - 4, top + 24, panel) > 30 && inkIs(f0, noteX, top + 4, kPx + kPw - 4, top + 24, dim));
        // --- the picture, 200 x 100 fitted (image-fit contain) in the free area x 560..900, y 38..230: scale 1.7, so 340 x 170 at y 49
        CHECK(f0.near(kPx + 20, 100, red, 8) && f0.near(kPx + kPw - 30, 100, blue, 8));  // left half red, right half blue
        CHECK(f0.near(kPx + 160, 100, red, 8) && f0.near(kPx + 190, 100, blue, 8));      // the split is in the middle
        CHECK(f0.near(kPx + 10 + 2, 100, red, 8) && f0.near(kPx + 10 - 3, 100, panel, 0) && f0.near(kPx + 10 + 340 + 2, 100, panel, 0) && f0.near(kPx + 10 + 340 - 3, 100, blue, 8));
        CHECK(f0.near(730, 49 + 2, red, 8) || f0.near(730, 49 + 2, blue, 8));
        CHECK(f0.near(700, 49 - 3, panel, 0) && f0.near(700, 49 + 2, red, 8));           // the top edge on y 49
        CHECK(f0.near(700, 49 + 170 - 3, red, 8) && f0.near(700, 49 + 170 + 2, panel, 0));   // the bottom edge on y 219
        // centred: the margins above and below the picture in the free area are equal (11 px)
        CHECK(f0.countNear(kPx + 10, rowH + top, kPx + 10 + 340, 49 - 1, red, 30) == 0 && f0.countNear(kPx + 10, 49 + 170 + 1, kPx + 10 + 340, kPay + kPah, red, 30) == 0);
        // the picture stays out of the status row
        CHECK(f0.countNear(kPx, top, kPx + kPw, top + rowH, red, 30) == 0 && f0.countNear(kPx, top, kPx + kPw, top + rowH, blue, 30) == 0);
    }

    // PicturePane B: the picture is height-limited (scale 0.82: 164 x 82 at x 648, y 278), the save key disabled, "Saving..."
    {
        const int top = kPby, rowH = 28;
        CHECK(f0.near(660, 300, red, 8) && f0.near(800, 300, blue, 8) && f0.near(640, 300, panel, 0) && f0.near(820, 300, panel, 0));
        CHECK(f0.near(648 + 2, 300, red, 8) && f0.near(648 - 3, 300, panel, 0) && f0.near(648 + 164 - 3, 300, blue, 8) && f0.near(648 + 164 + 2, 300, panel, 0));
        CHECK(f0.near(700, 278 + 2, red, 8) && f0.near(700, 278 - 3, panel, 0));
        CHECK(f0.near(700, 278 + 82 - 3, red, 8) && f0.near(700, 278 + 82 + 2, panel, 0));
        // the key is the disabled look: Theme.key-disabled-bg (the panel's own ground), Theme.key-disabled-fg words, and a border
        // at half alpha (Slint has no dashed border) - found by that border's straight run along the key's top, y 254
        const int run = firstLongRun(f0, top + 4, kPx + 120, kPx + kPw, offBorder, 20, 2);
        CHECK(run > kPx + 156 && run < kPx + 270);                                       // the run starts after the key's 6 px corner
        const int keyL = run - 6;
        CHECK(!f0.near(run + 10, top + 4, keyLine, 0) && f0.near(run + 10, top + 23, offBorder, 2));      // not the enabled border; 20 high
        CHECK(f0.near(run + 10, top + 3, panel, 0) && f0.near(run + 10, top + 24, panel, 0));
        CHECK(f0.colNear(keyL + 4, top + 7, top + 22, keyOffBg, 0));
        CHECK(f0.countInk(keyL + 8, top + 6, keyL + 70, top + 22, keyOffBg) > 40 && inkIs(f0, keyL + 8, top + 6, keyL + 70, top + 22, keyOffFg, 24));
        CHECK(f0.countNear(keyL + 8, top + 6, keyL + 70, top + 22, keyFg, 14) == 0);
        // the note, "Saving...", after the key (the key is about 76 px wide: the region starts past it)
        CHECK(f0.countInk(keyL + 92, top + 4, kPx + kPw - 4, top + 24, panel) > 20);
        CHECK(f0.countNear(keyL + 92, top + 4, kPx + kPw - 4, top + 24, fg, 14) == 0 && rowH == 28);
    }

    // PicturePane C: no picture yet - the dim note, no key, no picture
    {
        const int top = kPcy;
        CHECK(f0.countInk(kPx + 10, top + 2, kPx + 260, top + 26, panel) > 120);         // "Waiting for the first image..."
        CHECK(inkIs(f0, kPx + 10, top + 2, kPx + 260, top + 26, dim));
        CHECK(f0.countNear(kPx + 10, top + 2, kPx + 260, top + 26, fg, 14) == 0);
        CHECK(firstInkX(f0, kPx + 1, top + 2, kPx + 60, top + 26, panel) >= kPx + 10 && firstInkX(f0, kPx + 1, top + 2, kPx + 60, top + 26, panel) <= kPx + 13);
        CHECK(f0.countNear(kPx, top, kPx + kPw, top + kPch, red, 30) == 0 && f0.countNear(kPx, top, kPx + kPw, top + kPch, blue, 30) == 0);
        bool anyDisabledKey = false;
        for (int y = top; y < top + kPch; ++y) { anyDisabledKey = anyDisabledKey || firstLongRun(f0, y, kPx, kPx + kPw, offBorder, 20, 2) >= 0; }
        CHECK(!anyRun(f0, kPx, top, kPx + kPw, top + kPch, keyBg) && !anyDisabledKey);   // no key, of either look
        CHECK(f0.countInk(kPx + 1, top + 30, kPx + kPw - 1, top + kPch - 1, panel) == 0);   // nothing else
    }

    // ============================================================================================================================
    // TargetDetails
    // ============================================================================================================================
    // the bottom border row of a framed panel at (x0, y0): the last row of Theme.line in the column x0 + 40 above `limit`
    const auto panelHeight = [&](const Frame& f, int x0, int y0, int limit) {
        int last = -1;
        for (int y = y0; y < limit; ++y) { if (f.near(x0 + 40, y, line1, 0) && f.near(x0 + 40, y - 1, panel, 0) && !f.near(x0 + 40, y + 1, panel, 0)) { last = y; } }
        return last < 0 ? -1 : last - y0 + 1;
    };
    // --- A: the live panel with a heading line, two rules and two unknown lines
    int aH = 0, bH = 0, cH = 0, dH = 0;
    {
        const int x0 = kTx, y0 = 10;
        aH = panelHeight(f0, x0, y0, y0 + 300);
        std::printf("target details A: %d high\n", aH);
        // 1 + 8 + 24 + 4, then the lines (22 + 15 + 15 + 20 + 15 * 5 + 8 gaps of 2 = 163), then 8 + 1
        CHECK(aH >= 205 && aH <= 213);
        // the frame: Theme.panel with a 1 px Theme.line border, 8 px corners, 340 wide
        CHECK(f0.rowNear(y0, x0 + 12, x0 + kTw - 12, line1, 0) && f0.near(x0 + 40, y0 + 1, panel, 0) && f0.near(x0 + 40, y0 - 1, bg, 0));
        CHECK(f0.colNear(x0, y0 + 12, y0 + aH - 12, line1, 0) && f0.colNear(x0 + kTw - 1, y0 + 12, y0 + aH - 12, line1, 0));
        CHECK(f0.near(x0 - 1, y0 + 40, bg, 0) && f0.near(x0 + kTw, y0 + 40, bg, 0) && f0.near(x0 + 1, y0 + 40, panel, 0));
        CHECK(f0.near(x0, y0, bg, 10) && f0.near(x0 + kTw - 1, y0, bg, 10));             // the corners are cut, not square
        // the title row: a green dot (8 px) at x 941, y 27..35; "Target details" at x 955 in Theme.fg; the chip at the right
        CHECK(f0.near(x0 + 15, y0 + 21, lampGreen, 8) && f0.near(x0 + 8, y0 + 21, panel, 0) && f0.near(x0 + 22, y0 + 21, panel, 0));
        CHECK(f0.near(x0 + 15, y0 + 16, panel, 0) && f0.near(x0 + 15, y0 + 26, panel, 0));              // 8 px: ink from y0 + 17 to y0 + 25
        CHECK(f0.countInk(x0 + 26, y0 + 10, x0 + 130, y0 + 32, panel) > 80 && inkIs(f0, x0 + 26, y0 + 10, x0 + 130, y0 + 32, fg));
        const int titleX = firstInkX(f0, x0 + 20, y0 + 10, x0 + 60, y0 + 32, panel);
        CHECK(titleX >= x0 + 25 && titleX <= x0 + 28);                                   // 6 px after the dot
        // the chip "A1B2C3": the green chip, its right edge 11 px from the panel's edge, 20 px high, centred in the 24 px row
        CHECK(f0.countNear(x0 + kTw - 90, y0 + 10, x0 + kTw - 11, y0 + 32, chipGoodBg, 4) > 300);
        const int chipRight = lastInkX(f0, x0 + kTw - 40, y0 + 12, x0 + kTw - 4, y0 + 36, panel);
        CHECK(chipRight >= x0 + kTw - 13 && chipRight <= x0 + kTw - 11 && f0.near(x0 + kTw - 11, y0 + 21, panel, 0));
        const int chipTop = firstInkY(f0, x0 + kTw - 40, y0 + 2, x0 + kTw - 20, y0 + 36, panel);
        const int chipBottom = lastInkY(f0, x0 + kTw - 40, y0 + 2, x0 + kTw - 20, y0 + 36, panel);
        CHECK(chipBottom - chipTop + 1 == 20 && chipTop == y0 + 11);
        // the heading line (13 px semibold, no label): the first of the lines, at y 47
        const int hy = y0 + 1 + 8 + 24 + 4;
        CHECK(f0.countInk(x0 + 11, hy, x0 + 140, hy + 17, panel) > 40 && inkIs(f0, x0 + 11, hy, x0 + 140, hy + 17, fg, 10));
        CHECK(firstInkX(f0, x0 + 1, hy, x0 + 60, hy + 17, panel) >= x0 + 11 && firstInkX(f0, x0 + 1, hy, x0 + 60, hy + 17, panel) <= x0 + 14);
        // the lines: 11 px; known lines in Theme.fg, unknown ones dim
        const int lx = x0 + 11, lw = 150;
        const int regY = hy + 22 + 2;                                                    // the heading's item is 22 high (its rule included) + 2
        CHECK(inkIs(f0, lx, regY, lx + lw, regY + 15, fg) && f0.countInk(lx, regY, lx + lw, regY + 15, panel) > 30);
        // the unknown lines are the last two: 15 + 2 apart
        const int altY = hy + 22 + 2 + (15 + 2) + (15 + 2) + (20 + 2) + (15 + 2) * 3;     // reg, type, oper (20 with its rule), id, from, pos, then alt
        CHECK(f0.countInk(lx, altY, lx + lw, altY + 15, panel) > 30 && inkIs(f0, lx, altY, lx + lw, altY + 15, dim));
        CHECK(f0.countNear(lx, altY, lx + lw, altY + 15 + 17, fg, 14) == 0);
        CHECK(inkIs(f0, lx, altY + 17, lx + lw, altY + 17 + 15, dim));                    // "speed unknown"
        // the rules: exactly two hairlines across the lines column (after the heading line, after "oper"), none in the keys column
        CHECK(rulesIn(f0, lx + 2, lx + lw, hy, y0 + aH - 8, line1) == 2);
        // the keys: a 120 px column at the right, two small keys of 24 px with 4 px between, top-aligned with the first line
        const int kx = x0 + kTw - 11 - kKeysW;
        CHECK(f0.near(kx, hy + 12, keyLine, 0) && f0.near(kx - 1, hy + 12, panel, 0) && f0.near(kx + kKeysW - 1, hy + 12, keyLine, 0) && f0.near(kx + kKeysW, hy + 12, panel, 0));
        CHECK(f0.near(kx + 60, hy, keyLine, 0) && f0.near(kx + 60, hy - 1, panel, 0) && f0.near(kx + 60, hy + 23, keyLine, 0) && f0.near(kx + 60, hy + 24, panel, 0));
        CHECK(f0.near(kx + 60, hy + 28, keyLine, 0) && f0.near(kx + 60, hy + 27, panel, 0) && f0.near(kx + 60, hy + 51, keyLine, 0) && f0.near(kx + 60, hy + 52, panel, 0));
        CHECK(f0.colNear(kx + 6, hy + 1, hy + 23, keyBg, 0) && f0.colNear(kx + 6, hy + 29, hy + 51, keyBg, 0));
        CHECK(f0.countInk(kx + 10, hy + 3, kx + 110, hy + 21, keyBg) > 50 && inkIs(f0, kx + 10, hy + 3, kx + 110, hy + 21, keyFg));         // "Go to on map"
        CHECK(f0.countInk(kx + 10, hy + 31, kx + 110, hy + 49, keyBg) > 30 && inkIs(f0, kx + 10, hy + 31, kx + 110, hy + 49, keyFg));       // "Follow"
        CHECK(f0.countNear(kx, hy + 56, kx + kKeysW, y0 + aH - 4, keyBg, 0) == 0);        // and no third key
        // the lines column leaves room for the keys: 12 px between them, and a long line is cut short, not drawn under them
        CHECK(f0.countInk(kx - 12, hy, kx, y0 + aH - 8, panel) == 0);
    }
    // --- B: the heading property, a followed target (Stop following), no position (Go disabled), Close, an amber lamp
    {
        const int x0 = kTx, y0 = 230;
        bH = panelHeight(f0, x0, y0, y0 + 160);
        std::printf("target details B: %d high\n", bH);
        CHECK(bH >= 122 && bH <= 130);                                                   // the keys' 24 + 4 + 24 + 4 + 24 = 80 are taller than its 3 lines
        CHECK(f0.near(x0 + 15, y0 + 21, lampAmber, 8));
        CHECK(f0.countNear(x0 + kTw - 110, y0 + 10, x0 + kTw - 11, y0 + 32, chipWarnBg, 4) > 400);       // "A1B2C3D4E5": the amber chip
        const int hy = y0 + 1 + 8 + 24 + 4;
        CHECK(f0.countInk(x0 + 11, hy, x0 + 140, hy + 17, panel) > 40 && inkIs(f0, x0 + 11, hy, x0 + 140, hy + 17, fg, 10));
        CHECK(rulesIn(f0, x0 + 13, x0 + 160, hy, y0 + bH - 8, line1) == 0);               // no separator-after: no rule
        const int kx = x0 + kTw - 11 - kKeysW;
        // Go to on map: disabled (no position)
        CHECK(!f0.near(kx + 30, hy, keyLine, 0) && f0.near(kx + 30, hy, offBorder, 2) && f0.near(kx + 30, hy + 23, offBorder, 2));   // the half-alpha border
        CHECK(f0.colNear(kx + 6, hy + 1, hy + 23, keyOffBg, 0) && f0.countInk(kx + 10, hy + 3, kx + 110, hy + 21, keyOffBg) > 50);
        CHECK(inkIs(f0, kx + 10, hy + 3, kx + 110, hy + 21, keyOffFg, 24) && f0.countNear(kx + 10, hy + 3, kx + 110, hy + 21, keyFg, 14) == 0);
        // Stop following (following, so it is not gated by the position) and Close: ordinary keys
        CHECK(f0.colNear(kx + 6, hy + 29, hy + 51, keyBg, 0) && f0.countInk(kx + 10, hy + 31, kx + 110, hy + 49, keyBg) > 60 && inkIs(f0, kx + 10, hy + 31, kx + 110, hy + 49, keyFg));
        CHECK(f0.colNear(kx + 6, hy + 57, hy + 79, keyBg, 0) && f0.countInk(kx + 10, hy + 59, kx + 110, hy + 77, keyBg) > 30 && inkIs(f0, kx + 10, hy + 59, kx + 110, hy + 77, keyFg));
        CHECK(f0.near(kx + 60, hy + 80, panel, 0));
    }
    // --- C: gone - the id and the sentence, Close only, the lamp out
    {
        const int x0 = kTx, y0 = 366;
        cH = panelHeight(f0, x0, y0, y0 + 120);
        std::printf("target details C: %d high\n", cH);
        CHECK(cH >= 76 && cH <= 84);
        CHECK(f0.near(x0 + 15, y0 + 21, lampOff, 4));                                    // lit only while the target is still reported
        CHECK(f0.countNear(x0 + kTw - 110, y0 + 10, x0 + kTw - 11, y0 + 32, chipBg, 4) > 300);   // the neutral chip
        const int hy = y0 + 1 + 8 + 24 + 4;
        CHECK(f0.countInk(x0 + 11, hy, x0 + 140, hy + 17, panel) > 40 && inkIs(f0, x0 + 11, hy, x0 + 140, hy + 17, fg, 10));        // the id, as the heading
        CHECK(f0.countInk(x0 + 11, hy + 19, x0 + 200, hy + 34, panel) > 60 && inkIs(f0, x0 + 11, hy + 19, x0 + 200, hy + 34, dim));    // "No longer being reported."
        CHECK(f0.countNear(x0 + 11, hy + 17, x0 + 200, y0 + cH - 2, fg, 14) == 0);       // the lines the host still holds (two, known) are not shown
        const int kx = x0 + kTw - 11 - kKeysW;
        CHECK(f0.colNear(kx + 6, hy + 1, hy + 23, keyBg, 0) && f0.countInk(kx + 10, hy + 3, kx + 110, hy + 21, keyBg) > 30);               // Close, first in the column
        bool anyOff = false;
        for (int y = hy + 28; y < y0 + cH - 2; ++y) { anyOff = anyOff || firstLongRun(f0, y, kx, kx + kKeysW, offBorder, 20, 2) >= 0; }
        CHECK(!anyRun(f0, kx, hy + 28, kx + kKeysW, y0 + cH - 2, keyBg) && !anyOff);   // no Go, no Follow, of either look
    }
    // --- D: nothing chosen - the hint, dim, wrapped; the chip NONE; no keys
    {
        const int x0 = kTx, y0 = 456;
        dH = panelHeight(f0, x0, y0, y0 + 100);
        std::printf("target details D: %d high\n", dH);
        CHECK(dH >= 70 && dH <= 82);
        CHECK(f0.near(x0 + 15, y0 + 21, lampOff, 4));
        const int hy = y0 + 1 + 8 + 24 + 4;
        CHECK(f0.countInk(x0 + 11, hy, x0 + kTw - 11, hy + 34, panel) > 300 && inkIs(f0, x0 + 11, hy, x0 + kTw - 11, hy + 34, dim));
        CHECK(f0.countNear(x0 + 11, hy, x0 + kTw - 11, hy + 34, fg, 14) == 0);
        CHECK(lastInkY(f0, x0 + 11, hy, x0 + kTw - 11, y0 + dH - 2, panel) > hy + 16);    // it wraps: a second line under the first
        CHECK(f0.countNear(x0 + 11, hy, x0 + kTw - 11, y0 + dH - 2, keyBg, 0) == 0);      // no keys at all
        CHECK(lastInkX(f0, x0 + 11, hy, x0 + kTw - 11, hy + 34, panel) <= x0 + kTw - 11); // inside the padding
    }
    // --- E: the page form - unframed, the lines in the mono face (a label padded to a column lines up), Go and Follow
    {
        const int x0 = 10, y0 = 420;
        // no frame of its own: the panel ground the specimen puts behind it shows to the very edge
        CHECK(f0.near(x0, y0 + 40, panel, 0) && f0.near(x0 + 1, y0 + 1, panel, 0) && f0.near(x0 + 40, y0, panel, 0));
        const int hy = y0 + 1 + 8 + 24 + 4;
        CHECK(f0.near(x0 + 15, y0 + 21, lampGreen, 8));
        CHECK(f0.countInk(x0 + 11, hy, x0 + 140, hy + 17, panel) > 40);
        // mono: "reg     G-XWBA" and "type    737NG": both values start at the eighth character, so at the same x
        const int ly = hy + 17 + 2;
        const int vx1 = firstInkX(f0, x0 + 11 + 40, ly, x0 + 11 + 120, ly + 15, panel);
        const int vx2 = firstInkX(f0, x0 + 11 + 40, ly + 17, x0 + 11 + 120, ly + 17 + 15, panel);
        CHECK(vx1 > 0 && vx2 > 0 && std::abs(vx1 - vx2) <= 1);
        CHECK(vx1 >= x0 + 11 + static_cast<int>(8 * kGlyph) - 1 && vx1 <= x0 + 11 + static_cast<int>(8 * kGlyph) + 3);
        // and the proportional face of variant A does NOT line up the same pair (reg / type are different widths in Inter)
        const int ax1 = firstInkX(f0, kTx + 11 + 26, 71, kTx + 11 + 80, 71 + 15, panel);
        const int ax2 = firstInkX(f0, kTx + 11 + 26, 71 + 17, kTx + 11 + 80, 71 + 17 + 15, panel);
        CHECK(ax1 > 0 && ax2 > 0 && std::abs(ax1 - ax2) >= 2);
    }
    // --- F: the drawer in the 296 px sections panel - narrower than side-min-w, so the keys are a row under the lines
    int fH = 0;
    {
        const int x0 = 370, y0 = 470, w = 296;
        fH = panelHeight(f0, x0, y0, y0 + 200);
        std::printf("target details F: %d high\n", fH);
        // 1 + 8 + 24 + 4, the lines (17 + 3 * 15 + 3 gaps of 2 = 68), 4, the key row (26: 2 above its 24), 8 + 1
        CHECK(fH >= 140 && fH <= 148);
        CHECK(f0.rowNear(y0, x0 + 12, x0 + w - 12, line1, 0) && f0.colNear(x0 + w - 1, y0 + 12, y0 + fH - 12, line1, 0));
        const int hy = y0 + 1 + 8 + 24 + 4;
        // the lines take the whole width: "oper    Federal Express Corporation" reaches well past the 150 px a 120 px column would leave
        const int operY = hy + 17 + 2 + (15 + 2) * 2;
        CHECK(lastInkX(f0, x0 + 11, operY, x0 + w - 11, operY + 15, panel) > x0 + 11 + 160);
        CHECK(inkIs(f0, x0 + 11, operY, x0 + w - 11, operY + 15, fg));
        // no column of keys at the right of the lines
        CHECK(!anyRun(f0, x0 + w - 11 - 120, hy, x0 + w - 11, hy + 68, keyBg));
        // the row: three small keys 24 high on y 581..604, the first at the content's left edge, 6 px apart
        const int rowTop = hy + 68 + 4 + 2;
        const std::vector<int> runs = runStarts(f0, rowTop + 4, x0 + 2, x0 + w - 2, keyBg, 8);
        CHECK(runs.size() == 3);
        if (runs.size() == 3) {
            CHECK(runs[0] >= x0 + 11 + 1 && runs[0] <= x0 + 11 + 2);                                               // the first key's ground starts inside its border at x 381
            for (const int r : runs) {
                CHECK(f0.near(r + 20, rowTop, keyLine, 0) && f0.near(r + 20, rowTop - 1, panel, 0));              // top border, nothing above
                CHECK(f0.near(r + 20, rowTop + 23, keyLine, 0) && f0.near(r + 20, rowTop + 24, panel, 0));       // 24 high
            }
            CHECK(runs[1] - runs[0] > 40 && runs[2] - runs[1] > 40);
        }
        CHECK(f0.countInk(x0 + 12, rowTop + 6, x0 + 100, rowTop + 18, keyBg) > 30 && inkIs(f0, x0 + 12, rowTop + 6, x0 + 100, rowTop + 18, keyFg));   // "Go to on map"
    }
    CHECK(10 + aH < 230 && 230 + bH < 366 && 366 + cH < 456 && 456 + dH < kH);        // A, B, C and D do not touch each other
    CHECK(470 + fH < kH && 10 + 340 < 370);                                             // F is on the sheet, and clear of E at its left (E ends at x 350)

    // ============================================================================================================================
    // INTERACTION: LogView A
    // ============================================================================================================================
    {
        section("LogView A interaction");
        const int top = kAy;
        const int boxX = kLx + 10, boxY = top + 8;
        const int clearX = firstRun(f0, top + 15, boxX + 40, kLx + kLw / 2, keyBg);
        CHECK(ui->get_clears_a() == 0 && ui->get_follow_toggles() == 0 && ui->get_follow_a());
        // Clear: one click, clear-clicked
        click(win, clearX + 12.0f, top + 15.0f);
        CHECK(ui->get_clears_a() == 1 && ui->get_clears_b() == 0 && ui->get_clears_c() == 0);
        click(win, clearX + 12.0f, top + 15.0f, slint::PointerEventButton::Right);       // a right click is not a click
        CHECK(ui->get_clears_a() == 1);
        // the key lights under the pointer (Theme.menu-hot) like every key
        moveTo(win, clearX + 12.0f, top + 15.0f);
        CHECK(shot().near(clearX + 6, top + 10, tok(th.get_menu_hot()), 0));
        away();
        // Follow: the box toggles, follow-toggled(false), the two-way property follows
        click(win, boxX + 7.0f, top + 15.0f);
        CHECK(ui->get_follow_toggles() == 1 && !ui->get_last_follow() && !ui->get_follow_a());
        const Frame off = shot();
        CHECK(off.countNear(boxX, boxY, boxX + 14, boxY + 14, accent, 6) == 0);          // not accent any more
        CHECK(off.near(boxX + 6, boxY + 6, field, 0) && off.near(boxX, boxY + 6, lineStrong, 0));    // the field ground in a Theme.line-strong border
        click(win, boxX + 40.0f, top + 15.0f);                                           // the word toggles too
        CHECK(ui->get_follow_toggles() == 2 && ui->get_last_follow() && ui->get_follow_a());
        CHECK(shot().countNear(boxX, boxY, boxX + 14, boxY + 14, tok(th.get_accent_hover()), 6) >= 60);    // under the pointer a lit box is the hover accent
        away();
        CHECK(shot().countNear(boxX, boxY, boxX + 14, boxY + 14, accent, 6) >= 100);
        // writing the property from the host moves the box without firing anything
        ui->set_follow_a(false);
        CHECK(ui->get_follow_toggles() == 2 && shot().countNear(boxX, boxY, boxX + 14, boxY + 14, accent, 6) == 0);
        ui->set_follow_a(true);
        CHECK(ui->get_follow_toggles() == 2 && shot().countNear(boxX, boxY, boxX + 14, boxY + 14, accent, 6) >= 100);
        // disabled: neither acts, both show the disabled look
        ui->set_logs_enabled(false);
        click(win, clearX + 12.0f, top + 15.0f);
        click(win, boxX + 7.0f, top + 15.0f);
        CHECK(ui->get_clears_a() == 1 && ui->get_follow_toggles() == 2);
        const Frame dis = shot();
        CHECK(dis.colNear(clearX + 6, top + 6, top + 24, keyOffBg, 0));
        CHECK(dis.near(clearX + 10, top + 5, offBorder, 2) && !dis.near(clearX + 10, top + 5, keyLine, 0));         // the key's border at half alpha
        CHECK(dis.near(boxX + 2, boxY + 2, keyOffBg, 0) && dis.countNear(boxX, boxY, boxX + 14, boxY + 14, accent, 6) == 0);
        CHECK(dis.near(boxX, boxY + 6, over(lineStrong, 0.5, panel), 2));                                          // and the box's
        CHECK(dis.countInk(clearX + 8, top + 7, clearX + 40, top + 23, keyOffBg) > 20 && dis.countNear(clearX + 8, top + 7, clearX + 40, top + 23, keyFg, 14) == 0);
        // the log itself is not disabled: its lines are as they were
        CHECK(differing(f0, dis, kLx, kAy + kBarH, kLx + kLw, kAy + kAh) == 0);
        ui->set_logs_enabled(true);
        away();
        CHECK(differing(f0, shot(), kLx, kAy, kLx + kLw, kAy + kAh) == 0);               // back to the sheet, pixel for pixel
    }

    // ============================================================================================================================
    // LogView C: a long log is a virtual list, and Follow keeps the newest line in view
    // ============================================================================================================================
    {
        section("LogView C streaming");
        using clock = std::chrono::steady_clock;
        const int cBodyTop = kCy + kBarH, cListBottom = kCy + kCh - kPad;               // y 281, and the list ends 6 px above the foot, y 394
        const int boxX = kLx + 10, boxCentreY = kCy + 15;
        const auto cRegion = [&] { return std::array<int, 4>{kLx, cBodyTop, kLx + kLw, kCy + kCh}; };
        const auto cCalm = [&] { const auto r = cRegion(); calm(r[0], r[1], r[2], r[3]); };
        const auto lastRow = [&](const Frame& f, Rgb want) { return inkIs(f, afterText(9) + 4, cListBottom - kPitch + 1, kLx + kLw - 20, cListBottom - 1, want); };
        const auto rowAbove = [&](const Frame& f, int k, Rgb want) { return inkIs(f, afterText(9) + 4, cListBottom - kPitch * (k + 1) + 1, kLx + kLw - 20, cListBottom - kPitch * k - 1, want); };

        auto model = std::make_shared<slint::VectorModel<LogLine>>();
        for (int i = 0; i < 499; ++i) {
            // frame 497 is a line far too long for the log: it is cut at the right padding with an ellipsis, not run under the thumb
            const std::string text = i == 497 ? "frame 497 " + std::string(300, 'x') : "frame " + std::to_string(i);
            model->push_back(line("c-decoder", text, LineTone::Normal));
        }
        model->push_back(line("c-decoder", "ERROR the newest of 500", LineTone::Err));
        away();

        // 500 lines in, follow on: the view opens at the END, the newest line whole on the last row
        const auto t0 = clock::now();
        ui->set_lines_c(model);
        Frame c0 = shot();
        const double ms = std::chrono::duration<double, std::milli>(clock::now() - t0).count();
        std::printf("log: 500 lines set and the first frame rendered in %.1f ms\n", ms);
        CHECK(ms < 250.0);
        cCalm();
        const Frame c1 = shot();
        CHECK(c1.savePng(shotDir() + "output-specimen-streaming.png"));
        CHECK(lastRow(c1, errFg));                                                       // the ERROR line is the last row
        CHECK(rowAbove(c1, 1, fg) && rowAbove(c1, 2, fg));                               // frame 498, 497 above it
        // the long line (497, two rows above the last): its ink fills the row out to the right padding (x 520) and stops there
        const int longTop = cListBottom - 3 * kPitch + 1, longBottom = cListBottom - 2 * kPitch - 1;
        CHECK(lastInkX(c1, afterText(9) + 4, longTop, kLx + kLw - 9, longBottom, field) >= kLx + kLw - 16);
        CHECK(c1.countInk(kLx + kLw - 9, longTop, kLx + kLw, longBottom + 1, field) == 0);   // nothing in the 10 px of right padding (the thumb is lower down)
        CHECK(c1.countNear(kLx + 1, cBodyTop, kLx + kLw - 20, cListBottom, warnFg, 14) == 0);
        // whole: the row's ink is complete (its descenders reach the same row of pixels as the ink of the row above, 17 px up)
        const int endInk = lastInkY(c1, kTextX, cListBottom - kPitch, kTextX + 70, cListBottom + 3, field);
        const int prevInk = lastInkY(c1, kTextX, cListBottom - 2 * kPitch, kTextX + 70, cListBottom - kPitch + 1, field);
        CHECK(endInk > 0 && prevInk > 0 && endInk - prevInk == kPitch && endInk < cListBottom);
        // no gap under it: the body's 6 px of padding, then the foot
        CHECK(c1.countInk(kLx + 1, cListBottom, kLx + kLw - 1, kCy + kCh - 1, field) == 0);
        // a thumb: 6 px, at the foot of the travel
        CHECK(c1.countNear(kLx + kLw - 9, cBodyTop, kLx + kLw - 3, kCy + kCh, knobRing, 2) > 50);
        CHECK(c1.near(kLx + kLw - 6, kCy + kCh - 8, knobRing, 4));

        // the view is resized (the window dragged shorter, then back): Follow keeps the newest line in view both times
        ui->set_c_h(110.0f);
        cCalm();
        const int shortBottom = kCy + 110 - kPad;                                          // the list now ends on y 354
        const Frame cShort = shot();
        CHECK(inkIs(cShort, afterText(9) + 4, shortBottom - kPitch + 1, kLx + kLw - 20, shortBottom - 1, errFg));
        CHECK(cShort.countInk(kLx + 1, shortBottom, kLx + kLw - 1, kCy + 110 - 1, field) == 0);
        ui->set_c_h(150.0f);
        cCalm();
        CHECK(lastRow(shot(), errFg));

        // one line arrives (the host pushes into its model): the view stays on the end
        model->push_back(line("c-decoder", "WARN a line arrives", LineTone::Warn));
        cCalm();
        const Frame c2 = shot();
        CHECK(lastRow(c2, warnFg) && rowAbove(c2, 1, errFg));
        // ten at once
        for (int i = 0; i < 10; ++i) { model->push_back(line("c-decoder", "burst " + std::to_string(i), LineTone::Normal)); }
        model->push_back(line("c-decoder", "WARN the end of the burst", LineTone::Warn));
        cCalm();
        const Frame c3 = shot();
        CHECK(lastRow(c3, warnFg) && rowAbove(c3, 1, fg));

        // Follow off (click the box): follow-toggled(false), and a line that arrives is not scrolled to
        const int toggles = ui->get_follow_toggles();
        click(win, boxX + 7.0f, static_cast<float>(boxCentreY));
        CHECK(ui->get_follow_toggles() == toggles + 1 && !ui->get_last_follow() && !ui->get_follow_c());
        cCalm();
        const Frame c4 = shot();
        model->push_back(line("c-decoder", "ERROR arrives while not following", LineTone::Err));
        cCalm();
        const Frame c5 = shot();
        CHECK(differing(c4, c5, kLx, cBodyTop, kLx + kLw - 12, kCy + kCh) == 0);         // the view did not move
        CHECK(c5.countNear(kLx + 1, cBodyTop, kLx + kLw - 20, cListBottom, errFg, 14) == 0);
        // Follow on again: it jumps to the end at once, the newest line whole
        click(win, boxX + 7.0f, static_cast<float>(boxCentreY));
        CHECK(ui->get_follow_toggles() == toggles + 2 && ui->get_last_follow() && ui->get_follow_c());
        cCalm();
        const Frame c6 = shot();
        CHECK(lastRow(c6, errFg) && rowAbove(c6, 1, warnFg));

        // scrolling up with Follow on turns nothing off ... and the next line brings the view back to the end
        scroll(win, 200.0f, 340.0f, 0.0f, 68.0f);
        cCalm();
        const Frame c7 = shot();
        CHECK(differing(c6, c7, kLx, cBodyTop, kLx + kLw - 12, kCy + kCh) > 500);       // it moved, four rows
        CHECK(!lastRow(c7, errFg) && ui->get_follow_c() && ui->get_follow_toggles() == toggles + 2);
        model->push_back(line("c-decoder", "WARN after the user scrolled up", LineTone::Warn));
        cCalm();
        CHECK(lastRow(shot(), warnFg));
        // the wheel the other way is the same distance
        scroll(win, 200.0f, 340.0f, 0.0f, 68.0f);
        cCalm();
        const Frame c8 = shot();
        scroll(win, 200.0f, 340.0f, 0.0f, -68.0f);
        cCalm();
        CHECK(lastRow(shot(), warnFg) && differing(c8, shot(), kLx, cBodyTop, kLx + kLw - 12, kCy + kCh) > 500);
        // a thumb drag scrolls it too
        const int thumbX = kLx + kLw - 6;
        press(win, static_cast<float>(thumbX), static_cast<float>(kCy + kCh - 10));
        moveTo(win, static_cast<float>(thumbX), static_cast<float>(kCy + kCh - 40));
        moveTo(win, static_cast<float>(thumbX), static_cast<float>(kCy + kCh - 70));
        release(win, static_cast<float>(thumbX), static_cast<float>(kCy + kCh - 70));
        away();
        cCalm();
        CHECK(!lastRow(shot(), warnFg));
        model->push_back(line("c-decoder", "ERROR the line after the thumb", LineTone::Err));
        cCalm();
        CHECK(lastRow(shot(), errFg));

        // Clear: the host empties its model; "Listening..." returns, and a line that arrives starts at the TOP, not at an old scroll position
        const int clears = ui->get_clears_c();
        const int clearX = firstRun(c1, kCy + 15, boxX + 40, kLx + kLw / 2, keyBg);
        click(win, clearX + 12.0f, static_cast<float>(kCy + 15));
        CHECK(ui->get_clears_c() == clears + 1);
        model->clear();
        ui->set_count_c("0 lines");
        cCalm();
        const Frame c9 = shot();
        CHECK(c9.countNear(kLx + 1, cBodyTop, kLx + kLw - 20, cListBottom, errFg, 14) == 0 && c9.countNear(kLx + 1, cBodyTop, kLx + kLw - 20, cListBottom, warnFg, 14) == 0);
        CHECK(inkIs(c9, kTextX, lineTop(kCy, 0), kTextX + 120, lineTop(kCy, 0) + kPitch, dim) && c9.countInk(kTextX, lineTop(kCy, 0), kTextX + 120, lineTop(kCy, 0) + kPitch, field) > 40);
        CHECK(c9.countNear(kLx + kLw - 9, cBodyTop, kLx + kLw - 3, kCy + kCh, knobRing, 2) == 0);         // nothing to scroll: no thumb
        model->push_back(line("c-decoder", "the first line again", LineTone::Normal));
        cCalm();
        const Frame c10 = shot();
        CHECK(inkIs(c10, afterText(9) + 4, lineTop(kCy, 0), kLx + kLw - 20, lineTop(kCy, 0) + kPitch, fg));
        CHECK(c10.countInk(kLx + 1, lineTop(kCy, 1), kLx + kLw - 1, kCy + kCh - 1, field) == 0);

        // 20000 lines are the same cost: only the lines in view are made
        auto big = std::make_shared<slint::VectorModel<LogLine>>();
        for (int i = 0; i < 19999; ++i) { big->push_back(line("c-decoder", "frame " + std::to_string(i), LineTone::Normal)); }
        big->push_back(line("c-decoder", "ERROR the 20000th", LineTone::Err));
        const auto t1 = clock::now();
        ui->set_lines_c(big);
        const Frame c11 = shot();
        const double ms20k = std::chrono::duration<double, std::milli>(clock::now() - t1).count();
        std::printf("log: 20000 lines set and the first frame rendered in %.1f ms\n", ms20k);
        CHECK(ms20k < 250.0);
        CHECK(ms20k < 4.0 * std::max(ms, 15.0));
        (void)c11;
        cCalm();
        CHECK(lastRow(shot(), errFg));                                                    // at the end of 20000 lines
        ui->set_lines_c(std::make_shared<slint::VectorModel<LogLine>>());
        ui->set_count_c("0 lines");
        cCalm();
    }

    // ============================================================================================================================
    // INTERACTION: PicturePane
    // ============================================================================================================================
    {
        section("PicturePane interaction");
        away();
        const int fitX = 560, fitY = 49;                                                   // the fitted picture: 340 x 170, scale 1.7
        CHECK(ui->get_pressed_count() == 0 && !ui->get_hand_a() && !ui->get_hand_b());
        // the hand over the picture, the plain pointer over the margin and the status row
        moveTo(win, 730.0f, 130.0f);
        CHECK(ui->get_hand_a());
        moveTo(win, 730.0f, 44.0f);                                                       // inside the pane, above the picture (y 38..49)
        CHECK(!ui->get_hand_a());
        moveTo(win, 730.0f, 225.0f);                                                      // below it
        CHECK(!ui->get_hand_a());
        moveTo(win, 700.0f, 24.0f);                                                       // the status row
        CHECK(!ui->get_hand_a());
        moveTo(win, 730.0f, 300.0f);                                                      // pane B's picture: not interactive
        CHECK(!ui->get_hand_b() && !ui->get_hand_a());
        away();
        CHECK(!ui->get_hand_a());

        // a click is pressed(x, y) in the picture's OWN pixels: scale 1.7, so 86 / 1.7 = 50.6 -> 50 and 35 / 1.7 = 20.6 -> 20
        click(win, fitX + 86.0f, fitY + 35.0f);
        CHECK(ui->get_pressed_count() == 1 && ui->get_last_px() == 50 && ui->get_last_py() == 20);
        click(win, fitX + 0.5f, fitY + 0.5f);                                              // the first pixel
        CHECK(ui->get_pressed_count() == 2 && ui->get_last_px() == 0 && ui->get_last_py() == 0);
        click(win, fitX + 339.5f, fitY + 169.5f);                                          // the last pixel: 339.5 / 1.7 = 199.7, 169.5 / 1.7 = 99.7
        CHECK(ui->get_pressed_count() == 3 && ui->get_last_px() == 199 && ui->get_last_py() == 99);
        click(win, fitX + 170.0f, fitY + 85.0f);                                           // the middle: 100, 50
        CHECK(ui->get_pressed_count() == 4 && ui->get_last_px() == 100 && ui->get_last_py() == 50);
        // outside the fitted picture, inside the pane: no press (the margins above and below, the status row)
        click(win, 730.0f, 44.0f);
        click(win, 730.0f, 225.0f);
        click(win, 700.0f, 24.0f);
        CHECK(ui->get_pressed_count() == 4);
        // a press that starts on the picture and is released outside it is no click (the plugin must never be sent a stray position)
        press(win, 730.0f, 130.0f);
        release(win, 730.0f, 44.0f);
        CHECK(ui->get_pressed_count() == 4);
        click(win, 730.0f, 130.0f, slint::PointerEventButton::Right);                      // and a right click is no press
        CHECK(ui->get_pressed_count() == 4);
        // pane B shows the picture but is not interactive: no hand, no press
        click(win, 730.0f, 300.0f);
        CHECK(ui->get_pressed_b() == 0 && ui->get_pressed_count() == 4);

        // pic-w and pic-h are what the position is measured in: the same picture declared 400 x 200 is scale 0.85
        ui->set_pic_w(400);
        ui->set_pic_h(200);
        click(win, fitX + 86.0f, fitY + 35.0f);
        CHECK(ui->get_pressed_count() == 5 && ui->get_last_px() == 101 && ui->get_last_py() == 41);
        ui->set_pic_w(200);
        ui->set_pic_h(100);
        // not interactive (the plugin has not asked for clicks): no hand, no press
        ui->set_interactive_a(false);
        moveTo(win, 730.0f, 130.0f);
        CHECK(!ui->get_hand_a());
        click(win, 730.0f, 130.0f);
        CHECK(ui->get_pressed_count() == 5);
        ui->set_interactive_a(true);
        // disabled: neither
        ui->set_panes_enabled(false);
        moveTo(win, 730.0f, 130.0f);
        CHECK(!ui->get_hand_a());
        click(win, 730.0f, 130.0f);
        CHECK(ui->get_pressed_count() == 5);
        ui->set_panes_enabled(true);
        away();

        // a portrait picture: 60 x 120 is width-limited... no, height-limited: scale 1.6 -> 96 x 192, centred across the free 340 px at x 682
        ui->set_picture(makePicture(60, 120));
        ui->set_pic_w(60);
        ui->set_pic_h(120);
        const Frame portrait = shot();
        CHECK(portrait.savePng(shotDir() + "output-specimen-portrait.png"));
        CHECK(portrait.near(682 + 2, 100, red, 8) && portrait.near(682 - 3, 100, panel, 0) && portrait.near(682 + 96 - 3, 100, blue, 8) && portrait.near(682 + 96 + 2, 100, panel, 0));
        CHECK(portrait.near(715, 38 + 2, red, 8) && portrait.near(715, 38 + 192 - 3, red, 8) && portrait.near(715, 38 + 192 + 2, panel, 0) && portrait.near(715, 38 - 3, panel, 0));
        click(win, 682.0f + 48.0f, 38.0f + 96.0f);                                         // the centre: 30, 60
        CHECK(ui->get_pressed_count() == 6 && ui->get_last_px() == 30 && ui->get_last_py() == 60);
        click(win, 682.0f + 95.5f, 38.0f + 191.5f);                                        // the last pixel: 59, 119
        CHECK(ui->get_pressed_count() == 7 && ui->get_last_px() == 59 && ui->get_last_py() == 119);
        click(win, 670.0f, 130.0f);                                                        // the margin left of it
        CHECK(ui->get_pressed_count() == 7);
        ui->set_picture(makePicture(200, 100));
        ui->set_pic_w(200);
        ui->set_pic_h(100);

        // without pic-w and pic-h the picture's own size is used
        ui->set_pic_w(0);
        ui->set_pic_h(0);
        click(win, fitX + 86.0f, fitY + 35.0f);
        CHECK(ui->get_pressed_count() == 8 && ui->get_last_px() == 50 && ui->get_last_py() == 20);
        ui->set_pic_w(200);
        ui->set_pic_h(100);

        // the waiting pane takes the first picture: have turns on, the note goes, the picture comes
        ui->set_have_c(true);
        const Frame arrived = shot();
        // 70 high: the status row is 28, the free area 32 x 340, so the 200 x 100 picture is scale 0.32 - 64 x 32, centred at x 698
        CHECK(arrived.countNear(kPx, kPcy, kPx + kPw, kPcy + kPch, red, 30) > 800 && arrived.countNear(kPx, kPcy, kPx + kPw, kPcy + kPch, blue, 30) > 800);
        CHECK(arrived.near(698 + 2, kPcy + 28 + 16, red, 8) && arrived.near(698 + 64 - 3, kPcy + 28 + 16, blue, 8) && arrived.near(698 - 3, kPcy + 28 + 16, panel, 0));
        CHECK(inkIs(arrived, kPx + 10, kPcy + 2, kPx + 150, kPcy + 26, fg));            // the status line (Theme.fg) takes the place of the waiting note
        CHECK(differing(f0, arrived, kPx + 10, kPcy + 2, kPx + 260, kPcy + 26) > 100);
        CHECK(firstRun(arrived, kPcy + 14, kPx + 120, kPx + kPw, keyBg) > kPx + 120);   // and the Save key is there
        ui->set_have_c(false);
        away();

        // the Save as BMP key: A's saves, B's (disabled) does not, a disabled pane's does not
        const int keyA = firstRun(f0, kPay + 14, kPx + 120, kPx + kPw, keyBg);
        const int keyB = firstLongRun(f0, kPby + 4, kPx + 120, kPx + kPw, offBorder, 20, 2);          // pane B's disabled key, by its border
        CHECK(keyA > 0 && keyB > 0);
        CHECK(ui->get_saves_a() == 0 && ui->get_saves_b() == 0);
        click(win, keyA + 14.0f, kPay + 14.0f);
        CHECK(ui->get_saves_a() == 1 && ui->get_pressed_count() == 8);                     // the key is not the picture
        click(win, keyB + 14.0f, kPby + 14.0f);
        CHECK(ui->get_saves_b() == 0);
        click(win, keyA + 14.0f, kPay + 14.0f, slint::PointerEventButton::Right);
        CHECK(ui->get_saves_a() == 1);
        ui->set_panes_enabled(false);
        click(win, keyA + 14.0f, kPay + 14.0f);
        CHECK(ui->get_saves_a() == 1);
        away();
        const Frame offPane = shot();
        CHECK(offPane.near(keyA + 14, kPay + 4, offBorder, 2) && !offPane.near(keyA + 14, kPay + 4, keyLine, 0));   // the enabled key has gone to the disabled look
        CHECK(inkIs(offPane, keyA + 8, kPay + 6, keyA + 70, kPay + 22, keyOffFg, 24));
        ui->set_panes_enabled(true);
        away();
    }

    // ============================================================================================================================
    // INTERACTION: TargetDetails
    // ============================================================================================================================
    {
        section("TargetDetails interaction");
        away();
        const int kx = kTx + kTw - 11 - kKeysW;
        const int aKeyY = 10 + 1 + 8 + 24 + 4;                                              // the first key of A, y 47
        const int bKeyY = 230 + 1 + 8 + 24 + 4;                                             // B, y 267
        const int cKeyY = 366 + 1 + 8 + 24 + 4;                                             // C, y 403
        // A: Go to on map, Follow
        click(win, kx + 60.0f, aKeyY + 12.0f);
        CHECK(ui->get_a_go() == 1 && ui->get_a_follow() == 0);
        click(win, kx + 60.0f, aKeyY + 28.0f + 12.0f);
        CHECK(ui->get_a_go() == 1 && ui->get_a_follow() == 1);
        click(win, kx + 60.0f, aKeyY + 12.0f, slint::PointerEventButton::Right);
        CHECK(ui->get_a_go() == 1);
        moveTo(win, kx + 60.0f, aKeyY + 12.0f);
        CHECK(shot().near(kx + 6, aKeyY + 10, tok(th.get_menu_hot()), 0));
        away();
        // the whole key takes the click, the corners too
        click(win, kx + 2.0f, aKeyY + 2.0f);
        click(win, kx + kKeysW - 3.0f, aKeyY + 21.0f);
        CHECK(ui->get_a_go() == 3);
        // B: Go is disabled (the target has no position), Stop following and Close act
        click(win, kx + 60.0f, bKeyY + 12.0f);
        CHECK(ui->get_b_go() == 0);
        click(win, kx + 60.0f, bKeyY + 28.0f + 12.0f);
        CHECK(ui->get_b_follow() == 1 && ui->get_b_close() == 0);
        click(win, kx + 60.0f, bKeyY + 56.0f + 12.0f);
        CHECK(ui->get_b_close() == 1 && ui->get_b_follow() == 1 && ui->get_b_go() == 0);
        // C: Close only
        click(win, kx + 60.0f, cKeyY + 12.0f);
        CHECK(ui->get_c_close() == 1);
        click(win, kx + 60.0f, cKeyY + 28.0f + 12.0f);                                      // where a second key would be: nothing
        CHECK(ui->get_c_close() == 1 && ui->get_a_go() == 3);
        // F: the keys in a row under the lines (Go to on map, Follow, Close), each acting for itself
        const int fRow = 470 + 1 + 8 + 24 + 4 + 68 + 4 + 2;                                // y 581
        const std::vector<int> fk = runStarts(f0, fRow + 4, 372, 664, keyBg, 8);
        CHECK(fk.size() == 3);
        if (fk.size() == 3) {
            click(win, fk[0] + 8.0f, fRow + 12.0f);
            CHECK(ui->get_f_go() == 1 && ui->get_f_follow() == 0 && ui->get_f_close() == 0);
            click(win, fk[1] + 8.0f, fRow + 12.0f);
            CHECK(ui->get_f_go() == 1 && ui->get_f_follow() == 1 && ui->get_f_close() == 0);
            click(win, fk[2] + 8.0f, fRow + 12.0f);
            CHECK(ui->get_f_go() == 1 && ui->get_f_follow() == 1 && ui->get_f_close() == 1);
        }
        // the lines are not keys: a click on the text does nothing
        click(win, kTx + 60.0f, aKeyY + 30.0f);
        CHECK(ui->get_a_go() == 3 && ui->get_a_follow() == 1);
        // disabled: every key is, and none acts
        ui->set_details_enabled(false);
        click(win, kx + 60.0f, aKeyY + 12.0f);
        click(win, kx + 60.0f, aKeyY + 28.0f + 12.0f);
        click(win, kx + 60.0f, bKeyY + 28.0f + 12.0f);
        click(win, kx + 60.0f, bKeyY + 56.0f + 12.0f);
        CHECK(ui->get_a_go() == 3 && ui->get_a_follow() == 1 && ui->get_b_follow() == 1 && ui->get_b_close() == 1);
        const Frame offDetails = shot();
        CHECK(offDetails.colNear(kx + 6, aKeyY + 1, aKeyY + 23, keyOffBg, 0) && offDetails.colNear(kx + 6, aKeyY + 29, aKeyY + 51, keyOffBg, 0));
        CHECK(offDetails.colNear(kx + 6, bKeyY + 29, bKeyY + 51, keyOffBg, 0) && offDetails.colNear(kx + 6, bKeyY + 57, bKeyY + 79, keyOffBg, 0));
        CHECK(offDetails.near(kx + 30, aKeyY, offBorder, 2) && offDetails.near(kx + 30, aKeyY + 28, offBorder, 2));      // A's two keys, borders at half alpha
        CHECK(offDetails.near(kx + 30, bKeyY + 28, offBorder, 2) && offDetails.near(kx + 30, bKeyY + 56, offBorder, 2));  // B's Stop following and Close too
        CHECK(offDetails.countNear(kx, aKeyY, kx + kKeysW, aKeyY + 52, keyFg, 14) == 0);
        ui->set_details_enabled(true);
        away();
        CHECK(differing(f0, shot(), kTx, 0, kW, 540) == 0);                                 // and the column of target details is as it was
    }

    // ============================================================================================================================
    // the tooltips: a rested pointer shows them below the component
    // ============================================================================================================================
    {
        section("tooltips");
        // LogView A's (its body): the tip sits 6 px under it, y 166, over the gap above LogView B
        const auto tipShown = [&](int x0, int y) { return shot().countNear(x0, y, x0 + 100, y + 1, knobRing, 2) > 20; };
        moveTo(win, 200.0f, 100.0f);
        bool shown = false;
        for (int i = 0; i < 50 && !shown; ++i) { settle(std::chrono::milliseconds(100)); shown = tipShown(kLx + 8, kAy + kAh + 6); }
        CHECK(shown);                                                                        // after about 600 ms of rest
        away();
        CHECK(!tipShown(kLx + 8, kAy + kAh + 6));                                            // and it closes when the pointer leaves
        // the picture pane's (its picture): 6 px under the pane, y 246
        moveTo(win, 730.0f, 130.0f);
        shown = false;
        for (int i = 0; i < 50 && !shown; ++i) { settle(std::chrono::milliseconds(100)); shown = tipShown(kPx + 8, kPay + kPah + 6); }
        CHECK(shown);
        away();
        CHECK(!tipShown(kPx + 8, kPay + kPah + 6));
        // the target details' (over its lines, not over a key): 6 px under panel A, y 225
        moveTo(win, 1000.0f, 100.0f);
        shown = false;
        for (int i = 0; i < 50 && !shown; ++i) { settle(std::chrono::milliseconds(100)); shown = tipShown(kTx + 8, 10 + aH + 6); }
        CHECK(shown);
        away();
        CHECK(!tipShown(kTx + 8, 10 + aH + 6));
    }

    return testSummary("test_slint_output");
}
