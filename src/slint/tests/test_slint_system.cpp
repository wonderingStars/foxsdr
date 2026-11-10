// The SYSTEM bank's bodies (ui/system_bank.slint): DisplayBody, LanguageBody, UpdatesBody, SerialPortsBody with the shared GpsRowView, KeyBindingsBody and its
// key capture, DiagnosticsBody, UsageBody, TesterBody, and the two report pages (ReportSheet, kind 0 and kind 1) - rendered headlessly from
// ui/specimens/system_specimen.slint (every body inside the real Section it is shown under) and probed against the Banks artboard
// (artboards/Workbench-Banks.dc.html: the SYSTEM column, A5, and its closed sections A5b), the Dialogs artboard (the two pages, F1) and specs/SCREENS.md.
//
// What is asserted to the pixel: the rows' pitch (24 px rows, 6 px between: the artboard's .rw and .sbd gap), the dropdowns' widths (150, and the pair that
// share a row 6 px apart), the 14 px boxes and the lit radio, the slider tracks and their fill, the key caps (ONE width, 20 px high, the amber listening cap),
// the table of ports (22 px strip heading, 24 px rows), the tones (green status, amber hint, red clash, red plan error), the sheets' frame. What is asserted
// as ink in a region: every word (the typefaces are the embedded Inter and JetBrains Mono, never glyph positions). Every callback is driven by a real pointer
// or key event and read back through the specimen's `Rec` global (the bodies with an override) or through SystemEvents (the bodies that forward by default).
// The model owns every value: a press on a tick whose model says on asks for "off" and the pixels still show on.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "render_harness.hpp"
#include "test_check.hpp"

#include "system_specimen.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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

Rgb tok(slint::Color c) { return {c.red(), c.green(), c.blue()}; }

struct Ev {
    std::string name, args;
};

// The bounding box and count of the ink (pixels differing from `bg`) in a rectangle.
struct Ink {
    int n = 0, x0 = 1 << 30, y0 = 1 << 30, x1 = -1, y1 = -1;
    int w() const { return x1 - x0 + 1; }
    int h() const { return y1 - y0 + 1; }
};
Ink inkBox(const Frame& f, int x0, int y0, int x1, int y1, Rgb bg, int tol = 24) {
    Ink r;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            if (f.near(x, y, bg, tol)) { continue; }
            ++r.n;
            r.x0 = std::min(r.x0, x); r.y0 = std::min(r.y0, y);
            r.x1 = std::max(r.x1, x); r.y1 = std::max(r.y1, y);
        }
    }
    return r;
}
// The x of the first / last pixel on a row (in [x0,x1)) within tol of c; -1 when none.
int firstX(const Frame& f, int y, int x0, int x1, Rgb c, int tol = 3) {
    for (int x = x0; x < x1; ++x) { if (f.near(x, y, c, tol)) { return x; } }
    return -1;
}
int lastX(const Frame& f, int y, int x0, int x1, Rgb c, int tol = 3) {
    for (int x = x1 - 1; x >= x0; --x) { if (f.near(x, y, c, tol)) { return x; } }
    return -1;
}
// The width of the run of `ground` pixels found going left from xStart on a row, after skipping what is not ground: the gap between two keys.
int gapLeftOf(const Frame& f, int y, int xStart, Rgb ground) {
    int x = xStart;
    while (x > 0 && !f.near(x, y, ground, 2)) { --x; }
    int n = 0;
    while (x > 0 && f.near(x, y, ground, 2)) { --x; ++n; }
    return n;
}
// The rows [top, bottom) that carry ink in the columns [x0, x1) between y0 and y1: the lines of text of a body.
std::vector<std::pair<int, int>> textLines(const Frame& f, int x0, int x1, int y0, int y1, Rgb bg) {
    std::vector<std::pair<int, int>> out;
    bool in = false;
    int start = 0;
    for (int y = y0; y < y1; ++y) {
        const bool ink = f.countInk(x0, y, x1, y + 1, bg) > 0;
        if (ink && !in) { in = true; start = y; }
        if (!ink && in) { in = false; out.push_back({start, y}); }
    }
    if (in) { out.push_back({start, y1}); }
    return out;
}
// A debugging aid: with FOXSDR_SLINT_CROPS set, a region of a frame is saved as its own picture (the specimen is 2340 px high; a crop is readable).
void crop(const Frame& f, const char* name, int x, int y, int w, int h) {
    if (std::getenv("FOXSDR_SLINT_CROPS") == nullptr) { return; }
    Frame c;
    c.width = static_cast<std::uint32_t>(w);
    c.height = static_cast<std::uint32_t>(h);
    c.px.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
    for (int j = 0; j < h; ++j) {
        for (int i = 0; i < w; ++i) {
            if (x + i < static_cast<int>(f.width) && y + j < static_cast<int>(f.height)) { c.px[static_cast<std::size_t>(j) * static_cast<std::size_t>(w) + static_cast<std::size_t>(i)] = f.px[static_cast<std::size_t>(y + j) * f.width + static_cast<std::size_t>(x + i)]; }
        }
    }
    c.savePng(shotDir() + name);
}
using Lines = std::vector<std::pair<int, int>>;
// A line of text by position, from the front (i >= 0) or the back (i < 0 counts from the last: -1 is the last); {-5, -4} (no ink anywhere) when there is none, so a missing line fails its check.
std::pair<int, int> lineAt(const Lines& ls, int i) {
    const long long k = i >= 0 ? i : static_cast<long long>(ls.size()) + i;
    return k >= 0 && k < static_cast<long long>(ls.size()) ? ls[static_cast<std::size_t>(k)] : std::pair<int, int>{-5, -4};
}
double num(const std::string& s) {
    try { return std::stod(s); } catch (...) { return -1e9; }
}

// ---- the specimen's table (system_specimen.slint): columns at x 10 / 316 / 622 / 928, Sections 296 wide, bodies at +30, 254 wide ----
constexpr int kColA = 10, kColB = 316, kColC = 622, kColD = 928;
constexpr int kSecRow = 30, kBodyPad = 4, kBodyPadBottom = 12, kBodyX = 30, kBodyW = 254;
constexpr int kRow = 24, kPitch = 30;          // the artboard's .rw (min-height 24) and the .sbd gap of 6
constexpr int kW = 1240, kH = 2560;

}  // namespace

int main() {
    installHeadlessPlatform();
    auto ui = SystemSpecimen::create();
    slint::Window& win = ui->window();

    const Skin wb = ui->global<Workbench>().get_value();
    const Rgb bg = tok(wb.bg), panel = tok(wb.panel), card = tok(wb.card), field = tok(wb.field), line = tok(wb.line), lineStrong = tok(wb.line_strong);
    const Rgb fg = tok(wb.fg), fgSoft = tok(wb.fg_soft), muted = tok(wb.muted), dim = tok(wb.dim), accent = tok(wb.accent), accentSoft = tok(wb.accent_soft);
    const Rgb errFg = tok(wb.chip_err_fg), warnFg = tok(wb.chip_warn_fg), goodFg = tok(wb.chip_good_fg);
    const Rgb chipBg = tok(wb.chip_bg), chipLine = tok(wb.chip_line), chipFg = tok(wb.chip_fg), warnBg = tok(wb.chip_warn_bg), warnLine = tok(wb.chip_warn_line);
    const Rgb keyBg = tok(wb.key_bg), keyLine = tok(wb.key_line), menu = tok(wb.menu), menuHot = tok(wb.menu_hot), strip = tok(wb.strip);
    const Rgb sliderTrack = tok(wb.slider_track), sliderFill = tok(wb.slider_fill), sliderKnob = tok(wb.slider_knob);
    const Rgb lampRed = tok(wb.lamp_red), knobRing = tok(wb.knob_ring), disabledInk = tok(wb.disabled);
    const Rgb dis = over(keyLine, 0.5, panel);                      // a disabled key: the ground is the panel's, the border the key's at half alpha
    // the artboard's own values, once, so a drift of the Workbench filling is caught here
    CHECK(nearColour(panel, rgb(0x1d2026), 0) && nearColour(chipLine, rgb(0x363b45), 0) && nearColour(accent, rgb(0xf5a524), 0));
    CHECK(nearColour(errFg, rgb(0xff9b9b), 0) && nearColour(warnFg, rgb(0xf5c66b), 0) && nearColour(goodFg, rgb(0x7fe6ad), 0));

    std::vector<Ev> evs;
    ui->global<Rec>().on_fired([&](slint::SharedString n, slint::SharedString a) { evs.push_back({std::string(n), std::string(a)}); });
    std::vector<Ev> fwd;   // what the default-forwarding bodies send to SystemEvents
    auto& se = ui->global<SystemEvents>();
    se.on_display_theme([&](int i) { fwd.push_back({"display-theme", std::to_string(i)}); });
    se.on_display_tuner_switches([&](bool v) { fwd.push_back({"display-tuner-switches", v ? "1" : "0"}); });
    se.on_display_db_max([&](float v) { fwd.push_back({"display-db-max", std::to_string(v)}); });
    se.on_serial_refresh([&] { fwd.push_back({"serial-refresh", ""}); });
    se.on_gps_toggle([&] { fwd.push_back({"gps-toggle", ""}); });
    se.on_keys_capture([&](slint::SharedString id) { fwd.push_back({"keys-capture", std::string(id)}); });
    se.on_keys_reset([&] { fwd.push_back({"keys-reset", ""}); });

    const auto clk = [&](int x, int y) { click(win, static_cast<float>(x), static_cast<float>(y)); };
    const auto mv = [&](int x, int y) { moveTo(win, static_cast<float>(x), static_cast<float>(y)); };
    const auto tap = [&](std::u8string_view k) { keyTap(win, k); };
    const auto shot = [&] { return render(ui, kW, kH); };
    const auto park = [&] { mv(1235, 2555); settle(milliseconds(30)); };
    // click a spot and report the last event it fired ("" when none)
    const auto press = [&](int x, int y) { evs.clear(); fwd.clear(); clk(x, y); shot(); return evs.empty() ? Ev{} : evs.back(); };
    const auto yOf = [&](double v) { return static_cast<int>(std::lround(v)); };
    // the list a dropdown opens under a key at (keyX, keyY): row i's middle (a list with a filter field above it starts 30 px lower)
    const auto popupRow = [&](int keyX, int keyY, int i, int head = 0) { return std::pair<int, int>{keyX + 30, keyY + kRow + 2 + 5 + head + i * 26 + 13}; };
    // a dropdown of width w whose right edge is the body's: its border pixels
    const auto dropAt = [&](const Frame& f, int y, int right, int w) {
        return f.near(right - 1, y + 12, keyLine) && f.near(right - w, y + 12, keyLine) && !f.near(right - w - 1, y + 12, keyLine) && !f.near(right + 1, y + 12, keyLine)
            && f.near(right - 40, y, keyLine) && f.near(right - 40, y + 23, keyLine) && !f.near(right - 40, y - 1, keyLine) && !f.near(right - 40, y + 24, keyLine);
    };
    // the top border row of a 14 px checkbox whose left edge is x, searched in [y0, y1): ticked (the accent box) or empty (the field ground in a line-strong border); -1 when none
    const auto findBox = [&](const Frame& f, int x, int y0, int y1, bool ticked) {
        for (int y = y0; y < y1; ++y) {
            if (f.near(x + 7, y - 1, panel, 0) && (ticked ? (f.near(x + 7, y, accent, 0) && f.near(x + 7, y + 5, accent, 0)) : (f.near(x + 7, y, lineStrong, 0) && f.near(x + 7, y + 4, field, 0)))) { return y; }
        }
        return -1;
    };
    // the top of the first 24 px dropdown in [y0, y1) whose right edge is `right` (a key border above and below, a gap outside)
    const auto dropTop = [&](const Frame& f, int right, int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            if (f.near(right - 40, y, keyLine, 0) && f.near(right - 40, y + 23, keyLine, 0) && !f.near(right - 40, y - 1, keyLine, 0) && !f.near(right - 40, y + 24, keyLine, 0)) { return y; }
        }
        return -1;
    };
    // the top border row of a disabled key spanning [x, x + w): the half-alpha border at five places along one row (a glyph's edge never matches them all)
    const auto disabledKeyTop = [&](const Frame& f, int x, int w, int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            bool all = true;
            for (const int dx : {w / 6, w / 3, w / 2, 2 * w / 3, 5 * w / 6}) { all = all && f.near(x + dx, y, dis, 4); }
            if (all) { return y; }
        }
        return -1;
    };
    // a slider row (label 84, track, readout 56): the track's start and end on the row, the knob's centre colour
    struct Track { int start, end; };
    const auto trackOf = [&](const Frame& f, int rowTop, int x0) {
        Track t{-1, -1};
        for (const int dy : {13, 14, 15}) {                      // the three rows of the 6 px track the knob's round edge leaves alone at its ends
            for (int x = x0; x < x0 + kBodyW; ++x) { if (f.near(x, rowTop + dy, sliderTrack, 0) || f.near(x, rowTop + dy, sliderFill, 0)) { if (t.start < 0 || x < t.start) { t.start = x; } t.end = std::max(t.end, x); } }
        }
        return t;
    };

    // =========================================================================================================
    // 1. The sheet at rest
    // =========================================================================================================
    park();
    const Frame f0 = shot();
    CHECK(f0.width == kW && f0.height == kH);
    CHECK(f0.near(1235, 2555, bg, 0));
    f0.savePng(shotDir() + "system-specimen.png");
    crop(f0, "crop-q1.png", 0, 0, 620, 900);
    crop(f0, "crop-q2.png", 610, 0, 630, 900);
    crop(f0, "crop-q3.png", 0, 880, 620, 800);
    crop(f0, "crop-q4.png", 610, 880, 630, 800);

    // =========================================================================================================
    // 2. Display (a1, the artboard's A5 first section): the rows at the sheet's pitch, the controls at the sheet's sizes
    // =========================================================================================================
    const int a1y = yOf(ui->get_a1_y());
    const int x0 = kColA + kBodyX, R = x0 + kBodyW;                  // the body's left and right edge (exclusive)
    const int T = a1y + kSecRow + kBodyPad;                          // the first row's top
    const auto rowY = [&](int k) { return T + k * kPitch; };
    {
        CHECK_NEAR(ui->get_a1_y(), 10.0, 0.01);
        CHECK(f0.near(x0 + 3, T - 2, panel, 0));
        CHECK(f0.near(x0 - 2, T + 40, panel, 0) && f0.near(R + 4, T + 40, panel, 0));
        // rows 0, 1, 4, 5: a label at the left and a 150 px dropdown flush right, 24 high
        for (const int k : {0, 1, 4, 5}) {
            CHECKV(dropAt(f0, rowY(k), R, 150), "row %d: a 150 px dropdown flush right, 24 high", k);
            CHECKV(f0.countNear(x0, rowY(k) + 5, x0 + 120, rowY(k) + 19, fgSoft, 60) >= 25, "row %d: its label", k);
            CHECKV(f0.countNear(R - 140, rowY(k) + 5, R - 22, rowY(k) + 19, fg, 60) >= 25, "row %d: the closed text", k);
        }
        // rows 2, 3, 8, 9: Min dB, Max dB, Aircraft icons, Trail width - the label 84 wide, the 6 px track with its fill to the model's fraction, the knob, the readout flush right
        const double fracs[4] = {0.357, 0.833, 0.4, 0.1};
        int fi = 0;
        const Track common = trackOf(f0, rowY(3), x0);
        for (const int k : {2, 3, 8, 9}) {
            const int ty = rowY(k) + 12, len = common.end - common.start + 1;
            CHECKV(common.start >= x0 + 92 && common.start <= x0 + 98, "the track starts at %d (84 label + 8 of air + the knob's overhang)", common.start);
            CHECKV(common.end >= R - 64 - 6 && common.end <= R - 64, "the track ends at %d (56 readout + 8 of air before the body's edge %d)", common.end, R);
            CHECK(f0.near(x0 + 140, ty, sliderTrack, 0) || f0.near(x0 + 140, ty, sliderFill, 0) || f0.near(x0 + 140, ty, sliderKnob, 6));
            CHECKV(f0.near(common.start + 60, ty - 4, panel, 0) && f0.near(common.start + 60, ty + 3, panel, 0) && (f0.near(common.start + 60, ty - 3, sliderTrack, 0) || f0.near(common.start + 60, ty - 3, sliderFill, 0)) && (f0.near(common.start + 60, ty + 2, sliderTrack, 0) || f0.near(common.start + 60, ty + 2, sliderFill, 0)),
                   "row %d: the track is 6 px high", k);
            const int kx = common.start + static_cast<int>(std::lround(fracs[fi] * len));
            CHECKV(f0.near(kx, ty, sliderKnob, 6), "row %d: the knob at %d (fraction %.3f)", k, kx, fracs[fi]);
            const Ink rd = inkBox(f0, R - 60, rowY(k) + 2, R + 6, rowY(k) + 22, panel);
            CHECKV(rd.n >= 6 && rd.x1 >= R - 3 && rd.x1 <= R, "row %d: the readout ends at %d (the body ends at %d)", k, rd.x1, R);
            CHECK(f0.countNear(x0, rowY(k) + 5, x0 + 80, rowY(k) + 19, fgSoft, 60) >= 15);
            ++fi;
        }
        // the Min dB fill is shorter than the Max dB fill
        CHECK(f0.near(x0 + 92 + 60, rowY(2) + 12, sliderTrack, 0) && f0.near(x0 + 92 + 60, rowY(3) + 12, sliderFill, 0));
        // rows 6, 7: Tuner switches (ticked: accent box) and Enlarge every reading (not: the field ground in a line-strong border); 14 px boxes
        CHECKV(f0.countNear(x0, rowY(6) + 5, x0 + 14, rowY(6) + 19, accent, 0) >= 100, "Tuner switches is ticked");
        const int by = rowY(7) + (kRow - 14) / 2;
        CHECKV(f0.near(x0, by + 7, lineStrong) && f0.near(x0 + 13, by + 7, lineStrong) && f0.near(x0 + 3, by + 3, field, 0) && f0.near(x0 + 14, by + 7, panel, 0), "Enlarge every reading is a 14 px empty box");
        CHECK(f0.countNear(x0 + 22, rowY(7) + 4, x0 + 190, rowY(7) + 20, fg, 60) >= 40);
        // row 10: Trail style, a 150 px dropdown flush right under the Trail width slider (docs 6.52)
        CHECKV(dropAt(f0, rowY(10), R, 150), "Trail style: a 150 px dropdown");
        CHECK(f0.countNear(x0, rowY(10) + 5, x0 + 80, rowY(10) + 19, fgSoft, 60) >= 15);
        // row 11: the Band plan tick (ticked); row 12: Size and Colour share the line, 6 px apart, each half of it
        CHECKV(f0.countNear(x0, rowY(11) + 5, x0 + 14, rowY(11) + 19, accent, 0) >= 100, "Band plan is ticked");
        CHECKV(f0.near(x0, rowY(12) + 12, keyLine) && f0.near(R - 1, rowY(12) + 12, keyLine) && f0.near(x0, rowY(12), panel, 0), "Size starts at the left edge, Colour ends at the right");
        int sizeEnd = -1, colourStart = -1;
        for (int x = x0 + 40; x < R - 40; ++x) {
            if (f0.near(x, rowY(12) + 3, panel, 0) && sizeEnd < 0) { sizeEnd = x; }
            if (sizeEnd >= 0 && colourStart < 0 && !f0.near(x, rowY(12) + 3, panel, 0)) { colourStart = x; }
        }
        CHECKV(sizeEnd - x0 == 124 && colourStart - sizeEnd == 6 && R - colourStart == 124, "Size %d wide, a gap of %d, Colour %d wide", sizeEnd - x0, colourStart - sizeEnd, R - colourStart);
        // row 13: Region (150), row 14: the plan status in the good tone
        CHECKV(dropAt(f0, rowY(13), R, 150), "Region: a 150 px dropdown");
        CHECKV(f0.countNear(x0, rowY(14), R, rowY(14) + 14, goodFg, 40) >= 60, "the plan status is green: %d", f0.countNear(x0, rowY(14), R, rowY(14) + 14, goodFg, 40));
        // the second Display (a2): the error variant - Tuner switches unticked, Enlarge ticked, icons at the top, the status red, and no card
        const int T2 = yOf(ui->get_a2_y()) + kSecRow + kBodyPad;
        CHECKV(f0.countNear(x0, T2 + 6 * kPitch + 5, x0 + 14, T2 + 6 * kPitch + 19, accent, 0) == 0, "a2: Tuner switches is not ticked");
        CHECKV(f0.countNear(x0, T2 + 7 * kPitch + 5, x0 + 14, T2 + 7 * kPitch + 19, accent, 0) >= 100, "a2: Enlarge every reading is ticked");
        const int clen = common.end - common.start + 1;
        CHECKV(f0.near(common.start + clen, T2 + 8 * kPitch + 12, sliderKnob, 6), "a2: the icon knob is at the end of the track");
        CHECKV(f0.near(common.start, T2 + 2 * kPitch + 12, sliderKnob, 6), "a2: the Min dB knob is at the start");
        CHECKV(f0.countNear(x0, T2 + 14 * kPitch, R, T2 + 14 * kPitch + 30, errFg, 40) >= 100, "a2: the plan error is red: %d", f0.countNear(x0, T2 + 14 * kPitch, R, T2 + 14 * kPitch + 30, errFg, 40));
        CHECK(f0.countNear(x0, T2 + 14 * kPitch, R, T2 + 14 * kPitch + 30, goodFg, 40) == 0);
        CHECK(f0.countNear(x0, T2, R, T2 + 440, card, 0) == 0);
    }

    // =========================================================================================================
    // 3. Language & country (b1)
    // =========================================================================================================
    const int xb = kColB + kBodyX, Rb = xb + kBodyW;
    const int Tb = yOf(ui->get_b1_y()) + kSecRow + kBodyPad;
    {
        CHECKV(dropAt(f0, Tb, Rb, 150), "Language: a 150 px dropdown");
        CHECKV(f0.countNear(xb, Tb + 5, xb + 70, Tb + 19, fgSoft, 60) >= 25 && f0.countNear(Rb - 140, Tb + 5, Rb - 22, Tb + 19, fg, 60) >= 25, "Language: label and closed text");
        // the machine hint, amber, one line; SUGGEST A BETTER TRANSLATION, a small key across the body
        CHECKV(f0.countNear(xb, Tb + 30, Rb, Tb + 44, warnFg, 40) >= 80, "the machine hint is amber: %d", f0.countNear(xb, Tb + 30, Rb, Tb + 44, warnFg, 40));
        const int sy = Tb + 30 + 14 + 6;
        CHECKV(f0.near(xb, sy + 12, keyLine) && f0.near(Rb - 1, sy + 12, keyLine) && f0.near(xb + 100, sy, keyLine) && f0.near(xb + 100, sy + 23, keyLine) && !f0.near(xb + 100, sy + 24, keyLine), "SUGGEST: a 24 px key across the body");
        CHECK(f0.countNear(xb + 20, sy + 5, Rb - 20, sy + 19, tok(wb.key_fg), 60) >= 80);
        // Country, then the band-plan line (fg-soft), the override hint (dim), USE DEUTSCH
        const int cy = sy + 24 + 6;
        CHECKV(dropAt(f0, cy, Rb, 150), "Country: a 150 px dropdown");
        CHECKV(f0.countNear(xb, cy + 30, Rb, cy + 44, fgSoft, 40) >= 50, "the band-plan line in fg-soft");
        CHECKV(f0.countNear(xb, cy + 50, Rb, cy + 64, dim, 30) >= 50, "the override hint in dim");
        const int uy = cy + 30 + 14 + 6 + 14 + 6;
        CHECKV(f0.near(xb, uy + 12, keyLine) && f0.near(Rb - 1, uy + 12, keyLine), "USE DEUTSCH: a key across the body");
        CHECKV(yOf(ui->get_b1_body_h()) == kBodyPad + (uy + 24 - Tb) + kBodyPadBottom, "the body is %d high", yOf(ui->get_b1_body_h()));
    }

    // =========================================================================================================
    // 4. Updates (b2: up to date, b3: update available)
    // =========================================================================================================
    const int T2b = yOf(ui->get_b2_y()) + kSecRow + kBodyPad, T3b = yOf(ui->get_b3_y()) + kSecRow + kBodyPad;
    {
        CHECKV(f0.countNear(xb, T2b + 5, xb + 14, T2b + 19, accent, 0) >= 100, "Check for updates is ticked");
        // the two radios, one per line: 14 px rings, the first lit (accent) on b2, the second on b3
        const auto ring = [&](const Frame& f, int top) { return f.countNear(xb, top + 5, xb + 14, top + 19, accent, 0); };
        CHECKV(ring(f0, T2b + kPitch) >= 60 && ring(f0, T2b + 2 * kPitch) == 0, "b2: Stable is lit, Every release is not (%d / %d)", ring(f0, T2b + kPitch), ring(f0, T2b + 2 * kPitch));
        CHECKV(ring(f0, T3b + kPitch) == 0 && ring(f0, T3b + 2 * kPitch) >= 60, "b3: Every release is lit (%d / %d)", ring(f0, T3b + kPitch), ring(f0, T3b + 2 * kPitch));
        CHECK(f0.near(xb, T2b + 2 * kPitch + 12, lineStrong) && f0.near(xb + 4, T2b + 2 * kPitch + 12, field, 0));
        // the notes: the help, the privacy text and the version line, then the team message (accent-soft: it presses) and the green status
        const auto ls = textLines(f0, xb, Rb, T2b + 3 * kPitch, T2b + yOf(ui->get_b2_body_h()) - kBodyPadBottom, panel);
        CHECKV(ls.size() >= 9, "b2: %zu lines of notes", ls.size());
        const auto status = lineAt(ls, -1), team = lineAt(ls, -2), version = lineAt(ls, -3);
        CHECKV(f0.countNear(xb, status.first, Rb, status.second, goodFg, 40) >= 50, "b2: the status is green");
        CHECKV(f0.countNear(xb, team.first, Rb, team.second, accentSoft, 40) >= 50, "b2: the team message is accent-soft");
        CHECKV(f0.countNear(xb, version.first, Rb, version.second, fgSoft, 40) >= 30, "b2: the version line is fg-soft");
        CHECKV(f0.countNear(xb, lineAt(ls, 0).first, Rb, lineAt(ls, 0).second, dim, 30) >= 50, "b2: the channel help is dim");
        // b3: the status amber, then Show it again, a small key
        const int b3bottom = T3b + yOf(ui->get_b3_body_h()) - kBodyPadBottom - kBodyPad;
        const int kyp = b3bottom - 24;
        CHECKV(f0.near(xb + 20, kyp, keyLine) && f0.near(xb + 20, kyp + 23, keyLine) && f0.near(xb, kyp + 12, keyLine), "Show it again: a 24 px key at the left");
        CHECKV(f0.countNear(xb, kyp - 20, Rb, kyp - 6, warnFg, 40) >= 80, "b3: the status is amber");
        CHECK(f0.countNear(xb + 100, kyp, Rb, kyp + 24, keyLine, 0) == 0);                                                   // the key hugs its label
    }

    // =========================================================================================================
    // 5. Serial ports (b4: two ports, the GPS listening; b5: none)
    // =========================================================================================================
    const int T4b = yOf(ui->get_b4_y()) + kSecRow + kBodyPad, T5b = yOf(ui->get_b5_y()) + kSecRow + kBodyPad;
    {
        const auto ls = textLines(f0, xb, Rb, T4b, T4b + 40, panel);
        CHECKV(f0.countNear(xb, lineAt(ls, 0).first, Rb, lineAt(ls, 0).second, dim, 30) >= 80, "the intro is dim");
        // the intro is two lines (14 + 14 + 6 = 34 from the body top), then Refresh, a small key at the left
        int ky = -1;
        for (int y = T4b + 20; y < T4b + 80 && ky < 0; ++y) { if (f0.near(xb + 20, y, keyLine, 0)) { ky = y; } }
        CHECKV(ky > 0 && f0.near(xb, ky + 12, keyLine) && f0.near(xb + 20, ky + 23, keyLine) && !f0.near(xb + 20, ky + 24, keyLine), "Refresh: a 24 px key at %d", ky);
        // the table: a 22 px heading on the strip ground with a line-strong rule under it, two 24 px rows with a hairline under each
        const int hy = ky + 24 + 6;
        CHECKV(f0.near(xb + 100, hy + 2, strip, 0) && f0.near(xb + 100, hy + 20, strip, 0) && f0.near(xb + 100, hy + 21, lineStrong, 0) && f0.near(xb + 100, hy + 22, panel, 0), "the heading row is 22 high on the strip ground");
        CHECK(f0.countNear(xb + 8, hy + 5, xb + 60, hy + 17, muted, 40) >= 15);
        CHECKV(f0.near(xb + 100, hy + 22 + 23, line, 0) && f0.near(xb + 100, hy + 22 + 24 + 23, line, 0) && f0.near(xb + 100, hy + 22 + 24 + 24, panel, 0), "two rows of 24 with a hairline under each");
        CHECKV(f0.countNear(xb + 8, hy + 22 + 5, xb + 70, hy + 22 + 19, fg, 60) >= 20 && f0.countNear(xb + 8, hy + 22 + 29, xb + 70, hy + 22 + 43, fg, 60) >= 20, "COM3 and COM7 in mono ink");
        // the GPS receiver rule and caption, then the dropdown pair (port, then baud 84 wide, 8 apart), the lit Stop key, status amber, satellites dim, note dim
        const int gy = hy + 22 + 48 + 6;
        CHECKV(f0.near(xb + 100, gy, lineStrong, 0), "the GPS receiver rule");
        CHECK(f0.countNear(xb, gy + 5, xb + 110, gy + 17, muted, 40) >= 30);
        const int py = dropTop(f0, Rb, gy + 10, gy + 80);
        CHECKV(py > 0 && f0.near(Rb - 1, py + 12, keyLine) && f0.near(Rb - 84, py + 12, keyLine) && !f0.near(Rb - 85, py + 12, keyLine), "the baud dropdown is 84 wide, flush right");
        CHECKV(f0.near(xb, py + 12, keyLine) && gapLeftOf(f0, py + 3, Rb - 84 - 1, panel) == 8, "the port dropdown starts at the left, 8 px from the baud one");
        CHECKV(f0.near(xb + 100, py + 30 + 3, accent, 0) && f0.near(xb, py + 30 + 12, accent, 0), "Stop is a lit key across the body");
        CHECKV(f0.countNear(xb, py + 60, Rb, py + 74, warnFg, 40) >= 50, "the GPS status is amber");
        // b5: no ports - the empty line instead of the table, the GPS row at rest (an empty dropdown, the plain key)
        const auto l5 = textLines(f0, xb, Rb, T5b, T5b + 120, panel);
        CHECKV(l5.size() >= 3, "b5: %zu lines", l5.size());
        CHECKV(f0.countNear(xb, T5b + 34 + 30, Rb, T5b + 34 + 46, dim, 30) >= 60, "b5: the empty line, dim");
        CHECK(f0.countNear(xb, T5b, Rb, T5b + 190, strip, 0) == 0);
    }

    // =========================================================================================================
    // 6. Key bindings (c1: four rows - one listening, one clashing - then Mode; c2: plain)
    // =========================================================================================================
    const int xc = kColC + kBodyX, Rc = xc + kBodyW;
    const int Tc = yOf(ui->get_c1_y()) + kSecRow + kBodyPad;
    int capTop[6] = {};
    int c1hy = 0, c2hy = 0;
    {
        // the help (dim, wrapped), then the Receiver heading line with Reset to defaults (xs, 20 high) at the right
        const int rkTop = f0.firstRowNear(Rc - 40, Tc + 20, Tc + 120, keyLine, 0);
        const int hy = rkTop - 2;                                                          // the heading line's top (24 high, the xs key centred in it)
        CHECKV(rkTop > 0 && f0.near(Rc - 1, hy + 12, keyLine) && f0.near(Rc - 40, rkTop + 19, keyLine) && !f0.near(Rc - 40, rkTop + 20, keyLine), "Reset to defaults: an xs key (20 high) flush right (top %d)", rkTop);
        CHECKV(f0.countNear(xc, Tc, Rc, hy - 4, dim, 30) >= 80, "the help is dim");
        CHECKV(f0.countNear(xc, hy + 6, xc + 80, hy + 18, muted, 40) >= 25, "RECEIVER, the cap");
        // the rows: 24 px, 6 apart; the cap is 112 wide on every one and 20 high; the first group has four rows, the clash note is red under the fourth
        c1hy = hy;
        const int r0 = hy + 24 + 6;
        const int rows[4] = {r0, r0 + 30, r0 + 60, r0 + 90};
        const auto capEdge = [&](const Frame& f, int top, Rgb c) {
            return f.near(Rc - 1, top + 12, c) && f.near(Rc - 112, top + 12, c) && !f.near(Rc - 113, top + 12, c);
        };
        for (int i = 0; i < 4; ++i) {
            CHECKV(capEdge(f0, rows[i], i == 2 ? warnLine : chipLine), "row %d: the cap is 112 wide (right edge %d)", i, Rc);
            CHECKV(f0.countInk(xc, rows[i] + 5, xc + 130, rows[i] + 19, panel) >= 40, "row %d: the action's name", i);
            capTop[i] = rows[i] + 2;
        }
        // the cap: 20 high (border to border), the pill's colours, the chord in the chip's ink
        CHECKV(f0.near(Rc - 60, rows[0] + 2, chipLine) && f0.near(Rc - 60, rows[0] + 21, chipLine) && !f0.near(Rc - 60, rows[0] + 1, chipLine) && !f0.near(Rc - 60, rows[0] + 22, chipLine), "the cap is 20 high");
        CHECK(f0.near(Rc - 104, rows[0] + 12, chipBg, 0));
        CHECKV(f0.countNear(Rc - 108, rows[0] + 5, Rc - 4, rows[0] + 19, chipFg, 50) >= 20, "Ctrl+F2 in the chip's ink");
        // the listening cap (row 2): the amber pill, "Press a key..." in the amber ink
        CHECKV(f0.near(Rc - 104, rows[2] + 12, warnBg, 0) && f0.near(Rc - 60, rows[2] + 2, warnLine) && f0.near(Rc - 60, rows[2] + 21, warnLine), "the listening cap is the amber pill");
        CHECKV(f0.countNear(Rc - 108, rows[2] + 5, Rc - 4, rows[2] + 19, warnFg, 50) >= 25, "Press a key... in the amber ink: %d", f0.countNear(Rc - 108, rows[2] + 5, Rc - 4, rows[2] + 19, warnFg, 50));
        CHECK(f0.near(Rc - 104, rows[1] + 12, chipBg, 0) && f0.near(Rc - 104, rows[3] + 12, chipBg, 0));
        // the clash note under row 3: red, 6 px under the row
        CHECKV(f0.countNear(xc, rows[3] + 24 + 6, Rc, rows[3] + 24 + 6 + 14, errFg, 40) >= 80, "the clash note is red");
        // the next group: a thin rule over the heading MODE, then two rows of the same cap width, then the footer in dim
        const int rule = rows[3] + 24 + 6 + 14 + 6;
        CHECKV(f0.near(xc + 100, rule, lineStrong, 0) && f0.near(xc, rule, lineStrong, 0) && f0.near(Rc - 1, rule, lineStrong, 0), "the rule over the Mode heading spans the body");
        CHECK(f0.countNear(xc, rule + 5, xc + 60, rule + 17, muted, 40) >= 15);
        const int m0 = rule + 1 + 4 + 13 + 6;
        for (int i = 0; i < 2; ++i) {
            CHECKV(capEdge(f0, m0 + i * 30, chipLine), "Mode row %d: the same 112 px cap", i);
        }
        capTop[4] = m0 + 2; capTop[5] = m0 + 32;
        CHECKV(f0.countNear(xc, m0 + 60, Rc, m0 + 74, dim, 30) >= 40, "the footer is dim");
        // c2: the plain body, one group, two rows
        const int T2c = yOf(ui->get_c2_y()) + kSecRow + kBodyPad;
        const int rk2 = f0.firstRowNear(Rc - 40, T2c + 8, T2c + 60, keyLine, 0);
        CHECKV(rk2 > 0 && capEdge(f0, rk2 - 2 + 30, chipLine), "c2: the same cap width in a plain body (reset key top %d)", rk2);
        c2hy = rk2 - 2;
    }

    // =========================================================================================================
    // 7. Diagnostics (d1), Usage reporting (d2, d3), Beta tester (d4, d5)
    // =========================================================================================================
    const int xd = kColD + kBodyX, Rd = xd + kBodyW;
    const int Td1 = yOf(ui->get_d1_y()) + kSecRow + kBodyPad, Td2 = yOf(ui->get_d2_y()) + kSecRow + kBodyPad;
    const int Td3 = yOf(ui->get_d3_y()) + kSecRow + kBodyPad, Td4 = yOf(ui->get_d4_y()) + kSecRow + kBodyPad, Td5 = yOf(ui->get_d5_y()) + kSecRow + kBodyPad;
    {
        const Lines ls = textLines(f0, xd, Rd, Td1, Td1 + yOf(ui->get_d1_body_h()) - kBodyPadBottom - kBodyPad, panel);
        CHECKV(ls.size() >= 14, "d1: %zu lines of body", ls.size());
        CHECKV(f0.countNear(xd, lineAt(ls, 0).first, Rd, lineAt(ls, 0).second, warnFg, 40) >= 50, "d1: the offer line is amber");
        CHECK(f0.countNear(xd, lineAt(ls, 1).first, Rd, lineAt(ls, 1).second, dim, 30) >= 50);                            // the explanation
        // the tick (ticked, the label wraps to two lines: the box is centred in the row), the disclosure key (xs, 20 high, left)
        int box = -1;
        for (int y = Td1 + 20; y < Td1 + 140 && box < 0; ++y) { if (f0.near(xd + 7, y, accent, 0) && f0.near(xd + 7, y - 1, panel, 0)) { box = y; } }
        CHECKV(box > 0 && f0.countNear(xd, box, xd + 14, box + 14, accent, 0) >= 100, "the Record tick is ticked (box top %d)", box);
        int xs = -1;
        for (int y = box + 14; y < box + 200 && xs < 0; ++y) { if (f0.near(xd + 40, y, keyLine, 0)) { xs = y; } }
        CHECKV(xs > 0 && f0.near(xd + 40, xs + 19, keyLine, 0) && !f0.near(xd + 40, xs + 20, keyLine, 0) && f0.near(xd, xs + 10, keyLine), "What exactly is in a report?: an xs key (20 high) at the left (top %d)", xs);
        // the paths are mono and dim, wrapped anywhere
        CHECKV(f0.countNear(xd, Td1 + 200, Rd, Td1 + 330, dim, 30) >= 120, "the log and reports paths are dim mono");
        // the SDRplay block at the foot: a hairline, the offer, the Run key
        const int bottom = Td1 + yOf(ui->get_d1_body_h()) - kBodyPadBottom - kBodyPad;
        CHECKV(f0.near(xd + 40, bottom - 24, keyLine, 0) && f0.near(xd + 40, bottom - 1, keyLine, 0), "Run SDRplay diagnostic: a 24 px key ends the body (%d)", bottom);
        CHECK(f0.countNear(xd, bottom - 24 - 6 - 56, Rd, bottom - 24 - 6, dim, 30) >= 100);                           // the offer paragraph
        // Usage (d2): a payload list on the field ground with a 1 px line border; d3: the off line and no box
        CHECKV(f0.countNear(xd, Td2, Rd, Td2 + yOf(ui->get_d2_body_h()), field, 0) >= 3000, "d2: the payload box on the field ground");
        CHECKV(f0.countNear(xd, Td3, Rd, Td3 + yOf(ui->get_d3_body_h()), field, 0) < 400, "d3: no payload box (only the tick's box)");
        // Tester (d4 linked): the Linked line and Unlink; the preview box; d5: the red invalid line, the masked field, SHOW, the key across
        const int linked = Td4 + 28 + 6;
        CHECKV(f0.near(Rd - 1, linked + 12, keyLine) || f0.near(Rd - 1, linked + 14, keyLine) || f0.near(Rd - 1, linked + 8, keyLine), "d4: Unlink at the right edge");
        CHECKV(f0.countNear(xd, Td5, Rd, Td5 + 200, errFg, 40) >= 80, "d5: the invalid-code line is red");
    }

    // =========================================================================================================
    // 8. Rule 11: no body asks for more than the column
    // =========================================================================================================
    CHECKV(ui->get_p1_w() <= 254.01 && ui->get_p2_w() <= 254.01 && ui->get_p3_w() <= 254.01 && ui->get_p4_w() <= 254.01 && ui->get_p5_w() <= 254.01
               && ui->get_p6_w() <= 254.01 && ui->get_p7_w() <= 254.01 && ui->get_p8_w() <= 254.01 && ui->get_p9_w() <= 254.01,
           "bare bodies in a 254 px cell: %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f", ui->get_p1_w(), ui->get_p2_w(), ui->get_p3_w(), ui->get_p4_w(), ui->get_p5_w(),
           ui->get_p6_w(), ui->get_p7_w(), ui->get_p8_w(), ui->get_p9_w());
    CHECKV(ui->get_probe_y() >= kH, "the probes sit below the window (%.0f >= %d)", ui->get_probe_y(), kH);
    CHECKV(ui->get_pages_y() + 820 <= kH, "the pages' stages end inside the window (%.0f + 820 <= %d)", ui->get_pages_y(), kH);

    // =========================================================================================================
    // 9. Display: every callback through a real pointer event
    // =========================================================================================================
    park();
    {
        shot();
        // the Theme dropdown: a click opens the list under the key (26 px rows), row 3 picks index 3; the chosen row (0) carries the tick
        clk(R - 60, rowY(1) + 12);
        const Frame o = shot();
        CHECKV(o.near(R + 30, rowY(1) + 40, menu, 0) && o.near(R - 150, rowY(1) + 60, knobRing, 3), "the Theme list opens under the key (menu ground, knob-ring border): %s %s", hex(o.at(R + 30, rowY(1) + 40)).c_str(), hex(o.at(R - 150, rowY(1) + 60)).c_str());
        CHECKV(o.countNear(R - 150 + 5, rowY(1) + kRow + 2 + 5 + 2, R - 150 + 22, rowY(1) + kRow + 2 + 5 + 24, accent, 30) >= 6, "the chosen row has the tick");
        auto pr = popupRow(R - 150, rowY(1), 3);
        Ev e = press(pr.first, pr.second);
        CHECKV(e.name == "display-theme" && e.args == "3", "Theme row 3: '%s' '%s'", e.name.c_str(), e.args.c_str());
        park();
        clk(R - 60, rowY(0) + 12);
        shot();
        pr = popupRow(R - 150, rowY(0), 2);
        e = press(pr.first, pr.second);
        CHECKV(e.name == "display-scale" && e.args == "2", "Interface size row 2: '%s' '%s'", e.name.c_str(), e.args.c_str());
        park();
        clk(R - 60, rowY(4) + 12);
        shot();
        pr = popupRow(R - 150, rowY(4), 1);
        e = press(pr.first, pr.second);
        CHECKV(e.name == "display-freq-display" && e.args == "1", "Frequency display row 1: '%s' '%s'", e.name.c_str(), e.args.c_str());
        clk(R - 60, rowY(5) + 12);
        shot();
        pr = popupRow(R - 150, rowY(5), 1);
        e = press(pr.first, pr.second);
        CHECKV(e.name == "display-figures" && e.args == "1", "Counter figures row 1: '%s' '%s'", e.name.c_str(), e.args.c_str());
        clk(R - 60, rowY(10) + 12);   // Trail style (docs 6.52): row 1 is Ribbon
        shot();
        pr = popupRow(R - 150, rowY(10), 1);
        e = press(pr.first, pr.second);
        CHECKV(e.name == "display-trail-style" && e.args == "1", "Trail style row 1: '%s' '%s'", e.name.c_str(), e.args.c_str());
        clk(R - 60, rowY(13) + 12);
        shot();
        pr = popupRow(R - 150, rowY(13), 2);
        e = press(pr.first, pr.second);
        CHECKV(e.name == "display-region" && e.args == "2", "Region row 2: '%s' '%s'", e.name.c_str(), e.args.c_str());
        clk(x0 + 30, rowY(12) + 12);
        shot();
        pr = popupRow(x0, rowY(12), 2);
        e = press(pr.first, pr.second);
        CHECKV(e.name == "display-band-size" && e.args == "2", "Band size row 2: '%s' '%s'", e.name.c_str(), e.args.c_str());
        clk(R - 30, rowY(12) + 12);
        shot();
        pr = popupRow(R - 124, rowY(12), 1);
        e = press(pr.first, pr.second);
        CHECKV(e.name == "display-band-colour" && e.args == "1", "Band colour row 1: '%s' '%s'", e.name.c_str(), e.args.c_str());
        park();
        // the ticks: the model says on, so the press asks for off (and the box stays where the model has it)
        e = press(x0 + 60, rowY(6) + 12);
        CHECKV(e.name == "display-tuner-switches" && e.args == "0", "Tuner switches: '%s' '%s'", e.name.c_str(), e.args.c_str());
        park();
        CHECK(shot().countNear(x0, rowY(6) + 5, x0 + 14, rowY(6) + 19, accent, 0) >= 100);
        e = press(x0 + 60, rowY(7) + 12);
        CHECKV(e.name == "display-enlarge" && e.args == "1", "Enlarge: '%s' '%s'", e.name.c_str(), e.args.c_str());
        park();
        CHECK(shot().countNear(x0, rowY(7) + 5, x0 + 14, rowY(7) + 19, accent, 0) == 0);
        e = press(x0 + 40, rowY(11) + 12);
        CHECKV(e.name == "display-band-plan" && e.args == "0", "Band plan: '%s' '%s'", e.name.c_str(), e.args.c_str());
        park();
        // the sliders: a drag from the knob to the middle of the track reports about 0.5 (display-db-min, display-icon-px, display-trail-px)
        const struct { int k; const char* name; double from; } sl[] = {{2, "display-db-min", 0.357}, {3, "display-db-max", 0.833}, {8, "display-icon-px", 0.4}, {9, "display-trail-px", 0.1}};
        for (const auto& s : sl) {
            const Track t = trackOf(f0, rowY(3), x0);
            const int ty = rowY(s.k) + 12, len = t.end - t.start + 1;
            const int fromX = t.start + static_cast<int>(std::lround(s.from * len)), toX = t.start + len / 2;
            evs.clear();
            mv(fromX, ty);
            ::press(win, static_cast<float>(fromX), static_cast<float>(ty));
            mv(toX, ty);
            shot();
            ::release(win, static_cast<float>(toX), static_cast<float>(ty));
            double last = -1;
            for (const Ev& g : evs) { if (g.name == s.name) { last = num(g.args); } }
            CHECKV(std::abs(last - 0.5) < 0.04, "%s: a drag to the middle reports %.3f", s.name, last);
            park();
        }
        // the plain body forwards to SystemEvents: the Theme pick and the Tuner tick and a click on the Max dB track
        const int T2 = yOf(ui->get_a2_y()) + kSecRow + kBodyPad;
        fwd.clear();
        clk(R - 60, T2 + 30 + 12);
        shot();
        pr = popupRow(R - 150, T2 + 30, 1);
        clk(pr.first, pr.second);
        CHECKV(!fwd.empty() && fwd.back().name == "display-theme" && fwd.back().args == "1", "a2 forwards the theme pick to SystemEvents");
        fwd.clear();
        clk(x0 + 60, T2 + 6 * kPitch + 12);
        CHECKV(!fwd.empty() && fwd.back().name == "display-tuner-switches" && fwd.back().args == "1", "a2 forwards the tick to SystemEvents (the model says off: asks on)");
        park();
        // the greyed list and the band-plan toggled off in the model hides Size, Colour, Region and the status
        auto m = ui->get_m_display();
        const int h0 = yOf(ui->get_a1_body_h());
        m.band_plan = false;
        ui->set_m_display(m);
        shot();
        CHECKV(yOf(ui->get_a1_body_h()) == h0 - 2 * kPitch - 14 - 6, "Band plan off: Size, Colour, Region and the status go (%d -> %d)", h0, yOf(ui->get_a1_body_h()));
        m.band_plan = true;
        ui->set_m_display(m);
        shot();
        CHECK(yOf(ui->get_a1_body_h()) == h0);
    }

    // =========================================================================================================
    // 10. Language: the dropdowns, the filter field of the Country list, the keys
    // =========================================================================================================
    park();
    const auto lang0 = ui->get_m_lang();
    const int hLang = yOf(ui->get_b1_body_h());
    {
        shot();
        // the Language list: the chosen row (index 2) ticked, the greyed Magyar dim with its note and no pick
        clk(Rb - 60, Tb + 12);
        const Frame o = shot();
        const int r2 = Tb + kRow + 2 + 5 + 2 * 26, r5 = Tb + kRow + 2 + 5 + 5 * 26;
        CHECKV(o.countNear(Rb - 150 + 5, r2 + 2, Rb - 150 + 22, r2 + 24, accent, 30) >= 6, "the chosen language has the tick");
        CHECK(o.countNear(Rb - 150 + 26, r5 + 4, Rb - 150 + 100, r5 + 22, disabledInk, 40) >= 15);
        evs.clear();
        clk(Rb - 100, r5 + 13);
        CHECKV(evs.empty(), "a greyed language does nothing");
        auto pr = popupRow(Rb - 150, Tb, 3);
        Ev e = press(pr.first, pr.second);
        CHECKV(e.name == "language-selected" && e.args == "3", "language row 3: '%s' '%s'", e.name.c_str(), e.args.c_str());
        park();
        const int sy = Tb + 30 + 14 + 6;
        e = press(xb + 100, sy + 12);
        CHECKV(e.name == "language-translation", "SUGGEST A BETTER TRANSLATION: '%s'", e.name.c_str());
        const int cy = sy + 24 + 6;
        const int uy = cy + 30 + 14 + 6 + 14 + 6;
        e = press(xb + 100, uy + 12);
        CHECKV(e.name == "language-suggest", "USE DEUTSCH: '%s'", e.name.c_str());
        // the Country list: a filter field above the rows (field ground, placeholder in the disabled ink), the list under it
        clk(Rb - 60, cy + 12);
        const Frame c = shot();
        const int fy = cy + kRow + 2 + 5;
        CHECKV(c.near(Rb - 150 + 40, fy, lineStrong) && c.near(Rb - 150 + 40, fy + 23, lineStrong) && c.near(Rb - 150 + 40, fy + 5, field, 0), "the filter field is a 24 px input at the top of the list (top %d)", fy);
        CHECKV(c.countNear(Rb - 150 + 12, fy + 4, Rb - 150 + 200, fy + 20, disabledInk, 40) >= 30, "its placeholder");
        pr = popupRow(Rb - 150, cy, 1, 30);
        CHECKV(c.countNear(Rb - 150 + 5, pr.second - 11, Rb - 150 + 22, pr.second + 11, accent, 30) >= 6, "the chosen country (row 1) has the tick under the filter");
        // typing reaches the field (the popup's own input): each edit is reported; the list is the host's to shorten
        settle(milliseconds(100));
        evs.clear();
        tap(u8"g"); tap(u8"e");
        bool typed = false;
        for (const Ev& g : evs) { if (g.name == "country-filter" && g.args == "ge") { typed = true; } }
        CHECKV(typed, "the filter reports each edit ('ge'): %zu events", evs.size());
        // a pick reports the row and puts the filter back to empty
        e = press(pr.first, pr.second);
        CHECKV(e.name == "country-filter" && e.args.empty(), "a pick clears the filter: '%s' '%s'", e.name.c_str(), e.args.c_str());
        bool picked = false;
        for (const Ev& g : evs) { if (g.name == "country-selected" && g.args == "1") { picked = true; } }
        CHECKV(picked, "Country row 1 picks index 1");
        park();
        // a model with a short list under a filter: the popup is only as tall as its rows
        auto m = ui->get_m_lang();
        m.countries = std::make_shared<slint::VectorModel<SelectItem>>(std::vector<SelectItem>{SelectItem{"Germany", "DE", true, "DE"}});
        m.country_filter = "ger";
        ui->set_m_lang(m);
        shot();
        clk(Rb - 60, cy + 12);
        const Frame s = shot();
        const int bottomEdge = fy + 30 + 26 + 5;
        CHECKV(s.near(Rb - 150 + 40, bottomEdge - 1, knobRing, 3) && !s.near(Rb - 150 + 40, bottomEdge - 28, knobRing, 3), "the filtered list is one row under the field (the popup ends at %d)", bottomEdge);
        park();
        clk(R + 100, 1500);
        m.countries = std::make_shared<slint::VectorModel<SelectItem>>(std::vector<SelectItem>{SelectItem{"Not set", "", true, ""}, SelectItem{"United Kingdom", "GB", true, "GB"}});
        m.country_filter = "";
        m.machine_hint = "";
        ui->set_m_lang(m);
        shot();
        CHECKV(yOf(ui->get_b1_body_h()) == hLang - 14 - 6 - 24 - 6, "no machine hint: the hint and SUGGEST go (%d -> %d)", hLang, yOf(ui->get_b1_body_h()));
        ui->set_m_lang(lang0);                                  // the sheet goes back to what it was: the Sections below sit where they sat
        shot();
    }

    // =========================================================================================================
    // 11. Updates, Serial ports and the GPS row
    // =========================================================================================================
    park();
    {
        Ev e = press(xb + 60, T2b + 12);
        CHECKV(e.name == "updates-check" && e.args == "0", "check at startup: '%s' '%s'", e.name.c_str(), e.args.c_str());
        e = press(xb + 60, T2b + 2 * kPitch + 12);
        CHECKV(e.name == "updates-channel" && e.args == "1", "Every release (beta): '%s' '%s'", e.name.c_str(), e.args.c_str());
        e = press(xb + 40, T3b + kPitch + 12);
        CHECKV(e.name == "updates-channel" && e.args == "0", "Stable releases: '%s' '%s'", e.name.c_str(), e.args.c_str());
        CHECK(!shot().countNear(xb, T2b + 2 * kPitch + 5, xb + 14, T2b + 2 * kPitch + 19, accent, 0));                     // the model has not moved: still the first
        const auto ls = textLines(f0, xb, Rb, T2b + 3 * kPitch, T2b + yOf(ui->get_b2_body_h()) - kBodyPadBottom, panel);
        const auto team = lineAt(ls, -2);
        e = press(xb + 60, (team.first + team.second) / 2);
        CHECKV(e.name == "updates-team-message", "the team message: '%s'", e.name.c_str());
        const int kyp = T3b + yOf(ui->get_b3_body_h()) - kBodyPadBottom - kBodyPad - 24;
        e = press(xb + 40, kyp + 12);
        CHECKV(e.name == "updates-show-again", "Show it again: '%s'", e.name.c_str());
        // THE KILL SWITCH (docs 6.63) on b6: the amber "has been withdrawn" status and the advice under it, then Open foxsdr.com and Not now side by side
        {
            const Frame fr = shot();
            const int T6b = yOf(ui->get_b6_y()) + kSecRow + kBodyPad;
            const int bottom = T6b + yOf(ui->get_b6_body_h()) - kBodyPadBottom - kBodyPad;
            const int ky = bottom - 24;
            CHECKV(fr.countNear(xb, ky - 34, Rb, ky - 6, warnFg, 40) >= 120, "b6: the withdrawn status and its advice are amber (%d)", fr.countNear(xb, ky - 34, Rb, ky - 6, warnFg, 40));
            // the two small keys: runs of the key fill just under the top border (above the glyphs) give the left and right edge of each
            std::vector<std::pair<int, int>> runs;
            for (int x = xb; x < Rb; ++x) {
                if (!fr.near(x, ky + 3, keyBg)) { continue; }
                if (!runs.empty() && x - runs.back().second <= 2) { runs.back().second = x; } else { runs.emplace_back(x, x); }
            }
            CHECKV(runs.size() == 2, "b6: two keys on the row (%zu runs; row y %d, b6 y %d, body %d, xb %d..%d)", runs.size(), ky, yOf(ui->get_b6_y()), yOf(ui->get_b6_body_h()), xb, Rb);
            if (runs.size() == 2) {
                e = press((runs[0].first + runs[0].second) / 2, ky + 12);
                CHECKV(e.name == "updates-open-site", "Open foxsdr.com: '%s'", e.name.c_str());
                e = press((runs[1].first + runs[1].second) / 2, ky + 12);
                CHECKV(e.name == "updates-withdrawn-dismiss", "Not now: '%s'", e.name.c_str());
            }
            // the same notice with its keys gone (a fixed build is offered, or the notice was put away): the body is one key row shorter
            const int hRecall = yOf(ui->get_b6_body_h());
            auto mr = ui->get_m_upd_recall();
            mr.site_label = "";
            mr.dismiss_label = "";
            mr.withdrawn_advice = "We recommend updating to FoxSDR 0.99.106.";
            ui->set_m_upd_recall(mr);
            shot();
            CHECKV(yOf(ui->get_b6_body_h()) < hRecall - 20, "b6: no keys, a shorter body (%d -> %d)", hRecall, yOf(ui->get_b6_body_h()));
            mr.site_label = "Open foxsdr.com";
            mr.dismiss_label = "Not now";
            mr.withdrawn_advice = "A fixed version is not ready yet. foxsdr.com will have it as soon as it is.";
            ui->set_m_upd_recall(mr);
            shot();
            CHECK(yOf(ui->get_b6_body_h()) == hRecall);
            park();
        }
        // the Store copy has no channel radios: `packaged` takes both lines and the help away
        auto m = ui->get_m_upd_ok();
        const int h0 = yOf(ui->get_b2_body_h());
        m.packaged = true;
        ui->set_m_upd_ok(m);
        shot();
        CHECKV(yOf(ui->get_b2_body_h()) < h0 - 2 * kPitch, "no channel: the radios and their help go (%d -> %d)", h0, yOf(ui->get_b2_body_h()));
        m.packaged = false;
        ui->set_m_upd_ok(m);
        shot();
        CHECK(yOf(ui->get_b2_body_h()) == h0);
        park();

        // Serial ports
        int ky = -1;
        for (int y = T4b + 20; y < T4b + 80 && ky < 0; ++y) { if (f0.near(xb + 20, y, keyLine, 0)) { ky = y; } }
        e = press(xb + 20, ky + 12);
        CHECKV(e.name == "serial-refresh", "Refresh: '%s'", e.name.c_str());
        const int gy = ky + 24 + 6 + 22 + 48 + 6;
        const int py = dropTop(f0, Rb, gy + 10, gy + 80);
        clk(xb + 40, py + 12);
        shot();
        auto pr = popupRow(xb, py, 1);
        e = press(pr.first, pr.second);
        CHECKV(e.name == "gps-port" && e.args == "1", "GPS port row 1: '%s' '%s'", e.name.c_str(), e.args.c_str());
        clk(Rb - 40, py + 12);
        shot();
        pr = popupRow(Rb - 84, py, 2);
        e = press(pr.first, pr.second);
        CHECKV(e.name == "gps-baud" && e.args == "2", "GPS baud row 2: '%s' '%s'", e.name.c_str(), e.args.c_str());
        e = press(xb + 100, py + 30 + 12);
        CHECKV(e.name == "gps-toggle", "the GPS key: '%s'", e.name.c_str());
        // b5 forwards to SystemEvents; the GPS key is not lit while the model says not listening
        const int g5 = T5b + 50 + 14 + 6 + 18 + 6;
        fwd.clear();
        clk(xb + 100, g5 + 30 + 12);
        CHECKV(!fwd.empty() && fwd.back().name == "gps-toggle", "b5 forwards the GPS key to SystemEvents");
        // no port named: the key is dead (the model says key-enabled false) and a press says nothing
        auto n5 = ui->get_m_ser_none();
        n5.gps.key_enabled = false;
        ui->set_m_ser_none(n5);
        const Frame dead = shot();
        CHECKV(dead.near(xb + 100, g5 + 30, dis, 4), "a GPS key with no port is the disabled look");
        fwd.clear();
        clk(xb + 100, g5 + 30 + 12);
        CHECKV(fwd.empty(), "and takes no press");
        n5.gps.key_enabled = true;
        ui->set_m_ser_none(n5);
        shot();
        // a port added to the model grows the table by a 24 px row
        const auto ser0 = ui->get_m_ser();
        auto s = ui->get_m_ser();
        const int hs = yOf(ui->get_b4_body_h());
        s.ports = std::make_shared<slint::VectorModel<slint::SharedString>>(std::vector<slint::SharedString>{"COM3", "COM7", "COM11"});
        ui->set_m_ser(s);
        shot();
        CHECKV(yOf(ui->get_b4_body_h()) == hs + 24, "a third port adds one 24 px row (%d -> %d)", hs, yOf(ui->get_b4_body_h()));
        s.ports = std::make_shared<slint::VectorModel<slint::SharedString>>();
        ui->set_m_ser(s);
        shot();
        CHECKV(yOf(ui->get_b4_body_h()) < hs - 48 - 22 + 20, "no ports: the table goes for a line (%d)", yOf(ui->get_b4_body_h()));
        ui->set_m_ser(ser0);
        shot();
        park();
    }

    // =========================================================================================================
    // 12. Key bindings: the capture, through real key events
    // =========================================================================================================
    park();
    {
        crop(shot(), "crop-keys0.png", kColC, 0, 300, 400);
        // the host's side of the capture: set which row listens, row by row in the existing models (a row is told, not rebuilt) or by publishing whole models
        const auto listen = [&](const std::string& id, bool replace) {
            auto m = ui->get_m_keys();
            auto groups = std::make_shared<slint::VectorModel<KeyGroup>>();
            for (std::size_t g = 0; g < m.groups->row_count(); ++g) {
                KeyGroup kg = *m.groups->row_data(g);
                auto vm = std::dynamic_pointer_cast<slint::VectorModel<KeyRow>>(kg.rows);
                CHECK(vm != nullptr);
                auto rows = std::make_shared<slint::VectorModel<KeyRow>>();
                for (std::size_t r = 0; r < kg.rows->row_count(); ++r) {
                    KeyRow kr = *kg.rows->row_data(r);
                    kr.capturing = (id == std::string(kr.id));
                    if (replace) { rows->push_back(kr); } else if (vm) { vm->set_row_data(r, kr); }
                }
                kg.rows = rows;
                groups->push_back(kg);
            }
            if (replace) { m.groups = groups; ui->set_m_keys(m); }
            settle(milliseconds(150));
            shot();
        };
        listen("", false);                                // nothing listens: the presses below ask the host to listen
        const auto chordOf = [&](std::u8string_view k, bool ctrl, bool shiftK, bool alt) {
            evs.clear();
            if (ctrl) { keyDown(win, slint::platform::key_codes::Control); }
            if (shiftK) { keyDown(win, slint::platform::key_codes::Shift); }
            if (alt) { keyDown(win, slint::platform::key_codes::Alt); }
            tap(k);
            if (alt) { keyUp(win, slint::platform::key_codes::Alt); }
            if (shiftK) { keyUp(win, slint::platform::key_codes::Shift); }
            if (ctrl) { keyUp(win, slint::platform::key_codes::Control); }
            return evs.empty() ? Ev{} : evs.back();
        };
        // a press on a cap asks the host to listen to it (the id), whatever its row; the host answers by setting `capturing`
        const int capX = Rc - 60;
        Ev e = press(capX, capTop[1] + 10);
        CHECKV(e.name == "keys-capture" && e.args == "mute", "the Mute cap: '%s' '%s'", e.name.c_str(), e.args.c_str());
        e = press(capX, capTop[3] + 10);
        CHECKV(e.name == "keys-capture" && e.args == "squelchDown", "the Squelch down cap: '%s' '%s'", e.name.c_str(), e.args.c_str());
        e = press(capX, capTop[4] + 10);
        CHECKV(e.name == "keys-capture" && e.args == "modeCw", "a cap of the second group: '%s' '%s'", e.name.c_str(), e.args.c_str());
        // a press on the action's NAME is not a press on the cap
        e = press(xc + 20, capTop[1] + 10);
        CHECKV(e.name.empty(), "the action's name is not a key: '%s'", e.name.c_str());
        listen("squelchUp", false);                       // the artboard's state, reached the way the host reaches it
        crop(shot(), "crop-keys1.png", kColC, 0, 300, 400);
        // the listening cap (squelchUp) holds the keyboard: the chord arrives as the config's text, modifiers in the order Ctrl, Shift, Alt
        e = chordOf(slint::platform::key_codes::PageUp, true, true, false);
        CHECKV(e.name == "keys-captured" && e.args == "squelchUp Ctrl+Shift+PageUp", "Ctrl+Shift+PageUp: '%s' '%s'", e.name.c_str(), e.args.c_str());
        e = chordOf(u8"q", false, false, false);
        CHECKV(e.name == "keys-captured" && e.args == "squelchUp Q", "q is Q: '%s' '%s'", e.name.c_str(), e.args.c_str());
        e = chordOf(u8"F", false, true, false);
        CHECKV(e.name == "keys-captured" && e.args == "squelchUp Shift+F", "Shift+F: '%s' '%s'", e.name.c_str(), e.args.c_str());
        e = chordOf(slint::platform::key_codes::F5, false, false, true);
        CHECKV(e.name == "keys-captured" && e.args == "squelchUp Alt+F5", "Alt+F5: '%s' '%s'", e.name.c_str(), e.args.c_str());
        e = chordOf(slint::platform::key_codes::F5, false, true, true);
        CHECKV(e.name == "keys-captured" && e.args == "squelchUp Shift+Alt+F5", "Shift+Alt+F5: '%s' '%s'", e.name.c_str(), e.args.c_str());
        e = chordOf(slint::platform::key_codes::DownArrow, false, false, false);
        CHECKV(e.name == "keys-captured" && e.args == "squelchUp Down", "the Down arrow: '%s' '%s'", e.name.c_str(), e.args.c_str());
        e = chordOf(u8" ", false, false, false);
        CHECKV(e.name == "keys-captured" && e.args == "squelchUp Space", "Space: '%s' '%s'", e.name.c_str(), e.args.c_str());
        e = chordOf(u8"=", false, false, false);
        CHECKV(e.name == "keys-captured" && e.args == "squelchUp Plus", "the Equal key is Plus: '%s' '%s'", e.name.c_str(), e.args.c_str());
        e = chordOf(u8"-", true, false, false);
        CHECKV(e.name == "keys-captured" && e.args == "squelchUp Ctrl+Minus", "Ctrl+Minus: '%s' '%s'", e.name.c_str(), e.args.c_str());
        e = chordOf(u8"7", true, false, false);
        CHECKV(e.name == "keys-captured" && e.args == "squelchUp Ctrl+7", "Ctrl+7: '%s' '%s'", e.name.c_str(), e.args.c_str());
        // a modifier alone is not a chord: the cap goes on listening and says nothing
        evs.clear();
        keyDown(win, slint::platform::key_codes::Control);
        keyUp(win, slint::platform::key_codes::Control);
        keyDown(win, slint::platform::key_codes::Shift);
        keyUp(win, slint::platform::key_codes::Shift);
        CHECKV(evs.empty(), "Ctrl or Shift alone says nothing (%zu events)", evs.size());
        // Esc leaves it, Backspace clears it
        e = chordOf(slint::platform::key_codes::Escape, false, false, false);
        CHECKV(e.name == "keys-cancel", "Esc: '%s'", e.name.c_str());
        e = chordOf(slint::platform::key_codes::Backspace, false, false, false);
        CHECKV(e.name == "keys-clear" && e.args == "squelchUp", "Backspace: '%s' '%s'", e.name.c_str(), e.args.c_str());
        // a second click on the listening cap leaves it too
        e = press(capX, capTop[2] + 10);
        CHECKV(e.name == "keys-cancel", "a click on the listening cap: '%s'", e.name.c_str());
        park();
        // the host moves the listening to another row (the Mute cap), in place: the keyboard follows, and the old cap is deaf
        evs.clear();
        listen("mute", false);
        const Frame h = shot();
        crop(h, "crop-keys.png", kColC, 0, 300, 400);
        CHECKV(evs.empty(), "moving the listening in place says nothing (%zu events)", evs.size());
        CHECKV(h.near(Rc - 104, capTop[1] + 10, warnBg, 0) && h.near(Rc - 104, capTop[2] + 10, chipBg, 0), "the listening cap moved to Mute");
        CHECKV(h.countNear(Rc - 118, capTop[1] - 4, Rc + 4, capTop[1] + 24, tok(wb.focus), 0) >= 40, "the focus ring is on the listening cap");
        e = chordOf(u8"x", false, false, false);
        CHECKV(e.name == "keys-captured" && e.args == "mute X", "the keyboard followed to Mute: '%s' '%s'", e.name.c_str(), e.args.c_str());
        e = chordOf(slint::platform::key_codes::Escape, false, false, false);
        CHECKV(e.name == "keys-cancel", "Esc on Mute: '%s'", e.name.c_str());
        // the host publishes a whole new model with the Squelch up cap listening (rows made already listening): the cap still gets the keyboard
        evs.clear();
        listen("squelchUp", true);
        CHECKV(evs.empty(), "replacing the model says nothing (%zu events)", evs.size());
        e = chordOf(u8"p", true, false, false);
        CHECKV(e.name == "keys-captured" && e.args == "squelchUp Ctrl+P", "a cap made listening takes the keyboard: '%s' '%s'", e.name.c_str(), e.args.c_str());
        // the plain body (c2) forwards the cap press and Reset to SystemEvents
        fwd.clear();
        clk(capX, c2hy + 30 + 12);
        CHECKV(!fwd.empty() && fwd.back().name == "keys-capture" && fwd.back().args == "startStop", "c2 forwards the cap press: '%s'", fwd.empty() ? "" : fwd.back().args.c_str());
        fwd.clear();
        clk(Rc - 30, c2hy + 12);
        CHECKV(!fwd.empty() && fwd.back().name == "keys-reset", "c2 forwards Reset to defaults");
        // Reset to defaults on the first body
        e = press(Rc - 30, c1hy + 12);
        CHECKV(e.name == "keys-reset", "Reset to defaults: '%s'", e.name.c_str());
        // the listening ends when the model says so: the row goes back to the grey pill and takes no more keys
        listen("", false);
        const Frame n = shot();
        CHECKV(n.near(Rc - 104, capTop[1] + 10, chipBg, 0) && n.near(Rc - 104, capTop[2] + 10, chipBg, 0), "nothing is listening");
        evs.clear();
        e = chordOf(u8"x", false, false, false);
        CHECKV(e.name.empty(), "nothing listens: '%s'", e.name.c_str());
        park();
    }

    // =========================================================================================================
    // 13. Diagnostics, Usage reporting, Beta tester: the ticks and keys
    // =========================================================================================================
    park();
    const auto diag0 = ui->get_m_diag();
    const int hBase = yOf(ui->get_d1_body_h());
    {
        Ev e;
        const int dEnd = Td1 + yOf(ui->get_d1_body_h()) - kBodyPadBottom - kBodyPad;
        const int box = findBox(f0, xd, Td1 + 20, dEnd, true);
        e = press(xd + 60, box + 7);
        CHECKV(e.name == "diag-record" && e.args == "0", "Record crashes: '%s' '%s'", e.name.c_str(), e.args.c_str());
        int xs = -1;
        for (int y = box + 14; y < box + 200 && xs < 0; ++y) { if (f0.near(xd + 40, y, keyLine, 0)) { xs = y; } }
        e = press(xd + 40, xs + 10);
        CHECKV(e.name == "diag-what" && e.args == "0", "What exactly is in a report? (open: asks to close): '%s' '%s'", e.name.c_str(), e.args.c_str());
        // the dump tick follows the disclosure text, the ledger tick the paths
        const int dumpBox = findBox(f0, xd, xs + 24, dEnd, false);
        CHECKV(dumpBox > 0, "the memory dump box at %d", dumpBox);
        e = press(xd + 60, dumpBox + 7);
        CHECKV(e.name == "diag-minidump" && e.args == "1", "memory dump: '%s' '%s'", e.name.c_str(), e.args.c_str());
        const int ledgerBox = findBox(f0, xd, dumpBox + 16, dEnd, false);
        CHECKV(ledgerBox > 0, "the pointer ledger box at %d", ledgerBox);
        e = press(xd + 60, ledgerBox + 7);
        CHECKV(e.name == "diag-ledger" && e.args == "1", "pointer ledger: '%s' '%s'", e.name.c_str(), e.args.c_str());
        // the keys under the ledger's help: Copy diagnostics; Open reports folder and Open the log side by side
        int ck = -1;
        for (int y = ledgerBox + 20; y < ledgerBox + 140 && ck < 0; ++y) { if (f0.near(xd + 30, y, keyLine, 0) && f0.near(xd, y + 12, keyLine, 0)) { ck = y; } }
        CHECKV(ck > 0 && f0.near(xd + 30, ck + 23, keyLine, 0) && !f0.near(xd + 30, ck + 24, keyLine, 0), "Copy diagnostics: a 24 px key (top %d)", ck);
        e = press(xd + 30, ck + 12);
        CHECKV(e.name == "diag-copy", "Copy diagnostics: '%s'", e.name.c_str());
        e = press(xd + 30, ck + 30 + 12);
        CHECKV(e.name == "diag-open-reports", "Open reports folder: '%s'", e.name.c_str());
        const int second = lastX(f0, ck + 30 + 12, xd + 40, Rd, keyLine, 0);
        CHECKV(second > xd + 150, "the Open the log key is beside it (right edge %d)", second);
        e = press(second - 20, ck + 30 + 12);
        CHECKV(e.name == "diag-open-log", "Open the log: '%s'", e.name.c_str());
        const int run = dEnd - 24;
        e = press(xd + 40, run + 12);
        CHECKV(e.name == "diag-sdrplay", "Run SDRplay diagnostic: '%s'", e.name.c_str());
        // disabled while the radio is open: grey key, no event
        auto m = ui->get_m_diag();
        m.sdrplay_enabled = false;
        m.sdrplay_status = "FoxSDR has the SDRplay radio open - choose another source first.";
        ui->set_m_diag(m);
        const Frame g = shot();
        CHECKV(g.near(xd + 40, run, dis, 4) && g.near(xd + 40, run + 23, dis, 4) && g.near(xd + 40, run + 6, panel, 0), "the disabled Run key has the half-alpha border on the panel ground");
        e = press(xd + 40, run + 12);
        CHECKV(e.name.empty(), "a disabled key does nothing: '%s'", e.name.c_str());
        CHECKV(g.countNear(xd, run + 24 + 6, Rd, run + 24 + 6 + 28, dim, 30) >= 80, "the SDRplay status line under the key");
        // the disclosure closed in the model: the field list goes (the body gets shorter)
        m = diag0;
        m.what_open = false;
        ui->set_m_diag(m);
        shot();
        CHECKV(yOf(ui->get_d1_body_h()) < hBase - 30, "What exactly... closed: the field list goes (%d -> %d)", hBase, yOf(ui->get_d1_body_h()));
        ui->set_m_diag(diag0);                                  // the sheet goes back to what it was: the Sections below sit where they sat
        shot();
        CHECKV(yOf(ui->get_d1_body_h()) == hBase, "the original model restores the original height (%d, was %d)", yOf(ui->get_d1_body_h()), hBase);
        park();

        // Usage reporting (d2)
        const int ub = findBox(f0, xd, Td2 + 10, Td2 + 160, true);
        CHECKV(ub > 0, "the usage tick at %d", ub);
        e = press(xd + 60, ub + 7);
        CHECKV(e.name == "usage-send" && e.args == "0", "Send anonymous usage reports: '%s' '%s'", e.name.c_str(), e.args.c_str());
        int wk = -1;
        for (int y = ub + 14; y < ub + 220 && wk < 0; ++y) { if (f0.near(xd + 40, y, keyLine, 0)) { wk = y; } }
        CHECKV(wk > 0 && f0.near(xd + 40, wk + 19, keyLine, 0) && !f0.near(xd + 40, wk + 20, keyLine, 0), "What exactly is sent?: an xs key at %d", wk);
        e = press(xd + 40, wk + 10);
        CHECKV(e.name == "usage-what" && e.args == "0", "What exactly is sent? (open): '%s' '%s'", e.name.c_str(), e.args.c_str());
        e = press(xd + 40, wk + 26 + 10);
        CHECKV(e.name == "usage-privacy", "Privacy policy: '%s'", e.name.c_str());
        // the payload box: field ground, four mono lines in fg-soft
        const auto pl = textLines(f0, xd + 8, Rd - 8, wk + 60, Td2 + yOf(ui->get_d2_body_h()) - kBodyPadBottom, field);
        CHECKV(pl.size() >= 4, "d2: the payload's %zu lines", pl.size());
        // d3: the off line, the tick unticked (press asks on)
        const int ub3 = findBox(f0, xd, Td3 + 10, Td3 + 120, false);
        CHECKV(ub3 > 0, "d3: the unticked box at %d", ub3);
        e = press(xd + 60, ub3 + 7);
        CHECKV(e.name == "usage-send" && e.args == "1", "d3 asks to turn it on: '%s' '%s'", e.name.c_str(), e.args.c_str());
        park();

        // Beta tester: linked (d4)
        const auto l4 = textLines(f0, xd, Rd, Td4, Td4 + yOf(ui->get_d4_body_h()) - kBodyPadBottom - kBodyPad, panel);
        CHECKV(l4.size() >= 5, "d4: %zu lines", l4.size());
        int unlinkTop = -1;
        for (int y = Td4 + 20; y < Td4 + 120 && unlinkTop < 0; ++y) { if (f0.near(Rd - 20, y, keyLine, 0)) { unlinkTop = y; } }
        CHECKV(unlinkTop > 0 && f0.near(Rd - 20, unlinkTop + 19, keyLine, 0) && !f0.near(Rd - 20, unlinkTop + 20, keyLine, 0), "Unlink: an xs key at the right edge (top %d)", unlinkTop);
        e = press(Rd - 20, unlinkTop + 10);
        CHECKV(e.name == "tester-unlink", "Unlink: '%s'", e.name.c_str());
        int showTop = -1;
        for (int y = unlinkTop + 30; y < unlinkTop + 120 && showTop < 0; ++y) { if (f0.near(xd + 30, y, keyLine, 0) && f0.near(xd, y + 10, keyLine, 0)) { showTop = y; } }
        CHECKV(showTop > 0, "Hide what is sent: an xs key at %d", showTop);
        e = press(xd + 30, showTop + 10);
        CHECKV(e.name == "tester-show-sent" && e.args == "0", "Hide what is sent: '%s' '%s'", e.name.c_str(), e.args.c_str());
        CHECKV(f0.near(xd + 100, showTop + 20 + 6 + 6, field, 0), "the masked preview is on the field ground");
        // d5: Remove code, the masked field (dots: no readable text), SHOW reveals, the Use key commits the draft then asks
        int rc = -1;
        for (int y = Td5 + 20; y < Td5 + 160 && rc < 0; ++y) { if (f0.near(Rd - 20, y, keyLine, 0)) { rc = y; } }
        CHECKV(rc > 0, "Remove code at %d", rc);
        e = press(Rd - 20, rc + 10);
        CHECKV(e.name == "tester-remove-code", "Remove code: '%s'", e.name.c_str());
        int fieldTop = -1;
        for (int y = rc + 24; y < rc + 220 && fieldTop < 0; ++y) { if (f0.near(xd + 40, y, lineStrong, 0) && f0.near(xd + 40, y + 3, field, 0)) { fieldTop = y; } }
        CHECKV(fieldTop > 0 && f0.near(xd + 40, fieldTop + 23, lineStrong, 0), "the code field is a 24 px input at %d", fieldTop);
        CHECKV(f0.countInk(xd + 8, fieldTop + 4, xd + 120, fieldTop + 20, field) >= 30, "the masked code draws dots");
        const int showX = lastX(f0, fieldTop + 12, xd + 100, Rd, keyLine, 0);
        CHECKV(showX > Rd - 4, "the SHOW key ends at the body's edge (%d)", showX);
        park();
        const Frame masked = shot();
        clk(Rd - 25, fieldTop + 12);
        park();
        const Frame shown = shot();
        CHECKV(shown.countNear(Rd - 60, fieldTop + 2, Rd, fieldTop + 22, accent, 0) >= 100, "SHOW is lit once pressed (HIDE)");
        int diff = 0;
        for (int x = xd + 8; x < xd + 120; ++x) { for (int y = fieldTop + 4; y < fieldTop + 20; ++y) { if (masked.at(x, y).r != shown.at(x, y).r) { ++diff; } } }
        CHECKV(diff > 20, "the code shows as text when revealed (%d pixels differ)", diff);
        clk(Rd - 25, fieldTop + 12);
        shot();
        evs.clear();
        clk(xd + 100, fieldTop + 24 + 6 + 12);
        shot();
        bool committed = false, used = false;
        for (const Ev& g2 : evs) { if (g2.name == "tester-code-committed" && g2.args == "ab12cd34") { committed = true; } if (g2.name == "tester-use") { used = true; } }
        CHECKV(committed && used, "Use this code commits the draft, then asks (%zu events)", evs.size());
        // typing a new code and Enter commits it
        evs.clear();
        clk(xd + 40, fieldTop + 12);
        tap(slint::platform::key_codes::End);
        for (int i = 0; i < 12; ++i) { tap(slint::platform::key_codes::Backspace); }
        tap(u8"z"); tap(u8"9");
        tap(slint::platform::key_codes::Return);
        CHECKV(!evs.empty() && evs.back().name == "tester-code-committed" && evs.back().args == "z9", "Enter commits the typed code: '%s'", evs.empty() ? "" : evs.back().args.c_str());
        park();
        // Show what is sent (d5, closed): an xs key under the Use key
        const int showK = fieldTop + 24 + 6 + 24 + 6;
        e = press(xd + 30, showK + 10);
        CHECKV(e.name == "tester-show-sent" && e.args == "1", "Show what is sent: '%s' '%s'", e.name.c_str(), e.args.c_str());
        park();
    }

    // =========================================================================================================
    // 14. The two report pages (artboard F: Request a feature 440 wide, Report a bug 440 wide)
    // =========================================================================================================
    park();
    {
        const int py = yOf(ui->get_pages_y());
        const Frame f = shot();
        crop(f, "crop-pages.png", 0, py, 1060, 830);
        // the feature page: the stage is the backdrop; the panel is 440 wide, centred, panel ground, knob-ring border, a menu-ground title strip 30 high
        const int fx = 10, fw = 500, fh = 420, pw = 440;
        const int px = fx + (fw - pw) / 2;
        int top = -1;
        for (int y = py; y < py + fh && top < 0; ++y) { if (f.near(px + 100, y, knobRing, 0)) { top = y; } }
        CHECKV(top > 0, "the feature page's panel top at %d", top);
        CHECKV(f.near(px, top + 100, knobRing, 0) && f.near(px + pw - 1, top + 100, knobRing, 0) && f.near(px + 100, top + 10, menu, 0) && f.near(px + 100, top + 30, lineStrong, 0) && f.near(px + 100, top + 40, panel, 0), "title strip: menu ground, a rule, then the panel");
        CHECKV(f.countNear(px + 12, top + 6, px + 200, top + 24, fg, 60) >= 40, "the title, upper case");
        CHECK(f.countInk(px + pw - 34, top + 4, px + pw - 6, top + 26, menu) >= 10);
        CHECK(!f.near(fx + 4, py + 4, bg, 0));
        const int cx = px + 1 + 12, cw = pw - 2 - 24;
        // the prompt (fg), the 56 px message box, the counter (mono dim), the contact caption, the 24 px field, the amber no-email note, the sent explanation, SEND (disabled), the refusal, the sent line (green)
        const int c0 = top + 31 + 8;
        CHECKV(f.countNear(cx, c0, cx + cw, c0 + 14, fg, 60) >= 60, "the prompt in fg");
        const int bx = c0 + 14 + 6;
        CHECKV(f.near(cx + 40, bx, lineStrong) && f.near(cx + 40, bx + 55, lineStrong) && f.near(cx + 40, bx + 56, panel, 0) && f.near(cx + 40, bx + 10, field, 0), "the message box is 56 high on the field ground");
        CHECKV(f.countNear(cx, bx + 56 + 6, cx + 200, bx + 56 + 6 + 14, dim, 30) >= 40, "the counter, dim");
        const int cl = bx + 56 + 6 + 14 + 6;
        CHECK(f.countNear(cx, cl, cx + 200, cl + 14, dim, 30) >= 40);
        const int in = cl + 14 + 6;
        CHECKV(f.near(cx + 40, in, lineStrong) && f.near(cx + 40, in + 23, lineStrong) && f.near(cx + 40, in + 24, panel, 0), "the contact field is 24 high");
        CHECKV(f.countNear(cx, in + 30, cx + cw, in + 44, warnFg, 40) >= 100, "the no-email note is amber");
        // SEND: a small key across the content width, on the disabled ground
        const int send = disabledKeyTop(f, cx, cw, in + 60, in + 140);
        CHECKV(send > 0 && f.near(cx + 40, send + 23, dis, 4) && f.near(cx + 40, send - 1, panel, 0) && f.near(cx + 40, send + 24, panel, 0) && f.near(cx + 40, send + 8, panel, 0), "SEND is a 24 px key on the disabled look (top %d)", send);
        CHECKV(f.near(cx + 12, send, dis, 4) && f.near(cx + cw - 13, send, dis, 4) && f.near(cx, send + 12, dis, 4) && f.near(cx + cw - 1, send + 12, dis, 4), "SEND spans the content width");
        // the typed text enables it: the click goes into the message box, the keys type, the host (the specimen) answers with send-enabled
        evs.clear();
        clk(cx + 60, bx + 12);
        tap(u8"h"); tap(u8"i");
        bool sawText = false;
        for (const Ev& g : evs) { if (g.name == "report-text" && g.args == "hi") { sawText = true; } }
        CHECKV(sawText, "typing reports the text ('hi'): %zu events", evs.size());
        park();
        const Frame g = shot();
        CHECKV(g.near(cx + 40, send + 6, keyBg, 0) && g.near(cx + 40, send, keyLine, 0), "after typing, SEND is a live key");
        Ev e = press(cx + 60, send + 12);
        CHECKV(e.name == "report-send", "SEND: '%s'", e.name.c_str());
        clk(cx + 60, bx + 12);
        tap(slint::platform::key_codes::End);
        for (int i = 0; i < 2; ++i) { tap(slint::platform::key_codes::Backspace); }
        park();
        CHECKV(shot().near(cx + 40, send, dis, 4) && shot().near(cx + 40, send + 6, panel, 0), "emptied: SEND is dead again");
        e = press(cx + 60, send + 12);
        CHECKV(e.name.empty(), "emptied: SEND does nothing ('%s')", e.name.c_str());
        // the contact field: each edit is reported
        evs.clear();
        clk(cx + 60, in + 12);
        tap(u8"a");
        bool sawContact = false;
        for (const Ev& h : evs) { if (h.name == "report-contact" && h.args == "a") { sawContact = true; } }
        CHECKV(sawContact, "the contact field reports 'a' (%zu events)", evs.size());
        // the close key and the backdrop dismiss; a click on the panel does not
        e = press(px + pw - 17, top + 15);
        CHECKV(e.name == "report-closed", "the close key: '%s'", e.name.c_str());
        e = press(fx + 6, py + 6);
        CHECKV(e.name == "report-closed", "a click on the backdrop closes: '%s'", e.name.c_str());
        e = press(cx + 100, in - 3);
        CHECKV(e.name != "report-closed", "a click on the panel does not close it");
        park();

        // the bug page: 440 wide too; the message box 64 high; the attach block (bug only), the evidence lines, the SDRplay tick, the preview 140 high
        const int bxs = 520, bw = 520, bh = 820;
        const int bpx = bxs + (bw - pw) / 2;
        const Frame h = shot();
        int btop = -1;
        for (int y = py; y < py + bh && btop < 0; ++y) { if (h.near(bpx + 100, y, knobRing, 0)) { btop = y; } }
        CHECKV(btop > 0, "the bug page's panel top at %d", btop);
        const int bcx = bpx + 1 + 12;
        const int b0 = btop + 31 + 8;
        // the question, the two radios (neither chosen), then the prompt and the box
        CHECKV(h.countNear(bcx, b0, bcx + cw, b0 + 14, fg, 60) >= 60, "What are you reporting? in fg");
        const int rr = b0 + 14 + 6;
        CHECKV(h.near(bcx + 7, rr + 5, lineStrong) || h.near(bcx, rr + 12, lineStrong) || h.near(bcx + 7, rr + 5, lineStrong, 8), "the first radio's ring");
        int ring2 = -1;
        for (int x = bcx + 20; x < bcx + 300 && ring2 < 0; ++x) { if (h.near(x, rr + 12, lineStrong) && h.near(x + 3, rr + 12, field, 4)) { ring2 = x; } }
        CHECKV(ring2 > bcx + 100, "the second radio's ring at %d", ring2);
        CHECKV(h.countNear(bcx, rr + 3, bcx + cw, rr + 21, accent, 0) == 0, "neither radio is chosen when the page opens");
        Ev ek = press(bcx + 7, rr + 12);
        CHECKV(ek.name == "report-kind" && ek.args == "0", "Something is broken (bug): '%s' '%s'", ek.name.c_str(), ek.args.c_str());
        ek = press(ring2 + 7, rr + 12);
        CHECKV(ek.name == "report-kind" && ek.args == "1", "Something I dislike: '%s' '%s'", ek.name.c_str(), ek.args.c_str());
        auto mb = ui->get_m_bug();
        mb.problem_kind_index = 1;
        ui->set_m_bug(mb);
        const Frame chosen = shot();
        CHECKV(chosen.countNear(ring2, rr + 3, ring2 + 14, rr + 21, accent, 0) >= 80 && chosen.countNear(bcx, rr + 3, bcx + 14, rr + 21, accent, 0) == 0, "the model chooses the second radio");
        mb.problem_kind_index = -1;
        ui->set_m_bug(mb);
        shot();
        int bbx = -1;
        for (int y = rr + 24; y < rr + 120 && bbx < 0; ++y) { if (h.near(bcx + 40, y, lineStrong) && h.near(bcx + 40, y + 63, lineStrong) && h.near(bcx + 40, y + 64, panel, 0) && h.near(bcx + 40, y + 10, field, 0)) { bbx = y; } }
        CHECKV(bbx > 0, "the bug message box is 64 high (top %d)", bbx);
        const auto ls = textLines(h, bcx, bcx + cw, bbx + 64 + 4, btop + 700, panel);
        CHECKV(ls.size() >= 12, "the bug page's %zu lines", ls.size());
        // the detector line is green; the ticked attach box is accent; the evidence lines are mono; the preview box is 140 high on the field ground
        CHECKV(h.countNear(bcx, bbx + 64, bcx + cw, bbx + 120, goodFg, 40) >= 100, "the pasted-report line is green");
        const int attachBox = findBox(h, bcx, bbx + 100, bbx + 330, true);
        CHECKV(attachBox > 0, "Attach the diagnostics log is ticked (box at %d)", attachBox);
        int pv = -1;
        for (int y = bbx + 150; y < btop + 700 && pv < 0; ++y) { if (h.near(bcx + 40, y, lineStrong) && h.near(bcx + 40, y + 5, field, 0) && h.near(bcx + 40, y + 138, field, 0)) { pv = y; } }
        CHECKV(pv > 0 && h.near(bcx + 40, pv + 139, lineStrong) && h.near(bcx + 40, pv + 140, panel, 0), "the preview box is 140 high (top %d)", pv);
        CHECKV(h.countNear(bcx + 8, pv + 4, bcx + cw - 8, pv + 40, fgSoft, 40) >= 80, "the preview text in fg-soft mono");
        // SEND: the host says enabled but the box is empty: dead; type a word: live
        const int sendB = disabledKeyTop(h, bcx, cw, pv + 141, btop + 720);
        CHECKV(sendB > 0, "the bug page's SEND is on the disabled look while the box is empty (top %d)", sendB);
        e = press(bcx + 60, sendB + 12);
        CHECKV(e.name.empty(), "SEND with no text does nothing: '%s'", e.name.c_str());
        evs.clear();
        clk(bcx + 60, bbx + 12);
        tap(u8"x");
        park();
        const Frame k = shot();
        crop(k, "crop-bug.png", 520, py, 520, 740);
        CHECKV(k.near(bcx + 40, sendB + 6, keyBg, 0), "after typing, SEND is live although the host never changed send-enabled");
        e = press(bcx + 60, sendB + 12);
        CHECKV(e.name == "report-send", "SEND: '%s'", e.name.c_str());
        // the refusal (dim), the sent line (amber, sending), the dropped note (amber)
        CHECKV(k.countNear(bcx, sendB + 24 + 6, bcx + cw, sendB + 24 + 6 + 14, dim, 30) >= 40, "the refusal is dim");
        CHECKV(k.countNear(bcx, sendB + 24 + 6 + 20, bcx + cw, sendB + 24 + 6 + 34, warnFg, 40) >= 30, "the sent line is amber while sending");
        // the ticks: attach (on: asks off), SDRplay (off: asks on), Show what will be sent (on: asks off)
        e = press(bcx + 60, attachBox + 7);
        CHECKV(e.name == "report-attach" && e.args == "0", "Attach the diagnostics log: '%s' '%s'", e.name.c_str(), e.args.c_str());
        const int probe = findBox(h, bcx, attachBox + 60, attachBox + 200, false);
        CHECKV(probe > 0, "the SDRplay box at %d", probe);
        e = press(bcx + 60, probe + 7);
        CHECKV(e.name == "report-probe" && e.args == "1", "Attach the SDRplay diagnostic too: '%s' '%s'", e.name.c_str(), e.args.c_str());
        e = press(bcx + 60, probe + 30 + 7);
        CHECKV(e.name == "report-preview" && e.args == "0", "Show what will be sent: '%s' '%s'", e.name.c_str(), e.args.c_str());
        // the feature page has none of the bug-only rows: no ticks
        CHECKV(f.countNear(cx, top, cx + cw, top + 420, accent, 0) == 0, "the feature page has no ticks");
        // a model with shown = false: the stage is bare and takes no click
        auto m = ui->get_m_feature();
        m.shown = false;
        ui->set_m_feature(m);
        const Frame z = shot();
        CHECK(z.countInk(fx + 2, py + 2, fx + fw - 2, py + fh - 2, bg, 6) == 0);
        e = press(px + 60, top + 100);
        CHECKV(e.name.empty(), "a hidden page takes no click: '%s'", e.name.c_str());
        e = press(fx + 6, py + 6);
        CHECKV(e.name.empty(), "nor the backdrop's: '%s'", e.name.c_str());
        m.shown = true;
        ui->set_m_feature(m);
        park();
    }

    // =========================================================================================================
    // 15. The sheet once more, after everything
    // =========================================================================================================
    const Frame last = shot();
    last.savePng(shotDir() + "system-specimen-after.png");

    return testSummary("test_slint_system");
}
