// The SIGNAL bank's bodies (ui/signal_bank.slint): SourceBody and its eight variants, FrequencyCorrectionBody, RadioExtras, AudioFiltersBody,
// SinksBody, TransmitRowState, RecorderBody, and the two sheets RadioSetupSheet and ImportDialog - rendered headlessly from
// ui/specimens/signal_specimen.slint (every body inside the real Section it is shown under) and probed against the Banks artboard
// (artboards/Workbench-Banks.dc.html: the SIGNAL column, A1, and the Source-variants strip, A6) and specs/SCREENS.md A1 / A6.
//
// What is asserted to the pixel: the rows' pitch (24 px rows, 6 px between: the artboard's .rw and .sbd gap), the 8 px between a dropdown and its
// neighbour, key heights (small 24, xs 20), the 14 px boxes, the 6 px slider track and its fill, the card ground of a variant, the colours of the
// error (red), the advisory (amber) and the notes (dim). What is asserted as ink in a region: every word (the typefaces are the embedded Inter and
// JetBrains Mono, never glyph positions). Every callback is driven by a real pointer or key event and read back through the specimen's `Rec` global
// (the bodies with an override) or through SignalEvents (the bodies that forward by default). The model owns every value: a press on a tick whose model
// says off leaves it off, which the pixels show.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "render_harness.hpp"
#include "test_check.hpp"

#include "signal_specimen.h"

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
// The top row of a 24 px input at column x: a line-strong border row over the field ground; -1 when none in [y0, y1).
int fieldTopAt(const Frame& f, int x, int y0, int y1, Rgb lineStrong, Rgb field) {
    for (int y = y0; y < y1; ++y) { if (f.near(x, y, lineStrong, 0) && f.near(x, y + 1, field, 0)) { return y; } }
    return -1;
}
double num(const std::string& s) {
    try { return std::stod(s); } catch (...) { return -1e9; }
}

// ---- the specimen's table (signal_specimen.slint): columns at x 10 / 316 / 622 / 928, Sections 296 wide, bodies at +30, 254 wide ----
constexpr int kColA = 10, kColB = 316, kColC = 622, kColD = 928;
constexpr int kSecRow = 30, kBodyPad = 4, kBodyPadBottom = 12, kBodyX = 30, kBodyW = 254;
constexpr int kRow = 24, kPitch = 30;          // the artboard's .rw (min-height 24) and the .sbd gap of 6
constexpr int kW = 1240, kH = 2400;

}  // namespace

int main() {
    installHeadlessPlatform();
    auto ui = SignalSpecimen::create();
    slint::Window& win = ui->window();

    const Skin wb = ui->global<Workbench>().get_value();
    const Rgb bg = tok(wb.bg), panel = tok(wb.panel), card = tok(wb.card), field = tok(wb.field), line = tok(wb.line), lineStrong = tok(wb.line_strong);
    const Rgb fg = tok(wb.fg), fgSoft = tok(wb.fg_soft), muted = tok(wb.muted), dim = tok(wb.dim), accent = tok(wb.accent), accentInk = tok(wb.accent_ink);
    const Rgb accentSoft = tok(wb.accent_soft), errFg = tok(wb.chip_err_fg), warnFg = tok(wb.chip_warn_fg), goodFg = tok(wb.chip_good_fg);
    const Rgb keyBg = tok(wb.key_bg), keyLine = tok(wb.key_line), dangerBg = tok(wb.danger_bg), menu = tok(wb.menu), menuHot = tok(wb.menu_hot);
    const Rgb cardWarn = tok(wb.card_warn), cardWarnLine = tok(wb.card_warn_line), cardErr = tok(wb.card_err), cardErrLine = tok(wb.card_err_line);
    const Rgb sliderTrack = tok(wb.slider_track), sliderFill = tok(wb.slider_fill), sliderKnob = tok(wb.slider_knob), sliderOffFill = tok(wb.slider_disabled_fill);
    const Rgb lampRed = tok(wb.lamp_red), backdrop = tok(wb.backdrop), knobRing = tok(wb.knob_ring);
    // the artboard's own values, once, so a drift of the Workbench filling is caught here
    CHECK(nearColour(panel, rgb(0x1d2026), 0) && nearColour(card, rgb(0x20242b), 0) && nearColour(keyLine, rgb(0x363b45), 0) && nearColour(accent, rgb(0xf5a524), 0));
    CHECK(nearColour(errFg, rgb(0xff9b9b), 0) && nearColour(warnFg, rgb(0xf5c66b), 0) && nearColour(goodFg, rgb(0x7fe6ad), 0) && nearColour(cardWarn, rgb(0x2a2312), 0));

    std::vector<Ev> evs;
    ui->global<Rec>().on_fired([&](slint::SharedString n, slint::SharedString a) { evs.push_back({std::string(n), std::string(a)}); });
    std::vector<Ev> fwd;   // what the default-forwarding bodies send to SignalEvents
    auto& se = ui->global<SignalEvents>();
    se.on_sink_device_selected([&](int i) { fwd.push_back({"sink-device-selected", std::to_string(i)}); });
    se.on_sink_buffer_selected([&](int i) { fwd.push_back({"sink-buffer-selected", std::to_string(i)}); });
    se.on_radio_stereo([&](bool v) { fwd.push_back({"radio-stereo", v ? "1" : "0"}); });
    se.on_ppm_toggled([&](bool v) { fwd.push_back({"ppm-toggled", v ? "1" : "0"}); });
    se.on_ppm_committed([&](slint::SharedString t) { fwd.push_back({"ppm-committed", std::string(t)}); });

    const auto clk = [&](int x, int y) { click(win, static_cast<float>(x), static_cast<float>(y)); };
    const auto mv = [&](int x, int y) { moveTo(win, static_cast<float>(x), static_cast<float>(y)); };
    const auto tap = [&](std::u8string_view k) { keyTap(win, k); };
    const auto shot = [&] { return render(ui, kW, kH); };
    const auto park = [&] { mv(1235, 2395); settle(milliseconds(30)); };
    // click a spot and report the single event it fired ("" when none)
    const auto press = [&](int x, int y) { evs.clear(); fwd.clear(); clk(x, y); shot(); return evs.empty() ? Ev{} : evs.back(); };
    const auto yOf = [&](double v) { return static_cast<int>(std::lround(v)); };

    // =========================================================================================================
    // 1. The sheet at rest
    // =========================================================================================================
    park();
    const Frame f0 = shot();
    CHECK(f0.width == kW && f0.height == kH);
    CHECK(f0.near(1235, 2395, bg, 0));
    f0.savePng(shotDir() + "signal-specimen.png");

    // =========================================================================================================
    // 2. S1 - Source / RTL-SDR (A1): the rows at the artboard's pitch, the controls at the artboard's sizes
    // =========================================================================================================
    const int s1y = yOf(ui->get_s1_y());
    const int x0 = kColA + kBodyX, R = x0 + kBodyW;                  // the body's left and right edge (exclusive)
    const int T = s1y + kSecRow + kBodyPad;                          // the first row's top
    const auto rowY = [&](int k) { return T + k * kPitch; };
    {
        CHECK_NEAR(ui->get_s1_y(), 10.0, 0.01);
        // the Section: a 30 px row, the body on the panel ground, the body's padding (30 at the left, 12 at the right, 4 above)
        CHECK(f0.near(x0 + 3, T - 2, panel, 0));
        CHECK(f0.near(x0 - 2, T + 40, panel, 0) && f0.near(R + 4, T + 40, panel, 0));

        // row 0: the source dropdown (flex) and Refresh (a small key, 24 high), 8 px apart, 24 px tall
        CHECK(f0.near(x0, rowY(0) + 12, keyLine) && f0.near(x0 + 50, rowY(0), keyLine) && f0.near(x0 + 50, rowY(0) + 23, keyLine));
        CHECK(f0.near(x0 + 50, rowY(0) + 3, keyBg) && f0.near(x0 + 50, rowY(0) - 1, panel, 0) && f0.near(x0 + 50, rowY(0) + 24, panel, 0));
        CHECKV(f0.countNear(R - 9, rowY(0) - 3, R - 8, rowY(0) + 27, keyLine, 0) == 2, "Refresh: %d border rows at its right padding", f0.countNear(R - 9, rowY(0) - 3, R - 8, rowY(0) + 27, keyLine, 0));
        CHECK(f0.near(R - 1, rowY(0) + 12, keyLine));
        const int gap0 = gapLeftOf(f0, rowY(0) + 3, R - 1, panel);
        CHECKV(gap0 == 8, "the dropdown and Refresh are %d px apart (the sheet's gap 8)", gap0);
        CHECK(f0.countInk(x0 + 8, rowY(0) + 5, x0 + 130, rowY(0) + 19, keyBg) >= 60);                       // the closed text
        CHECK(f0.countNear(x0 + 8, rowY(0) + 5, x0 + 130, rowY(0) + 19, fg, 60) >= 40);
        // row 1: the checkbox (14 px box, unchecked: field ground, line-strong border) and the xs RADIO SETUP key, 20 px, at the right edge
        const int by = rowY(1) + (kRow - 14) / 2;
        CHECK(f0.near(x0, by + 7, lineStrong) && f0.near(x0 + 13, by + 7, lineStrong) && f0.near(x0 + 7, by, lineStrong) && f0.near(x0 + 7, by + 13, lineStrong));
        CHECK(f0.near(x0 + 3, by + 3, field, 0) && f0.near(x0 - 1, by + 7, panel, 0) && f0.near(x0 + 14, by + 7, panel, 0));
        CHECK(f0.countNear(x0 + 22, rowY(1) + 4, x0 + 170, rowY(1) + 20, fg, 60) >= 30);                      // "Look for network USRPs"
        CHECKV(f0.countNear(R - 9, rowY(1) - 3, R - 8, rowY(1) + 27, keyLine, 0) == 2, "RADIO SETUP: %d border rows", f0.countNear(R - 9, rowY(1) - 3, R - 8, rowY(1) + 27, keyLine, 0));
        CHECK(f0.firstRowNear(R - 9, rowY(1) - 3, rowY(1) + 27, keyLine, 0) == rowY(1) + 2);                       // xs: 20 high, centred in the 24 row
        CHECK(f0.near(R - 1, rowY(1) + 12, keyLine));
        // row 2: "Rate", a 108 px dropdown, "Actual", the mono readout flush right
        CHECK(f0.countNear(x0, rowY(2) + 5, x0 + 30, rowY(2) + 19, fgSoft, 60) >= 8);
        const int rl = firstX(f0, rowY(2) + 12, x0 + 10, R, keyLine);
        CHECKV(rl > x0 + 14 && rl < x0 + 50, "the rate dropdown starts at %d", rl);
        CHECK(f0.near(rl + 107, rowY(2) + 12, keyLine) && !f0.near(rl + 108, rowY(2) + 12, keyLine) && !f0.near(rl - 1, rowY(2) + 12, keyLine));      // 108 wide
        CHECK(f0.countNear(rl + 120, rowY(2) + 4, R, rowY(2) + 20, dim, 40) >= 10);                                // "Actual"
        const Ink act = inkBox(f0, rl + 150, rowY(2) + 2, R + 6, rowY(2) + 22, panel);
        CHECKV(act.n >= 40 && act.w() >= 58 && act.x1 >= R - 8 && act.x1 <= R, "actual-rate readout ends at %d (the body ends at %d)", act.x1, R);
        // row 3: "Antenna" and a 132 px dropdown
        const int al = firstX(f0, rowY(3) + 12, x0 + 10, R, keyLine);
        CHECK(f0.near(al + 131, rowY(3) + 12, keyLine) && !f0.near(al + 132, rowY(3) + 12, keyLine) && !f0.near(al - 1, rowY(3) + 12, keyLine));       // 132 wide
        CHECK(f0.countNear(x0, rowY(3) + 5, x0 + 48, rowY(3) + 19, fgSoft, 60) >= 15);
        // row 4: Auto gain and Bias tee, one box each, half a row apart (123 + 8)
        const int bx2 = firstX(f0, rowY(4) + 12, x0 + 60, R, lineStrong, 0);
        CHECKV(bx2 > x0 + 90 && bx2 < x0 + 160, "the Bias tee box starts at %d", bx2);
        for (const int bx : {x0, bx2}) {
            const int y4 = rowY(4) + (kRow - 14) / 2;
            CHECKV(f0.near(bx, y4 + 7, lineStrong) && f0.near(bx + 3, y4 + 3, field, 0) && f0.near(bx - 1, y4 + 7, panel, 0), "tick box at %d", bx);
        }
        // row 5: TUNER - the name, a 6 px track from x0 + 64 to x0 + 184 with the fill to 66 %, the knob, the mono readout flush right
        const int ty = rowY(5) + 12;
        CHECK(f0.countNear(x0, rowY(5) + 5, x0 + 56, rowY(5) + 19, fgSoft, 60) >= 15);
        CHECK(f0.near(x0 + 64 + 100, ty, sliderTrack, 0) && f0.near(x0 + 64 + 100, ty - 3, sliderTrack, 0) && f0.near(x0 + 64 + 100, ty + 2, sliderTrack, 0));
        CHECK(f0.near(x0 + 64 + 100, ty - 4, panel, 0) && f0.near(x0 + 64 + 100, ty + 3, panel, 0));                    // 6 px high
        CHECK(f0.near(x0 + 64 + 30, ty, sliderFill, 0) && f0.near(x0 + 64 + 2, ty, sliderFill, 0) && f0.near(x0 + 63, ty, panel, 0));
        const int trackEnd = lastX(f0, ty, x0 + 64 + 100, R, sliderTrack, 0);
        CHECKV(trackEnd >= R - 70 - 4 && trackEnd <= R - 70, "the track ends at %d (readout 62 + 8 of air before the body's edge %d)", trackEnd, R);
        CHECK(f0.near(x0 + 64 + 79, ty, sliderKnob, 0) && f0.near(x0 + 64 + 79, ty - 4, sliderKnob, 6));                // the 66 % knob, 16 px round
        const Ink rd = inkBox(f0, x0 + 64 + 128, rowY(5) + 2, R + 6, rowY(5) + 22, panel);
        CHECKV(rd.n >= 25 && rd.x1 >= R - 3 && rd.x1 <= R, "TUNER readout ends at %d", rd.x1);
        CHECK(f0.countNear(x0 + 64 + 128, rowY(5) + 3, R, rowY(5) + 21, fg, 60) >= 20);
        // the Frequency correction caption (a cap, muted) sits above its tick, and the tick is checked (accent)
        const Ink cap = inkBox(f0, x0, rowY(6) - 2, R, rowY(6) + 14, panel);
        CHECKV(cap.n >= 80 && cap.h() <= 12, "the FREQUENCY CORRECTION cap: %d ink, %d high", cap.n, cap.h());
        CHECK(f0.countNear(x0, rowY(6) - 2, R, rowY(6) + 14, muted, 40) >= 40);
        const int ppmTick = rowY(6) + 13 + 6;                                                                              // cap line (13) + 6
        CHECKV(f0.countNear(x0, ppmTick, x0 + 14, ppmTick + 24, accent, 0) >= 100, "the PPM tick is checked: %d accent pixels", f0.countNear(x0, ppmTick, x0 + 14, ppmTick + 24, accent, 0));
    }

    // the closing lines: the ppm refusal and the mismatch note amber, the source error red, the how-applied note dim above them
    {
        const int bodyBottom = s1y + kSecRow + yOf(ui->get_s1_body_h()) - kBodyPadBottom;     // the body's last pixel row + 1
        const auto ls = textLines(f0, x0, R, rowY(6), bodyBottom + 2, panel);
        CHECKV(ls.size() >= 7, "%zu lines of text under the TUNER row", ls.size());
        const auto at = [&](std::size_t back) { return ls.size() >= back ? ls[ls.size() - back] : std::pair<int, int>{-5, -4}; };
        const auto err = at(1), mis = at(2), ref = at(3), note = at(4), who = at(6);
        CHECKV(f0.countNear(x0, err.first, R, err.second, errFg, 40) >= 50, "the source error in chip-err-fg: %d", f0.countNear(x0, err.first, R, err.second, errFg, 40));
        CHECKV(f0.countNear(x0, mis.first, R, mis.second, warnFg, 40) >= 50, "the tune-mismatch note amber: %d", f0.countNear(x0, mis.first, R, mis.second, warnFg, 40));
        CHECKV(f0.countNear(x0, ref.first, R, ref.second, warnFg, 40) >= 50, "the ppm refusal amber: %d", f0.countNear(x0, ref.first, R, ref.second, warnFg, 40));
        CHECKV(f0.countNear(x0, note.first, R, note.second, dim, 30) >= 50, "the how-applied note in dim: %d", f0.countNear(x0, note.first, R, note.second, dim, 30));
        CHECKV(f0.countNear(x0, who.first, R, who.second, dim, 30) >= 50, "the whose-radio line in dim");
        CHECK(f0.countNear(x0, mis.first, R, err.second, lampRed, 40) == 0 && f0.countNear(x0, err.first, R, err.second, warnFg, 10) == 0);   // amber is not red
        // 6 px between the lines (the sheet's gap): one text line is 13 px of box, the next starts 6 px under it
        CHECKV(err.first - mis.first >= 18 && err.first - mis.first <= 21 && mis.first - ref.first >= 18 && mis.first - ref.first <= 21, "line pitch %d / %d", err.first - mis.first, mis.first - ref.first);
        CHECK(f0.near(x0 + 100, bodyBottom + 4, panel, 0) && f0.near(x0 + 100, bodyBottom + 11, line, 0));                  // 12 px under the last line: the Section's hairline
        // the mono number field of the ppm block: right-aligned figures, "ppm" and the two step arrows at the right
        const int fieldTop = fieldTopAt(f0, x0 + 40, rowY(6) + 20, bodyBottom, lineStrong, field);
        CHECKV(fieldTop > 0 && f0.near(x0 + 40, fieldTop + 23, lineStrong, 0) && f0.near(x0 + 40, fieldTop + 24, panel, 0), "the ppm field is 24 high at %d", fieldTop);
        const Ink fig = inkBox(f0, x0 + 60, fieldTop + 3, x0 + 190, fieldTop + 21, field);
        CHECKV(fig.n >= 15 && fig.x1 >= x0 + 150, "the figure is right-aligned in the field: ink to %d", fig.x1);
        CHECK(f0.countInk(x0 + 214, fieldTop + 1, R, fieldTop + 23, panel) >= 20);                       // "ppm" + the arrows
        CHECKV(f0.countNear(x0 + 195, fieldTop + 4, x0 + 224, fieldTop + 20, dim, 40) >= 12, "the unit 'ppm' in dim");
    }

    // =========================================================================================================
    // 3. Rule 11: no body asks for more than the column
    // =========================================================================================================
    CHECKV(ui->get_p1_w() <= 254.01 && ui->get_p2_w() <= 254.01 && ui->get_p3_w() <= 254.01 && ui->get_p4_w() <= 254.01 && ui->get_p5_w() <= 254.01,
           "bare bodies in a 254 px cell: %.1f %.1f %.1f %.1f %.1f", ui->get_p1_w(), ui->get_p2_w(), ui->get_p3_w(), ui->get_p4_w(), ui->get_p5_w());

    // =========================================================================================================
    // 4. The variants (A6): a card for its own variant number and no other
    // =========================================================================================================
    const auto cardPixels = [&](const Frame& f, int sx, int y0, int y1) { return f.countNear(sx + kBodyX, y0, sx + kBodyX + kBodyW, y1, card, 0); };
    {
        // S1 is variant 0: no card ground anywhere in its body
        CHECK(cardPixels(f0, kColA, s1y + kSecRow, s1y + kSecRow + yOf(ui->get_s1_body_h())) == 0);
        // the I/Q file (d6): the card is the card ground with a line-strong border, 8 px corners, the tag in accent-soft, the path field and Open
        const int y6 = yOf(ui->get_d6_y()), t6 = y6 + kSecRow + kBodyPad;
        const int cardTop = t6 + 2 * kPitch;                                                                  // under the device and network rows
        CHECKV(f0.near(kColD + kBodyX + 20, cardTop, lineStrong) && f0.near(kColD + kBodyX + 20, cardTop + 3, card, 0), "the I/Q file card starts at %d", cardTop);
        CHECK(f0.near(kColD + kBodyX, cardTop + 30, lineStrong) && f0.near(kColD + kBodyX + kBodyW - 1, cardTop + 30, lineStrong));            // full width
        CHECK(f0.near(kColD + kBodyX, cardTop, panel, 0));                                                                                       // a rounded corner
        const Ink tag = inkBox(f0, kColD + kBodyX + 9, cardTop + 4, kColD + kBodyX + 120, cardTop + 17, card);
        CHECKV(tag.n >= 40 && tag.h() <= 10, "the card's tag: %d ink, %d high", tag.n, tag.h());
        CHECK(f0.countNear(kColD + kBodyX + 9, cardTop + 4, kColD + kBodyX + 120, cardTop + 17, accentSoft, 40) >= 30);
        const int fy = cardTop + 1 + 6 + 13 + 4;                                                              // border, padding, the tag line, 4
        CHECKV(f0.near(kColD + kBodyX + 40, fy, lineStrong) && f0.near(kColD + kBodyX + 40, fy + 23, lineStrong), "the path field is 24 high at %d", fy);
        CHECK(f0.near(kColD + kBodyX + 40, fy + 3, field, 0));
        CHECKV(f0.countNear(kColD + kBodyX + kBodyW - 9 - 60, fy + 1, kColD + kBodyX + kBodyW - 8, fy + 23, keyLine) >= 2, "an xs Open key at the right");
        CHECK(f0.countNear(kColD + kBodyX + 9, fy + 26, kColD + kBodyX + kBodyW, fy + 50, dim, 40) >= 60);                                      // the slow-disk line, dim
        // the other variants' cards exist where they should: Pluto, rtl_tcp, the radio extras, the sound cards, the Airspy
        for (const double y : {ui->get_d7_y(), ui->get_d8_y(), ui->get_d9_y(), ui->get_b1_y(), ui->get_b2_y(), ui->get_b3_y(), ui->get_b4_y(), ui->get_c1_y()}) {
            const int sx = (y == ui->get_d7_y() || y == ui->get_d8_y() || y == ui->get_d9_y()) ? kColD : (y == ui->get_c1_y() ? kColC : kColB);
            CHECKV(cardPixels(f0, sx, yOf(y) + kSecRow, yOf(y) + kSecRow + 400) > 400, "a variant card at y %.0f", y);
        }
        // the Device-stopped card is red (card-err ground and line, red tag, the red sentence), the Advisories card amber
        const int y10 = yOf(ui->get_d10_y()), t10 = y10 + kSecRow + kBodyPad;
        CHECK(f0.near(kColD + kBodyX + 20, t10, cardErrLine) && f0.near(kColD + kBodyX + 20, t10 + 4, cardErr, 0));
        CHECKV(f0.countNear(kColD + kBodyX + 9, t10 + 4, kColD + kBodyX + 160, t10 + 17, errFg, 40) >= 30, "the Device stopped tag in red");
        CHECKV(f0.countNear(kColD + kBodyX + 9, t10 + 20, R + kColD - kColA, t10 + 56, errFg, 40) >= 100, "the stopped sentence in red");
        CHECKV(f0.countNear(kColD + kBodyX + 9, t10 + 56, kColD + kBodyX + kBodyW, t10 + 100, dim, 40) >= 80, "the reconnect advice in dim");
        int amberTop = f0.firstRowNear(kColD + kBodyX + 20, t10 + 60, t10 + 140, cardWarnLine);
        CHECKV(amberTop > 0 && f0.near(kColD + kBodyX + 20, amberTop + 4, cardWarn, 0), "the Advisories card is the amber one (top at %d)", amberTop);
        CHECK(f0.countNear(kColD + kBodyX + 9, amberTop + 4, kColD + kBodyX + kBodyW, amberTop + 60, warnFg, 40) >= 100);
    }
    {
        // a model change redraws a variant: the same Source body takes every variant number, and only that variant's card appears
        auto m = ui->get_m_rtl();
        const int h0 = yOf(ui->get_s1_body_h());
        for (int v = 0; v <= 7; ++v) {
            m.variant = v;
            m.options = std::make_shared<slint::VectorModel<OptionRow>>(std::vector<OptionRow>{OptionRow{"fm", "FM notch", true, true, ""}});
            m.cards = std::make_shared<slint::VectorModel<SelectItem>>(std::vector<SelectItem>{SelectItem{"Line in", "a", true, ""}});
            m.converter_shown = false;
            ui->set_m_rtl(m);
            const Frame f = shot();
            const int bodyEnd = s1y + kSecRow + yOf(ui->get_s1_body_h());
            const int n = cardPixels(f, kColA, s1y + kSecRow, bodyEnd);
            if (v == 0) { CHECKV(n == 0, "variant 0 draws no card (%d)", n); }
            else { CHECKV(n > 500, "variant %d draws its card (%d card pixels)", v, n); }
            // the plain radio rows (Rate, Antenna, gain) belong to variants 0, 6 and 7 (the Airspy has its own panel instead): a file, a Pluto, an rtl_tcp and a sound card have no TUNER slider
            int tuner = 0;
            for (int y = s1y + kSecRow; y < bodyEnd; ++y) { if (f.near(x0 + 64 + 30, y, sliderFill, 0)) { ++tuner; } }
            if (v == 0 || v == 6 || v == 7) { CHECKV(tuner >= 5, "variant %d keeps the gain row (%d fill px)", v, tuner); }
            else { CHECKV(tuner == 0, "variant %d has no gain row (%d fill px)", v, tuner); }
        }
        m.variant = 0;
        m.options = std::make_shared<slint::VectorModel<OptionRow>>();
        m.cards = std::make_shared<slint::VectorModel<SelectItem>>();
        ui->set_m_rtl(m);
        const Frame f = shot();
        CHECK(std::abs(yOf(ui->get_s1_body_h()) - h0) <= 1);
        CHECK(cardPixels(f, kColA, s1y + kSecRow, s1y + kSecRow + yOf(ui->get_s1_body_h())) == 0);
    }

    // =========================================================================================================
    // 5. The Source body's controls: every callback through a real pointer event
    // =========================================================================================================
    const auto popupRow = [&](int keyX, int keyY, int i) { return std::pair<int, int>{keyX + 30, keyY + kRow + 2 + 5 + i * 26 + 13}; };   // a list's row i under a key at (keyX, keyY)
    park();
    {
        // --- the source dropdown: a click opens the list under the key (menu ground, 26 px rows), a click on a row picks it
        shot();
        clk(x0 + 60, rowY(0) + 12);
        const Frame o = shot();
        CHECKV(o.near(x0 + 120, rowY(0) + 26 + 8, menu, 0), "the list opens under the key: %s", hex(o.at(x0 + 120, rowY(0) + 34)).c_str());
        CHECK(o.near(x0, rowY(0) + 26 + 40, knobRing, 3));                                          // its border
        // the selected row (RTL-SDR Blog V4, index 3) carries a tick mark in the accent; the greyed row (Airspy R2) is dim with its note
        const int r3 = rowY(0) + kRow + 2 + 5 + 3 * 26, r4 = rowY(0) + kRow + 2 + 5 + 4 * 26;
        CHECKV(o.countNear(x0 + 5, r3 + 2, x0 + 22, r3 + 24, accent, 30) >= 6, "the chosen row has the tick: %d", o.countNear(x0 + 5, r3 + 2, x0 + 22, r3 + 24, accent, 30));
        CHECK(o.countNear(x0 + 26, r4 + 4, x0 + 120, r4 + 22, tok(wb.disabled), 40) >= 20);
        CHECK(o.countNear(x0 + 150, r4 + 4, x0 + 250, r4 + 22, dim, 40) >= 10);                         // the note "(in use by the patch)"
        mv(x0 + 80, r3 + 13);
        CHECK(shot().near(x0 + 120, r3 + 3, menuHot, 0));                                               // the pointer row lights
        // a greyed row neither picks nor closes the list
        evs.clear();
        clk(x0 + 60, r4 + 13);
        const Frame still = shot();
        CHECKV(evs.empty() && still.near(x0 + 120, rowY(0) + 34, menu, 0), "a greyed row does nothing (%zu events)", evs.size());
        const Ev e = press(x0 + 60, rowY(0) + kRow + 2 + 5 + 13);                                       // row 0
        CHECKV(e.name == "source-device-selected" && e.args == "0", "picked row 0: '%s' '%s'", e.name.c_str(), e.args.c_str());
        CHECK(!shot().near(x0 + 120, rowY(0) + 34, menu, 0));                                              // the list closed
        park();
        // a click outside closes it without picking
        clk(x0 + 60, rowY(0) + 12);
        shot();
        evs.clear();
        clk(R + 100, rowY(2));
        CHECK(evs.empty() && !shot().near(x0 + 120, rowY(0) + 34, menu, 0));
        park();

        // --- Refresh, the network tick, RADIO SETUP
        Ev e2 = press(R - 20, rowY(0) + 12);
        CHECKV(e2.name == "source-refresh", "Refresh: '%s'", e2.name.c_str());
        e2 = press(x0 + 60, rowY(1) + 12);
        CHECKV(e2.name == "source-usrps" && e2.args == "1", "network tick: '%s' '%s'", e2.name.c_str(), e2.args.c_str());
        CHECK(!shot().near(x0 + 3, rowY(1) + 5 + 3, accent, 10));                                       // the model says off: the box springs back
        e2 = press(R - 20, rowY(1) + 12);
        CHECKV(e2.name == "source-setup", "RADIO SETUP: '%s'", e2.name.c_str());

        // --- Rate and Antenna dropdowns: the list opens, row i picks i
        const int rl = firstX(f0, rowY(2) + 12, x0 + 10, R, keyLine);
        clk(rl + 40, rowY(2) + 12);
        shot();
        auto pr = popupRow(rl, rowY(2), 3);
        Ev e3 = press(pr.first, pr.second);
        CHECKV(e3.name == "source-rate-selected" && e3.args == "3", "rate row 3: '%s' '%s'", e3.name.c_str(), e3.args.c_str());
        const int al = firstX(f0, rowY(3) + 12, x0 + 10, R, keyLine);
        clk(al + 40, rowY(3) + 12);
        shot();
        pr = popupRow(al, rowY(3), 1);
        e3 = press(pr.first, pr.second);
        CHECKV(e3.name == "source-antenna-selected" && e3.args == "1", "antenna row 1: '%s' '%s'", e3.name.c_str(), e3.args.c_str());
        park();

        // --- Auto gain and Bias tee: the host's answer is awaited, the box stays where the model has it
        e3 = press(x0 + 40, rowY(4) + 12);
        CHECKV(e3.name == "source-auto-gain" && e3.args == "1", "auto gain: '%s' '%s'", e3.name.c_str(), e3.args.c_str());
        CHECK(!shot().near(x0 + 3, rowY(4) + 5 + 3, accent, 10));
        e3 = press(x0 + 131 + 40, rowY(4) + 12);
        CHECKV(e3.name == "source-bias" && e3.args == "1", "bias tee: '%s' '%s'", e3.name.c_str(), e3.args.c_str());

        // --- the TUNER slider: a drag reports (stage 0, fraction)
        evs.clear();
        const int track0 = x0 + 64, knobX = track0 + 79, sy = rowY(5) + 12;
        press(1235, 2395);
        evs.clear();
        mv(knobX, sy);
        ::press(win, static_cast<float>(knobX), static_cast<float>(sy));
        mv(track0 + 60, sy);
        shot();
        ::release(win, static_cast<float>(track0 + 60), static_cast<float>(sy));
        bool sawGain = false;
        std::string lastGain;
        for (const Ev& g : evs) { if (g.name == "source-gain-changed") { sawGain = true; lastGain = g.args; } }
        CHECK(sawGain);
        {
            const std::size_t sp = lastGain.find(' ');
            CHECKV(lastGain.substr(0, sp) == "0", "the stage index is 0: '%s'", lastGain.c_str());
            CHECKV(std::abs(num(lastGain.substr(sp + 1)) - 0.5) < 0.03, "dragged to 60/120 = 0.5: '%s'", lastGain.c_str());
        }
        // the host did not move the stage, so the knob is where the user put it until the model changes (the slider's rule)
        park();
    }

    // --- the ppm block: its tick, its field (Enter commits, the focus leaving commits), its arrows
    {
        const int ppmTick = rowY(6) + 13 + 6;
        Ev e = press(x0 + 60, ppmTick + 7);
        CHECKV(e.name == "ppm-toggled" && e.args == "0", "ppm tick (the model says on): '%s' '%s'", e.name.c_str(), e.args.c_str());
        const int noteTop = yOf(ui->get_s1_y()) + kSecRow + yOf(ui->get_s1_body_h()) - kBodyPadBottom - 14 - 6 - 14 - 6 - 14 - 6 - 14;
        const int fy = noteTop - 6 - 24 + 12;
        evs.clear();
        clk(x0 + 80, fy);
        tap(slint::platform::key_codes::End);
        for (int i = 0; i < 6; ++i) { tap(slint::platform::key_codes::Backspace); }
        for (const char c : std::string("12.5")) { const char8_t u[2] = {static_cast<char8_t>(c), 0}; tap(std::u8string_view(u, 1)); }
        CHECKV(evs.empty(), "typing commits nothing (%zu)", evs.size());
        tap(slint::platform::key_codes::Return);
        CHECKV(!evs.empty() && evs.back().name == "ppm-committed" && evs.back().args == "12.5", "Enter commits '12.5': %zu events, '%s'", evs.size(), evs.empty() ? "" : evs.back().args.c_str());
        const std::size_t after = evs.size();
        clk(R + 200, 4);                                                                                  // away: nothing was edited since, so nothing more
        CHECKV(evs.size() == after, "Enter then a click away commits once (%zu -> %zu)", after, evs.size());
        clk(x0 + 80, fy);
        tap(u8"7");
        const int fc1Field = fieldTopAt(shot(), x0 + 40, yOf(ui->get_fc1_y()) + kSecRow, yOf(ui->get_fc1_y()) + kSecRow + yOf(ui->get_fc1_body_h()), lineStrong, field);
        clk(x0 + 80, fc1Field + 12);                                                                     // the focus moves to another field with an edit pending
        CHECKV(evs.size() == after + 1 && evs.back().name == "ppm-committed" && evs.back().args.find('7') != std::string::npos, "the focus leaving commits: '%s'", evs.empty() ? "" : evs.back().args.c_str());
        // a model change replaces the typed text (the host's number wins)
        auto m = ui->get_m_rtl();
        m.ppm_text = "+3.0";
        ui->set_m_rtl(m);
        shot();
        evs.clear();
        clk(x0 + 80, fy);
        tap(slint::platform::key_codes::Return);
        CHECKV(!evs.empty() && evs.back().args == "+3.0", "the field shows the model's text: '%s'", evs.empty() ? "" : evs.back().args.c_str());
        clk(R + 200, 4);
        // the arrows: the field's figure plus or minus a tenth
        const int ax = R - 10;
        evs.clear();
        clk(ax, fy - 4);
        CHECKV(!evs.empty() && evs.back().name == "ppm-committed" && std::abs(num(evs.back().args) - 3.1) < 0.001, "the up arrow: '%s'", evs.empty() ? "" : evs.back().args.c_str());
        evs.clear();
        clk(ax, fy + 4);
        CHECKV(!evs.empty() && evs.back().name == "ppm-committed" && std::abs(num(evs.back().args) - 2.9) < 0.001, "the down arrow: '%s'", evs.empty() ? "" : evs.back().args.c_str());
        m.ppm_text = "+0.0";
        ui->set_m_rtl(m);
        park();
    }

    // --- Frequency correction as its own body (fc1): it forwards to SignalEvents
    {
        const int fy0 = yOf(ui->get_fc1_y()) + kSecRow + kBodyPad;
        fwd.clear();
        clk(x0 + 60, fy0 + 7);
        CHECKV(!fwd.empty() && fwd.back().name == "ppm-toggled" && fwd.back().args == "0", "fc1 tick forwards to SignalEvents");
        park();
        const Frame f = shot();
        CHECKV(f.countNear(x0, fy0 + 5, x0 + 14, fy0 + 19, accent, 40) >= 100, "fc1 tick: %d accent px at y %d (the box is ticked while the model says on); ppm_on=%d", f.countNear(x0, fy0 + 5, x0 + 14, fy0 + 19, accent, 40), fy0, static_cast<int>(ui->get_m_rtl().ppm_on));
    }

    // =========================================================================================================
    // 6. The sound card (b1: I/Q pair, b2: real audio): Device + Format, Swap I/Q, Centre, Sample rate + Open
    // =========================================================================================================
    {
        const int xb = kColB + kBodyX, Tb = yOf(ui->get_b1_y()) + kSecRow + kBodyPad;
        const int cardTop = Tb + 2 * kPitch, l0 = cardTop + 1 + 6 + 13 + 4;              // the card's first line (border, padding, the 13 px tag, 4)
        const int fr = xb + kBodyW - 9;                                                  // the card's inner right edge, exclusive
        const Frame f = shot();
        CHECKV(f.near(xb + 20, cardTop, lineStrong, 0) && f.near(xb + 20, cardTop + 3, card, 0), "the sound card's card starts at %d", cardTop);
        CHECK(f.countNear(xb + 9, cardTop + 4, xb + 120, cardTop + 17, accentSoft, 40) >= 30);                    // the tag SOUND CARD
        // the Format dropdown is 104 wide at the right of the Device dropdown, 8 px between
        CHECK(f.near(fr - 1, l0 + 12, keyLine) && f.near(fr - 104, l0 + 12, keyLine) && !f.near(fr - 105, l0 + 12, keyLine));
        CHECKV(gapLeftOf(f, l0 + 3, fr - 3, card) == 8, "Device and Format are %d px apart", gapLeftOf(f, l0 + 3, fr - 3, card));
        CHECK(f.near(xb + 9, l0 + 12, keyLine));                                                                 // the Device dropdown starts at the card's inner edge
        // I/Q: Swap I/Q on a line of its own (a 14 px box, unticked), then Centre (MHz) with the mono field (82 wide) at the right
        CHECK(f.near(xb + 9, l0 + 28 + 12, lineStrong) && f.near(xb + 12, l0 + 28 + 8, field, 0));
        const int cy = l0 + 2 * 28;                                                        // a card keeps 4 px between its lines
        CHECK(f.near(fr - 1, cy + 12, lineStrong) && f.near(fr - 82, cy + 12, lineStrong) && !f.near(fr - 83, cy + 12, lineStrong));
        CHECK(f.countNear(xb + 9, cy + 5, xb + 90, cy + 19, fgSoft, 60) >= 25);
        CHECK(f.countNear(fr - 70, cy + 4, fr - 6, cy + 20, fg, 60) >= 15);                                      // "7.100", right-aligned
        // the events
        clk(xb + 9 + 30, l0 + 12);
        shot();
        auto pr = popupRow(xb + 9, l0, 1);
        Ev e = press(pr.first, pr.second);
        CHECKV(e.name == "card-selected" && e.args == "1", "card row 1: '%s' '%s'", e.name.c_str(), e.args.c_str());
        clk(fr - 104 + 30, l0 + 12);
        shot();
        pr = popupRow(fr - 104, l0, 0);
        e = press(pr.first, pr.second);
        CHECKV(e.name == "card-format" && e.args == "0", "format row 0: '%s' '%s'", e.name.c_str(), e.args.c_str());
        e = press(xb + 9 + 40, l0 + 28 + 12);
        CHECKV(e.name == "card-swap" && e.args == "1", "swap: '%s' '%s'", e.name.c_str(), e.args.c_str());
        evs.clear();
        clk(fr - 30, cy + 12);
        tap(slint::platform::key_codes::End);
        for (int i = 0; i < 6; ++i) { tap(slint::platform::key_codes::Backspace); }
        tap(u8"1"); tap(u8"4"); tap(u8".");  tap(u8"2");
        tap(slint::platform::key_codes::Return);
        CHECKV(!evs.empty() && evs.back().name == "card-centre-committed" && evs.back().args == "14.2", "centre: '%s'", evs.empty() ? "" : evs.back().args.c_str());
        clk(R + 200, 4);
        // Sample rate dropdown (108 wide) and Open (xs) on the last line
        const int ry = l0 + 3 * 28;
        const Frame g0 = shot();
        const int rl = firstX(g0, ry + 12, xb + 9 + 50, fr, keyLine);
        CHECKV(rl > 0 && g0.near(rl + 107, ry + 12, keyLine) && !g0.near(rl + 108, ry + 12, keyLine), "the sample-rate dropdown is 108 wide (left %d)", rl);
        clk(rl + 40, ry + 12);
        shot();
        pr = popupRow(rl, ry, 2);
        e = press(pr.first, pr.second);
        CHECKV(e.name == "card-rate-selected" && e.args == "2", "card rate row 2: '%s' '%s'", e.name.c_str(), e.args.c_str());
        e = press(fr - 12, ry + 12);
        CHECKV(e.name == "card-open", "Open: '%s'", e.name.c_str());
        {   // with no sound card chosen (docs 6.61) the Open key is greyed: drawn as a disabled key, and it asks for nothing
            auto sm = ui->get_m_card_iq();
            const Frame live = shot();
            sm.card_can_open = false;
            ui->set_m_card_iq(sm);
            const Frame grey = shot();
            int changed = 0;
            for (int x = fr - 40; x < fr; ++x) { const auto a = grey.at(x, ry + 12), b = live.at(x, ry + 12); if (a.r != b.r || a.g != b.g || a.b != b.b) { ++changed; } }
            CHECKV(changed > 10, "a greyed Open is drawn differently (%d pixels changed)", changed);
            e = press(fr - 12, ry + 12);
            CHECKV(e.name.empty(), "a greyed Open asks for nothing: '%s'", e.name.c_str());
            sm.card_can_open = true;
            ui->set_m_card_iq(sm);
            shot();
        }
        CHECKV(g0.countNear(xb + 9, ry + 30, fr, ry + 46, dim, 40) >= 60, "the Receives line is dim");
        park();
        // the second card (b2, real audio): amber busy and none lines, the Channel line, no Swap I/Q and no Centre, the missing-card note, the converter card
        const int Tb2 = yOf(ui->get_b2_y()) + kSecRow + kBodyPad, c2 = Tb2 + 2 * kPitch;
        const Frame g = shot();
        CHECKV(g.countNear(xb + 9, c2 + 24, fr, c2 + 24 + 30, warnFg, 40) >= 100, "the busy and none lines are amber: %d", g.countNear(xb + 9, c2 + 24, fr, c2 + 24 + 30, warnFg, 40));
        int bottom = -1;
        for (int y = c2 + 60; y < c2 + 400 && bottom < 0; ++y) { if (g.near(xb + 8, y, lineStrong, 0) && g.near(xb + 8, y - 1, card, 0)) { bottom = y; } }
        CHECKV(bottom > 0 && g.countNear(xb + 9, bottom - 34, fr, bottom, warnFg, 40) >= 100, "the missing-card note is amber (card bottom %d)", bottom);
        CHECK(g.countNear(xb + 9, c2 + 24 + 34 + 28, xb + 90, c2 + 24 + 34 + 28 + 24, fgSoft, 60) >= 25);   // Channel, under the Device line
        // the converter card under it: Off, a mode dropdown and nothing else
        const Frame h = shot();
        int convTop = -1;
        for (int y = bottom + 2; y < bottom + 40 && convTop < 0; ++y) { if (h.near(xb + 20, y, lineStrong, 0) && h.near(xb + 20, y + 3, card, 0)) { convTop = y; } }
        CHECKV(convTop > 0 && convTop - bottom == 7, "the converter card follows the sound card by 6 px (%d)", convTop - bottom);
        CHECK(h.near(xb + 9, convTop + 24 + 12, keyLine) && h.near(xb + 9 + 123, convTop + 24 + 12, keyLine));      // the mode dropdown, 124 wide
        const int convBottom = yOf(ui->get_b2_y()) + kSecRow + yOf(ui->get_b2_body_h()) - kBodyPadBottom;
        CHECKV(convBottom - (convTop + 24) == 24 + 7, "the converter card holds one 24 px line (%d)", convBottom - (convTop + 24));
        park();
    }

    // =========================================================================================================
    // 7. The Airspy (b3: Sensitive, b4: Free) and the converter (c1)
    // =========================================================================================================
    {
        const int xb = kColB + kBodyX, T3 = yOf(ui->get_b3_y()) + kSecRow + kBodyPad;
        const int fr = xb + kBodyW - 9;
        const Frame f = shot();
        // Rate row, the single port as text, then Bias tee alone on its line, ticked (the model says on)
        CHECKV(f.countNear(xb, T3 + 4 * kPitch + 5, xb + 14, T3 + 4 * kPitch + 19, accent, 0) >= 100, "Bias tee is ticked: the model says on");
        CHECK(f.countNear(xb, T3 + 3 * kPitch + 5, xb + 56, T3 + 3 * kPitch + 19, fgSoft, 60) >= 15);                // "Antenna"
        // the Airspy card: tag, Decimation (a 72 px dropdown at the right), the dim hardware-rate line, GAIN MODE with three radios, one lit
        const int cardTop = T3 + 5 * kPitch, l0 = cardTop + 1 + 6 + 13 + 4;
        CHECKV(f.near(xb + 20, cardTop, lineStrong, 0) && f.near(xb + 20, cardTop + 4, card, 0), "the Airspy card starts at %d", cardTop);
        CHECK(f.near(fr - 72, l0 + 12, keyLine) && f.near(fr - 1, l0 + 12, keyLine) && !f.near(fr - 73, l0 + 12, keyLine));
        CHECK(f.countNear(xb + 9, l0 + 24 + 4, fr, l0 + 24 + 4 + 14, dim, 40) >= 60);                                  // radio at 10 MS/s, decimated by 2
        int ringTop = -1;
        for (int y = l0 + 40; y < l0 + 140 && ringTop < 0; ++y) { if (f.near(xb + 9 + 7, y, accent, 0)) { ringTop = y; } }
        CHECKV(ringTop > 0, "the lit radio's ring top at %d", ringTop);
        int litRun = 0;
        for (int x = xb + 9; x < fr; ++x) { if (f.near(x, ringTop + 7, accent, 0)) { ++litRun; } }
        CHECKV(litRun >= 4 && litRun <= 12, "one radio is lit (accent run %d on the middle row)", litRun);
        CHECK(f.countNear(xb + 9, ringTop - 5 - 17, xb + 120, ringTop - 5 - 4, muted, 40) >= 30);                      // the GAIN MODE cap above the radios
        std::vector<int> rings;
        for (int x = xb + 9 + 20; x < fr; ++x) { if (f.near(x, ringTop + 7, lineStrong, 8) && f.near(x + 2, ringTop + 7, field, 4)) { rings.push_back(x); } }
        CHECKV(rings.size() == 2, "two unlit radios (%zu)", rings.size());
        Ev e = press(xb + 9 + 7, ringTop + 7);
        CHECKV(e.name == "airspy-gain-mode" && e.args == "0", "Sensitive: '%s' '%s'", e.name.c_str(), e.args.c_str());
        if (rings.size() == 2) {
            e = press(rings[0] + 7, ringTop + 7);
            CHECKV(e.name == "airspy-gain-mode" && e.args == "1", "Linear: '%s' '%s'", e.name.c_str(), e.args.c_str());
            e = press(rings[1] + 7, ringTop + 7);
            CHECKV(e.name == "airspy-gain-mode" && e.args == "2", "Free: '%s' '%s'", e.name.c_str(), e.args.c_str());
        }
        // the Gain slider on the next line: a click on the track at 25 % reports about 0.25
        const int gy = ringTop - 5 + 24 + 4 + 12;
        const int gtrack = xb + 9 + 56 + 8;
        evs.clear();
        clk(gtrack + 25, gy);
        bool saw = false;
        for (const Ev& g : evs) { if (g.name == "airspy-gain") { saw = true; CHECKV(std::abs(num(g.args) - 0.25) < 0.1, "gain at 25 / 102: '%s'", g.args.c_str()); } }
        CHECKV(saw, "the Gain slider reports (%zu events)", evs.size());
        CHECK(shot().near(gtrack + 5, gy, sliderFill, 0));
        // Decimation dropdown: row 2
        clk(fr - 72 + 30, l0 + 12);
        shot();
        auto pr = popupRow(fr - 72, l0, 2);
        e = press(pr.first, pr.second);
        CHECKV(e.name == "airspy-decimation" && e.args == "2", "decimation row 2: '%s' '%s'", e.name.c_str(), e.args.c_str());
        park();

        // Free mode (b4): LNA AGC ticked, Mixer AGC not, LNA greyed (disabled fill), MIXER and VGA live; the lines 28 px apart
        const int T4 = yOf(ui->get_b4_y()) + kSecRow + kBodyPad;
        const Frame g = shot();
        int lnaY = -1;
        for (int y = T4 + 2 * kPitch; y < T4 + 400 && lnaY < 0; ++y) { if (g.near(gtrack + 4, y, sliderOffFill, 0)) { lnaY = y; } }
        CHECKV(lnaY > 0, "the greyed LNA track at %d", lnaY);
        const int mixY = lnaY + 28, vgaY = lnaY + 56;
        CHECKV(g.near(gtrack + 4, mixY, sliderFill, 0) && g.near(gtrack + 4, vgaY, sliderFill, 0), "MIXER and VGA tracks 28 px under it");
        CHECK(g.countNear(xb + 9, lnaY - 9, xb + 9 + 56, lnaY + 15, tok(wb.disabled), 40) >= 8);                       // the greyed LNA label
        evs.clear();
        clk(gtrack + 30, lnaY + 2);                                                  // the LNA slider is greyed: a press does nothing
        bool lnaEvt = false;
        for (const Ev& ev : evs) { if (ev.name == "airspy-lna") { lnaEvt = true; } }
        CHECKV(!lnaEvt, "the greyed LNA slider takes no press");
        evs.clear();
        clk(gtrack + 25, mixY + 2);
        bool mixEvt = false;
        for (const Ev& ev : evs) { if (ev.name == "airspy-mixer") { mixEvt = true; CHECK(std::abs(num(ev.args) - 0.25) < 0.1); } }
        CHECK(mixEvt);
        evs.clear();
        clk(gtrack + 76, vgaY + 2);
        bool vgaEvt = false;
        for (const Ev& ev : evs) { if (ev.name == "airspy-vga") { vgaEvt = true; CHECK(std::abs(num(ev.args) - 0.75) < 0.1); } }
        CHECK(vgaEvt);
        // the two AGC ticks, one line above LNA: LNA AGC (the model says on -> asks false), Mixer AGC (-> true)
        const int agcY = lnaY - 9 - 28 + 12;
        CHECK(g.countNear(xb + 9, agcY - 7, xb + 9 + 14, agcY + 7, accent, 0) >= 100);
        e = press(xb + 9 + 40, agcY);
        CHECKV(e.name == "airspy-lna-agc" && e.args == "0", "LNA AGC: '%s' '%s'", e.name.c_str(), e.args.c_str());
        const int bx2 = firstX(g, agcY, xb + 9 + 60, fr, lineStrong, 0);
        e = press(bx2 + 40, agcY);
        CHECKV(bx2 > 0 && e.name == "airspy-mixer-agc" && e.args == "1", "Mixer AGC: '%s' '%s'", e.name.c_str(), e.args.c_str());
        park();
    }
    {
        // the converter card (c1): mode Up, the amber alias note, the LO field with a red border (the parse error), the lit 125 MHz quick key,
        // Inverts ticked, the parse error and the on-air line
        const int xc = kColC + kBodyX, Tc = yOf(ui->get_c1_y()) + kSecRow + kBodyPad;
        const Frame f = shot();
        int cardTop = -1;
        for (int y = Tc + 3 * kPitch; y < Tc + 400 && cardTop < 0; ++y) { if (f.near(xc + 20, y, lineStrong, 0) && f.near(xc + 20, y + 3, card, 0)) { cardTop = y; } }
        CHECKV(cardTop > 0, "the converter card at %d", cardTop);
        CHECK(f.countNear(xc + 9, cardTop + 3, xc + 120, cardTop + 17, accentSoft, 40) >= 30);
        const int mode = cardTop + 1 + 6 + 13 + 4;
        CHECK(f.near(xc + 9, mode + 12, keyLine) && f.near(xc + 9 + 123, mode + 12, keyLine) && !f.near(xc + 9 + 124, mode + 12, keyLine));      // 124 wide
        CHECK(f.countNear(xc + 9, mode + 28, xc + kBodyW - 9, mode + 28 + 28, warnFg, 40) >= 100);                                               // the alias note, amber, two lines
        int loTop = -1;
        for (int y = mode + 30; y < mode + 120 && loTop < 0; ++y) { if (f.near(xc + kBodyW - 9 - 40, y, lampRed, 0)) { loTop = y; } }
        CHECKV(loTop > 0, "the LO field's red border at %d", loTop);
        CHECK(f.near(xc + kBodyW - 9 - 40, loTop + 23, lampRed, 0) && f.near(xc + kBodyW - 10, loTop + 12, lampRed, 0) && f.near(xc + kBodyW - 9 - 92, loTop + 12, lampRed, 0));   // 92 wide
        CHECK(f.countNear(xc + 9, loTop + 5, xc + 130, loTop + 19, fgSoft, 60) >= 40);                                                           // "Local oscillator"
        // the quick keys: three xs keys 4 px under the LO line, the lit one (125 MHz) in accent
        const int qk = loTop + 28 + 12;
        int litX = -1;
        for (int x = xc + 9; x < xc + kBodyW && litX < 0; ++x) { if (f.near(x, qk, accent, 0)) { litX = x; } }
        // the keys are in the engine's order (kQuickLoHz: 125, 100, 2 MHz), so the lit 125 MHz key is the FIRST one
        CHECKV(litX >= xc + 9 && litX < xc + 9 + 12, "the 125 MHz key is the lit one, first (x %d)", litX);
        CHECK(f.near(litX + 30, qk - 10, accent, 0) && f.near(litX + 30, qk + 9, accent, 0) && f.near(litX + 30, qk - 11, card, 0));              // 20 high
        Ev e = press(litX + 10, qk);
        CHECKV(e.name == "converter-quick" && e.args == "0", "125 MHz: '%s' '%s'", e.name.c_str(), e.args.c_str());
        // the second and third keys follow the first at the row's 6 px spacing; each is its label plus 2 x 8 px padding wide
        int secondX = -1;
        for (int x = litX + 40; x < xc + kBodyW && secondX < 0; ++x) { if (!f.near(x, qk, accent, 0) && !f.near(x, qk, card, 0) && f.near(x - 1, qk, card, 0)) { secondX = x; } }
        e = press(secondX + 10, qk);
        CHECKV(e.name == "converter-quick" && e.args == "1", "100 MHz (x %d): '%s' '%s'", secondX, e.name.c_str(), e.args.c_str());
        int thirdX = -1;
        for (int x = secondX + 40; x < xc + kBodyW && thirdX < 0; ++x) { if (!f.near(x, qk, card, 0) && f.near(x - 1, qk, card, 0)) { thirdX = x; } }
        e = press(thirdX + 10, qk);
        CHECKV(e.name == "converter-quick" && e.args == "2", "2 MHz (x %d): '%s' '%s'", thirdX, e.name.c_str(), e.args.c_str());
        // Inverts: a ticked box (the model says on) on a two-line label, pressing asks false
        const int inv = loTop + 56;
        CHECK(f.countNear(xc + 9, inv, xc + 9 + 14, inv + 28, accent, 0) >= 100);
        e = press(xc + 9 + 7, inv + 8);
        CHECKV(e.name == "converter-inverts" && e.args == "0", "inverts: '%s' '%s'", e.name.c_str(), e.args.c_str());
        // LO: typing and Enter commit
        evs.clear();
        clk(xc + kBodyW - 9 - 30, loTop + 12);
        tap(slint::platform::key_codes::End);
        for (int i = 0; i < 8; ++i) { tap(slint::platform::key_codes::Backspace); }
        tap(u8"1"); tap(u8"0");
        tap(slint::platform::key_codes::Return);
        CHECKV(!evs.empty() && evs.back().name == "converter-lo-committed" && evs.back().args == "10", "LO: '%s'", evs.empty() ? "" : evs.back().args.c_str());
        clk(R + 200, 4);
        // mode dropdown: Down-converter (row 2)
        clk(xc + 9 + 50, mode + 12);
        shot();
        auto pr = popupRow(xc + 9, mode, 2);
        e = press(pr.first, pr.second);
        CHECKV(e.name == "converter-mode" && e.args == "2", "mode row 2: '%s' '%s'", e.name.c_str(), e.args.c_str());
        // the parse error in amber and the on-air line dim, under Inverts
        CHECKV(f.countNear(xc + 9, inv + 28, xc + kBodyW - 9, inv + 28 + 18, warnFg, 40) >= 60, "the parse error is amber");
        CHECKV(f.countNear(xc + 9, inv + 28 + 18, xc + kBodyW - 9, inv + 28 + 18 + 40, dim, 40) >= 80, "the on-air line is dim");
        park();
    }

    // =========================================================================================================
    // 8. The radio's own options (d9) and the file, Pluto and rtl_tcp cards: a card of ticks, a greyed one takes no press
    // =========================================================================================================
    {
        const int xd = kColD + kBodyX, Td = yOf(ui->get_d9_y()) + kSecRow + kBodyPad;
        const int first = Td + 2 * kPitch + 1 + 6 + 13 + 4;                                  // the first tick's row; each row is 24, 4 apart
        const Frame f = shot();
        CHECKV(f.countNear(xd + 9, first + 5, xd + 9 + 14, first + 19, accent, 0) >= 100, "FM notch is ticked");
        CHECK(f.countNear(xd + 9, first + 28 + 5, xd + 9 + 14, first + 28 + 19, accent, 0) == 0);                       // DAB notch is not
        CHECK(f.countNear(xd + 9, first + 4 * 28 + 5, xd + 9 + 14, first + 4 * 28 + 19, accent, 0) >= 100);              // ADC randomiser is
        CHECK(f.countNear(xd + 9 + 22, first + 3 * 28 + 4, xd + 9 + 120, first + 3 * 28 + 20, tok(wb.key_disabled_fg), 40) >= 20);   // ADC dither is greyed
        Ev e = press(xd + 9 + 40, first + 12);
        CHECKV(e.name == "option-toggled" && e.args == "0 0", "FM notch off: '%s' '%s'", e.name.c_str(), e.args.c_str());
        e = press(xd + 9 + 40, first + 28 + 12);
        CHECKV(e.name == "option-toggled" && e.args == "1 1", "DAB notch on: '%s' '%s'", e.name.c_str(), e.args.c_str());
        e = press(xd + 9 + 40, first + 3 * 28 + 12);
        CHECKV(e.name.empty(), "the greyed ADC dither does nothing: '%s'", e.name.c_str());
        e = press(xd + 9 + 40, first + 4 * 28 + 12);
        CHECKV(e.name == "option-toggled" && e.args == "4 0", "ADC randomiser: '%s' '%s'", e.name.c_str(), e.args.c_str());
        // the I/Q file: Enter commits the path, Open
        const int T6 = yOf(ui->get_d6_y()) + kSecRow + kBodyPad, fy6 = T6 + 2 * kPitch + 1 + 6 + 13 + 4;
        evs.clear();
        clk(xd + 9 + 60, fy6 + 12);
        tap(slint::platform::key_codes::End);
        tap(u8"x");
        tap(slint::platform::key_codes::Return);
        CHECKV(!evs.empty() && evs.back().name == "iq-path-committed" && evs.back().args == "C:\\iq\\pass-0412.cu8x", "the I/Q path commits: '%s'", evs.empty() ? "" : evs.back().args.c_str());
        e = press(xd + kBodyW - 9 - 20, fy6 + 12);
        CHECKV(e.name == "iq-open", "I/Q Open: '%s'", e.name.c_str());
        // BROWSE (docs 6.63) sits left of Open: two key fills at the right end of the row, the first is Browse
        {
            const Frame fr = shot();
            std::vector<std::pair<int, int>> runs;
            for (int x = xd + kBodyW / 2; x < xd + kBodyW; ++x) {
                if (!fr.near(x, fy6 + 3, keyBg)) { continue; }
                if (!runs.empty() && x - runs.back().second <= 2) { runs.back().second = x; } else { runs.emplace_back(x, x); }
            }
            // (the text box's own fill and the label of Open break the runs up; the widest unbroken fill left of Open is Browse)
            const int openX = xd + kBodyW - 9 - 20;
            std::pair<int, int> browse{0, 0};
            for (const auto& r : runs) {
                if (r.second < openX - 20 && r.second - r.first > browse.second - browse.first) { browse = r; }
            }
            CHECKV(browse.second - browse.first >= 40, "I/Q row: a Browse key left of Open (widest fill %d px)", browse.second - browse.first);
            e = press((browse.first + browse.second) / 2, fy6 + 12);
            CHECKV(e.name == "iq-browse", "I/Q Browse: '%s'", e.name.c_str());
            e = press(openX, fy6 + 12);
            CHECKV(e.name == "iq-open", "I/Q Open beside Browse: '%s'", e.name.c_str());
            // Ctrl+V in the box is offered to the host first (a copied FILE is not text); the host took it, so nothing else is pasted
            evs.clear();
            clk(xd + 9 + 60, fy6 + 12);
            ctrlTap(win, u8"v");
            CHECKV(!evs.empty() && evs.back().name == "iq-paste", "Ctrl+V in the I/Q box asks the host: '%s'", evs.empty() ? "" : evs.back().name.c_str());
        }
        const int T7 = yOf(ui->get_d7_y()) + kSecRow + kBodyPad, fy7 = T7 + 2 * kPitch + 1 + 6 + 13 + 4;
        e = press(xd + kBodyW - 9 - 20, fy7 + 12);
        CHECKV(e.name == "pluto-open", "Pluto Open: '%s'", e.name.c_str());
        evs.clear();
        clk(xd + 9 + 60, fy7 + 12);
        tap(slint::platform::key_codes::End);
        tap(u8"z");
        tap(slint::platform::key_codes::Return);
        CHECKV(!evs.empty() && evs.back().name == "pluto-address-committed" && evs.back().args == "ip:192.168.2.1z", "the Pluto address commits: '%s'", evs.empty() ? "" : evs.back().args.c_str());
        const int T8 = yOf(ui->get_d8_y()) + kSecRow + kBodyPad, fy8 = T8 + 2 * kPitch + 1 + 6 + 13 + 4;
        e = press(xd + kBodyW - 9 - 20, fy8 + 12);
        CHECKV(e.name == "rtltcp-open", "rtl_tcp Open: '%s'", e.name.c_str());
        evs.clear();
        clk(xd + 9 + 60, fy8 + 12);
        tap(slint::platform::key_codes::End);
        tap(u8"5");
        clk(xd + 9 + 60, fy7 + 12);                                                                      // the focus moves to the Pluto field with an edit pending
        CHECKV(!evs.empty() && evs.back().name == "rtltcp-address-committed" && evs.back().args == "192.168.1.20:12345", "rtl_tcp address (focus moved): '%s'", evs.empty() ? "" : evs.back().args.c_str());
        clk(R + 500, 4);
        park();
    }

    // =========================================================================================================
    // 9. Audio filters (c2, c3)
    // =========================================================================================================
    {
        const int xc = kColC + kBodyX, T2 = yOf(ui->get_c2_y()) + kSecRow + kBodyPad;
        const Frame f = shot();
        // the chain note (dim), then Noise reduction ticked, its Strength slider, Notch ticked, Freq, Q, Auto notch ticked with "Locked on 1.2 kHz" in green
        CHECKV(f.countNear(xc, T2, xc + kBodyW, T2 + 14, dim, 30) >= 80, "the chain note is dim");
        int nrBox = -1;
        for (int y = T2; y < T2 + 60 && nrBox < 0; ++y) { if (f.near(xc + 3, y, accent, 0)) { nrBox = y; } }
        CHECKV(nrBox > 0, "the Noise reduction box at %d", nrBox);
        const int nrY = nrBox - 5, stY = nrY + kPitch, ntY = stY + kPitch, frY = ntY + kPitch, qY = frY + kPitch, anY = qY + kPitch;
        CHECK(f.countNear(xc, nrY + 5, xc + 14, nrY + 19, accent, 0) >= 100);
        CHECK(f.countNear(xc, ntY + 5, xc + 14, ntY + 19, accent, 0) >= 100);
        CHECK(f.countNear(xc, anY + 5, xc + 14, anY + 19, accent, 0) >= 100);
        const int track0 = xc + 64;
        CHECK(f.near(track0 + 30, stY + 12, sliderFill, 0) && f.near(track0 + 100, stY + 12, sliderTrack, 0));          // 0.45 of 119 = 54
        CHECK(f.near(track0 + 50, frY + 12, sliderFill, 0) && f.near(track0 + 100, frY + 12, sliderTrack, 0));         // 0.5 = 60
        CHECK(f.near(track0 + 4, qY + 12, sliderFill, 0) && f.near(track0 + 60, qY + 12, sliderTrack, 0));            // 0.15 = 18
        CHECK(f.countNear(xc, stY + 5, xc + 56, stY + 19, fgSoft, 60) >= 20);                                          // "Strength"
        CHECKV(f.countNear(xc + kBodyW - 120, anY + 3, xc + kBodyW, anY + 21, goodFg, 40) >= 25, "Locked on 1.2 kHz in green: %d", f.countNear(xc + kBodyW - 120, anY + 3, xc + kBodyW, anY + 21, goodFg, 40));
        const Ink st = inkBox(f, xc + kBodyW - 120, anY + 3, xc + kBodyW + 4, anY + 21, panel);
        CHECKV(st.x1 >= xc + kBodyW - 6, "the status ends flush right (%d)", st.x1);
        // events
        Ev e = press(xc + 40, nrY + 12);
        CHECKV(e.name == "filter-nr" && e.args == "0", "NR: '%s' '%s'", e.name.c_str(), e.args.c_str());
        evs.clear();
        clk(track0 + 89, stY + 12);
        CHECKV(!evs.empty() && evs.back().name == "filter-nr-strength" && std::abs(num(evs.back().args) - 0.75) < 0.04, "strength 0.75: '%s'", evs.empty() ? "" : evs.back().args.c_str());
        e = press(xc + 40, ntY + 12);
        CHECKV(e.name == "filter-notch" && e.args == "0", "notch: '%s' '%s'", e.name.c_str(), e.args.c_str());
        evs.clear();
        clk(track0 + 24, frY + 12);
        CHECKV(!evs.empty() && evs.back().name == "filter-notch-freq" && std::abs(num(evs.back().args) - 0.2) < 0.04, "freq 0.2: '%s'", evs.empty() ? "" : evs.back().args.c_str());
        evs.clear();
        clk(track0 + 95, qY + 12);
        CHECKV(!evs.empty() && evs.back().name == "filter-notch-q" && std::abs(num(evs.back().args) - 0.8) < 0.04, "Q 0.8: '%s'", evs.empty() ? "" : evs.back().args.c_str());
        e = press(xc + 40, anY + 12);
        CHECKV(e.name == "filter-auto-notch" && e.args == "0", "auto notch: '%s' '%s'", e.name.c_str(), e.args.c_str());
        // c3: everything off: the sliders carry the disabled fill and take no press; the status is dim ("Searching")
        const int T3 = yOf(ui->get_c3_y()) + kSecRow + kBodyPad;
        const int s3 = T3 + (stY - T2), an3 = T3 + (anY - T2);
        const Frame g = shot();
        CHECK(g.near(track0 + 20, s3 + 12, sliderOffFill, 0));
        CHECK(!g.near(xc + 3, T3 + (nrY - T2) + 8, accent, 0));
        evs.clear();
        clk(track0 + 90, s3 + 12);
        CHECKV(evs.empty(), "the greyed Strength slider takes no press (%zu)", evs.size());
        CHECK(g.countNear(xc + kBodyW - 80, an3 + 3, xc + kBodyW, an3 + 21, dim, 40) >= 15 && g.countNear(xc + kBodyW - 80, an3 + 3, xc + kBodyW, an3 + 21, goodFg, 40) == 0);
        park();
    }

    // =========================================================================================================
    // 10. Sinks (c4, c5: SignalEvents), Recorder (d1, d2), Radio extras (d3, d4: SignalEvents), Transmit (d5)
    // =========================================================================================================
    {
        const int xc = kColC + kBodyX, T4 = yOf(ui->get_c4_y()) + kSecRow + kBodyPad;
        const Frame f = shot();
        CHECK(f.near(xc + 40, T4, keyLine) && f.near(xc + 40, T4 + 23, keyLine) && f.near(xc + kBodyW - 1, T4 + 12, keyLine) && f.near(xc, T4 + 12, keyLine));   // a full-width dropdown
        CHECKV(f.countNear(xc, T4 + 30, xc + kBodyW, T4 + 44, warnFg, 40) >= 80, "the audio-health note is amber");
        int bufY = -1;
        for (int y = T4 + 30; y < T4 + 90 && bufY < 0; ++y) { if (f.near(xc + kBodyW - 40, y, keyLine, 0)) { bufY = y; } }
        CHECKV(bufY > 0, "the buffer dropdown at %d", bufY);
        CHECK(f.near(xc + kBodyW - 1, bufY + 12, keyLine) && f.near(xc + kBodyW - 160, bufY + 12, keyLine) && !f.near(xc + kBodyW - 161, bufY + 12, keyLine));    // 160 wide at the right
        CHECK(f.countNear(xc, bufY + 5, xc + 80, bufY + 19, fgSoft, 60) >= 25);                                           // "Audio buffer"
        const auto ls = textLines(f, xc, xc + kBodyW, bufY + 26, bufY + 26 + 70, panel);
        CHECKV(ls.size() == 3, "three lines under the buffer (%zu)", ls.size());
        if (ls.size() == 3) {
            CHECKV(f.countNear(xc, ls[0].first, xc + kBodyW, ls[0].second, warnFg, 40) >= 80, "the lead notice is amber");
            CHECKV(f.countNear(xc, ls[1].first, xc + kBodyW, ls[1].second, warnFg, 40) >= 80, "Muted by ... is amber");
            CHECKV(f.countNear(xc, ls[2].first, xc + kBodyW, ls[2].second, dim, 30) >= 80, "the volume note is dim");
        }
        // the default-forwarding body: the device list, row 1; the buffer list, row 3
        fwd.clear();
        clk(xc + 60, T4 + 12);
        shot();
        auto pr = popupRow(xc, T4, 1);
        clk(pr.first, pr.second);
        CHECKV(!fwd.empty() && fwd.back().name == "sink-device-selected" && fwd.back().args == "1", "sink device row 1 forwards to SignalEvents");
        fwd.clear();
        clk(xc + kBodyW - 80, bufY + 12);
        shot();
        pr = popupRow(xc + kBodyW - 160, bufY, 3);
        clk(pr.first, pr.second);
        CHECKV(!fwd.empty() && fwd.back().name == "sink-buffer-selected" && fwd.back().args == "3", "sink buffer row 3 forwards to SignalEvents");
        // the disabled device row (HDMI, index 2) does not pick
        fwd.clear();
        clk(xc + 60, T4 + 12);
        shot();
        pr = popupRow(xc, T4, 2);
        clk(pr.first, pr.second);
        CHECK(fwd.empty());
        clk(R + 250, 4);                                                                                 // a click outside closes the list that stayed open
        park();
        // c5: no device line, the dim sentence in its place, the buffer line follows
        const int T5 = yOf(ui->get_c5_y()) + kSecRow + kBodyPad;
        const Frame g = shot();
        CHECK(g.countNear(xc, T5, xc + kBodyW, T5 + 14, dim, 30) >= 60 && !g.near(xc + 40, T5, keyLine));
        CHECK(g.near(xc + kBodyW - 1, T5 + 13 + 6 + 12, keyLine));
    }
    {
        // the recorder: two keys side by side, 8 px apart, idle; running: Stop IQ in the danger look and the take's lines under the keys
        const int xd = kColD + kBodyX, T1 = yOf(ui->get_d1_y()) + kSecRow + kBodyPad;
        const Frame f = shot();
        CHECKV(f.countInk(xd, T1, xd + kBodyW, T1 + 13, panel) >= 80 && f.countNear(xd, T1, xd + kBodyW, T1 + 14, dim, 30) >= 60, "the record folder, dim");
        int ky = -1;
        for (int y = T1; y < T1 + 40 && ky < 0; ++y) { if (f.near(xd + 40, y, keyLine, 0)) { ky = y; } }
        CHECKV(ky > 0 && f.near(xd, ky + 12, keyLine) && f.near(xd + 40, ky + 23, keyLine) && f.near(xd + 40, ky + 24, panel, 0), "Record IQ: a 24 px key at %d", ky);
        CHECKV(f.near(xd + kBodyW - 1, ky + 12, keyLine) && gapLeftOf(f, ky + 3, xd + kBodyW - 3 - 20, panel) == 8, "the two keys share the row, 8 px apart");
        CHECK(f.countNear(xd + 8, ky + 5, xd + 110, ky + 19, fg, 60) >= 40);
        Ev e = press(xd + 50, ky + 12);
        CHECKV(e.name == "recorder-iq", "Record IQ: '%s'", e.name.c_str());
        e = press(xd + kBodyW - 50, ky + 12);
        CHECKV(e.name == "recorder-audio", "Record audio: '%s'", e.name.c_str());
        // d2: IQ running
        const int T2 = yOf(ui->get_d2_y()) + kSecRow + kBodyPad;
        const Frame g = shot();
        int ky2 = -1;
        for (int y = T2; y < T2 + 40 && ky2 < 0; ++y) { if (g.near(xd + 40, y, tok(wb.danger_line), 0)) { ky2 = y; } }
        CHECKV(ky2 > 0, "Stop IQ is a danger key (top %d)", ky2);
        CHECK(g.near(xd + 50, ky2 + 3, dangerBg, 3) && g.near(xd + kBodyW - 50, ky2 + 3, keyBg, 3));
        const auto ls = textLines(g, xd, xd + kBodyW, ky2 + 26, ky2 + 26 + 100, panel);
        CHECKV(ls.size() >= 5, "five lines under the keys (%zu)", ls.size());
        if (ls.size() >= 5) {
            CHECKV(g.countNear(xd, ls[0].first, xd + kBodyW, ls[0].second, dim, 30) >= 60, "the take's progress, mono dim");
            CHECKV(g.countNear(xd, ls[1].first, xd + kBodyW, ls[1].second, warnFg, 40) >= 100, "the muted warning amber");
            CHECKV(g.countNear(xd, ls[2].first, xd + kBodyW, ls[2].second, dim, 30) >= 60, "the hint dim");
            CHECKV(g.countNear(xd, ls[3].first, xd + kBodyW, ls[3].second, errFg, 40) >= 50, "the recorder error red");
            CHECKV(g.countNear(xd, ls[4].first, xd + kBodyW, ls[4].second, dim, 30) >= 60, "the notice dim");
        }
        e = press(xd + 50, ky2 + 12);
        CHECKV(e.name == "recorder-iq", "Stop IQ: '%s'", e.name.c_str());
        const int imp = yOf(ui->get_d2_y()) + kSecRow + yOf(ui->get_d2_body_h()) - kBodyPadBottom - 24 + 12;
        e = press(xd + 40, imp);
        CHECKV(e.name == "recorder-import", "the import key: '%s'", e.name.c_str());
        park();
    }
    {
        // Radio extras: Stereo ticked beside a green STEREO chip, the RDS block; unticked with an amber MONO chip and no RDS; forwards to SignalEvents
        const int xd = kColD + kBodyX, T3 = yOf(ui->get_d3_y()) + kSecRow + kBodyPad;
        const Frame f = shot();
        CHECK(f.countNear(xd, T3 + 5, xd + 14, T3 + 19, accent, 0) >= 100);
        const Ink chip = inkBox(f, xd + 150, T3, xd + kBodyW + 4, T3 + 24, panel);
        CHECKV(chip.w() >= 55 && chip.w() <= 95 && chip.h() == 20 && chip.x1 == xd + kBodyW - 1, "the STEREO chip is %d x %d, flush right (x1 %d)", chip.w(), chip.h(), chip.x1);
        CHECK(f.near(chip.x0 + 4, T3 + 12, tok(wb.chip_good_bg), 2));
        CHECKV(f.countNear(xd, T3 + 30, xd + 40, T3 + 30 + 14, muted, 40) >= 15, "the RDS cap");
        const auto ls = textLines(f, xd, xd + kBodyW, T3 + 28, T3 + 110, panel);
        CHECKV(ls.size() == 3, "RDS: cap, PI and PS, radio text (%zu lines)", ls.size());
        if (ls.size() == 3) {
            CHECK(f.countNear(xd, ls[1].first, xd + kBodyW, ls[1].second, fg, 60) >= 40);
            CHECK(f.countNear(xd, ls[2].first, xd + kBodyW, ls[2].second, fgSoft, 60) >= 40);
        }
        fwd.clear();
        clk(xd + 40, T3 + 12);
        CHECKV(!fwd.empty() && fwd.back().name == "radio-stereo" && fwd.back().args == "0", "Stereo forwards to SignalEvents");
        const int T4 = yOf(ui->get_d4_y()) + kSecRow + kBodyPad;
        const Frame g = shot();
        CHECK(g.near(xd + kBodyW - 6, T4 + 12, tok(wb.chip_warn_bg), 2));
        CHECK(g.countNear(xd + 9, T4 + 5, xd + 9 + 14, T4 + 19, accent, 0) == 0);                                          // Stereo is not ticked
        CHECKV(std::abs(ui->get_d4_body_h() - (kBodyPad + 24 + kBodyPadBottom)) < 1.0, "no RDS block: the body is %.1f high", ui->get_d4_body_h());
        // Transmit: a fault line in red, nothing else
        const int T5 = yOf(ui->get_d5_y()) + kSecRow + kBodyPad;
        CHECKV(g.countNear(xd, T5, xd + kBodyW, T5 + 14, errFg, 40) >= 80, "the transmit fault line is red");
        CHECK(yOf(ui->get_d5_body_h()) <= kBodyPad + 14 + kBodyPadBottom + 1);
        park();
    }

    // =========================================================================================================
    // 11. The two sheets
    // =========================================================================================================
    {
        const int sy = yOf(ui->get_setup_y());
        const int sx = 10, stageW = 620, stageH = 420;
        const Frame f = shot();
        // the stage is the backdrop; the panel is 580 x min(520, stage - 24) centred, panel ground, knob-ring border, a menu-ground title strip 30 high
        const int panelW = 580, panelH = stageH - 24, px = sx + (stageW - panelW) / 2, py = sy + (stageH - panelH) / 2;
        CHECKV(f.near(px + 100, py, knobRing, 0) && f.near(px + 100, py + panelH - 1, knobRing, 0) && f.near(px, py + 100, knobRing, 0) && f.near(px + panelW - 1, py + 100, knobRing, 0), "the sheet's border at (%d, %d)", px, py);
        CHECK(f.near(px + 300, py + 10, menu, 0) && f.near(px + 300, py + 30, lineStrong, 0) && f.near(px + 100, py + 40, panel, 0));
        CHECKV(f.countNear(px + 12, py + 6, px + 200, py + 24, fg, 60) >= 40, "the title, upper case");
        CHECK(f.countInk(px + panelW - 34, py + 4, px + panelW - 6, py + 26, menu) >= 10);                                // the close key
        CHECK(!f.near(sx + 4, sy + 4, bg, 0));                                                                             // the backdrop is dimmed over the stage's ground
        // the lines and findings: busy line amber, the headline in fg, the findings' titles amber, the family in mono dim, the advice in fg-soft
        CHECKV(f.countNear(px + 12, py + 31 + 12, px + panelW - 12, py + 31 + 28, warnFg, 40) >= 60, "the checking line is amber");
        const auto ls = textLines(f, px + 12, px + panelW - 12, py + 31 + 12, py + panelH - 60, panel);
        CHECKV(ls.size() >= 7, "the sheet's lines of text (%zu)", ls.size());
        if (ls.size() >= 7) {
            CHECK(f.countNear(px + 12, ls[1].first, px + panelW - 12, ls[1].second, fg, 60) >= 100);                      // the headline
            CHECK(f.countNear(px + 12, ls[3].first, px + panelW - 12, ls[3].second, warnFg, 40) >= 100);                  // a finding's title
            CHECK(f.countNear(px + 12, ls[4].first, px + panelW - 12, ls[4].second, dim, 40) >= 40);                      // RTL-SDR  0BDA:2838
            CHECK(f.countNear(px + 12, ls[5].first, px + panelW - 12, ls[5].second, fgSoft, 60) >= 100);                  // the advice
        }
        // the footer: the key row 10 px over the tick row (24), 12 px of padding under it
        const int tickRow = py + panelH - 2 - 12 - 24;
        int keyRow = -1;
        for (int y = tickRow - 10 - 24 - 6; y < tickRow - 10 && keyRow < 0; ++y) { if (f.near(px + 12 + 40, y, keyLine, 0)) { keyRow = y; } }
        CHECKV(keyRow > 0 && f.near(px + 12 + 40, keyRow + 23, keyLine, 0), "the footer's key row at %d", keyRow);
        CHECK(f.near(px + 13, tickRow + 12, lineStrong) && f.near(px + 16, tickRow + 8, field, 0));                          // "Don't show this again", unticked
        Ev e = press(px + 12 + 40, keyRow + 12);
        CHECKV(e.name == "setup-key" && e.args == "install-driver", "INSTALL DRIVER: '%s' '%s'", e.name.c_str(), e.args.c_str());
        // the second and third keys: find each key's start after the 6 px gap
        std::vector<int> starts;
        bool inKey = true;
        for (int x = px + 12 + 40; x < px + panelW - 12; ++x) {
            const bool ground = f.near(x, keyRow + 3, panel, 1);
            if (ground && inKey) { inKey = false; }
            if (!ground && !inKey) { inKey = true; starts.push_back(x); }
        }
        CHECKV(starts.size() >= 2, "three keys on the footer (%zu starts after the first)", starts.size());
        if (starts.size() >= 2) {
            e = press(starts[0] + 30, keyRow + 12);
            CHECKV(e.name == "setup-key" && e.args == "check-again", "CHECK AGAIN: '%s' '%s'", e.name.c_str(), e.args.c_str());
            e = press(starts[1] + 30, keyRow + 12);
            CHECKV(e.name == "setup-key" && e.args == "link", "the vendor link key: '%s' '%s'", e.name.c_str(), e.args.c_str());
        }
        e = press(px + 12 + 40, tickRow + 12);
        CHECKV(e.name == "setup-dont-show" && e.args == "1", "Don't show this again: '%s' '%s'", e.name.c_str(), e.args.c_str());
        e = press(px + panelW - 17, py + 15);
        CHECKV(e.name == "setup-closed", "the close key: '%s'", e.name.c_str());
        e = press(sx + 6, sy + 6);
        CHECKV(e.name == "setup-closed", "a click on the backdrop closes: '%s'", e.name.c_str());
        e = press(px + 300, py + 200);
        CHECKV(e.name.empty(), "a click on the sheet's panel does not: '%s'", e.name.c_str());
        // shown = false: the stage is bare and takes no click
        auto m = ui->get_m_setup();
        m.shown = false;
        ui->set_m_setup(m);
        const Frame h = shot();
        CHECK(h.countInk(sx + 2, sy + 2, sx + stageW - 2, sy + stageH - 2, bg, 6) == 0);
        e = press(px + 12 + 40, keyRow + 12);
        CHECKV(e.name.empty(), "a hidden sheet takes no click: '%s'", e.name.c_str());
        e = press(sx + 6, sy + 6);
        CHECKV(e.name.empty(), "nor the backdrop's: '%s'", e.name.c_str());
        m.shown = true;
        ui->set_m_setup(m);
        park();

        // the import dialog: title, the reason line dim, "Import as", the drop zone, the path field + Browse..., Cancel
        const int ix = 640, iw = 560, ih = 340;
        const Frame g = shot();
        const int ipanelW = 460;
        const int ipx = ix + (iw - ipanelW) / 2;
        int topY = -1;
        for (int y = sy; y < sy + ih && topY < 0; ++y) { if (g.near(ipx + 100, y, knobRing, 0)) { topY = y; } }
        CHECKV(topY > 0, "the import dialog's panel top at %d", topY);
        CHECK(g.near(ipx + 100, topY + 10, menu, 0) && g.near(ipx + 100, topY + 30, lineStrong, 0));
        const int contentTop = topY + 31 + 12;
        CHECKV(g.countNear(ipx + 12, contentTop, ipx + ipanelW - 12, contentTop + 14, dim, 30) >= 60, "the reason line is dim");
        // "Import as" and its dropdown (200 wide at the right), then the drop zone: a 96 px box with a muted hairline
        int zoneTop = -1;
        for (int y = contentTop + 20; y < contentTop + 120 && zoneTop < 0; ++y) { if (g.near(ipx + 200, y, muted, 2)) { zoneTop = y; } }
        CHECKV(zoneTop > 0 && zoneTop - contentTop >= 55 && zoneTop - contentTop <= 60, "the drop zone is %d px under the reason line", zoneTop - contentTop);
        const int kindY = zoneTop - 10 - 24;
        CHECK(g.near(ipx + ipanelW - 14, kindY + 12, keyLine) && g.near(ipx + ipanelW - 13 - 200, kindY + 12, keyLine));
        CHECK(g.countNear(ipx + 12, kindY + 5, ipx + 100, kindY + 19, fgSoft, 60) >= 30);
        CHECKV(g.near(ipx + 200, zoneTop, muted, 2) && g.near(ipx + 200, zoneTop + 95, muted, 2) && g.near(ipx + 13, zoneTop + 48, muted, 2) && g.near(ipx + ipanelW - 14, zoneTop + 48, muted, 2), "the drop zone is 96 high (top %d)", zoneTop);
        CHECK(g.countNear(ipx + 20, zoneTop + 30, ipx + ipanelW - 20, zoneTop + 70, fg, 60) >= 100);
        const int pathY = zoneTop + 96 + 10;
        CHECK(g.near(ipx + 100, pathY, lineStrong) && g.near(ipx + 100, pathY + 23, lineStrong));
        CHECKV(g.countNear(ipx + 14, pathY + 4, ipx + 200, pathY + 20, tok(wb.disabled), 40) >= 30, "the path placeholder");
        // events
        e = press(ipx + ipanelW - 12 - 30, pathY + 12);
        CHECKV(e.name == "import-open", "Browse...: '%s'", e.name.c_str());
        e = press(ipx + ipanelW - 12 - 30, pathY + 24 + 10 + 12);
        CHECKV(e.name == "import-closed", "Cancel: '%s'", e.name.c_str());
        evs.clear();
        clk(ipx + 100, pathY + 12);
        tap(u8"a"); tap(u8"."); tap(u8"c"); tap(u8"s"); tap(u8"v");
        tap(slint::platform::key_codes::Return);
        CHECKV(!evs.empty() && evs.back().name == "import-path-committed" && evs.back().args == "a.csv", "the path commits: '%s'", evs.empty() ? "" : evs.back().args.c_str());
        // the kind dropdown ("Import as"): row 1
        const int kl = ipx + ipanelW - 13 - 200;
        clk(kl + 60, kindY + 12);
        shot();
        auto pr = popupRow(kl, kindY, 1);
        e = press(pr.first, pr.second);
        CHECKV(e.name == "import-kind" && e.args == "1", "kind row 1: '%s' '%s'", e.name.c_str(), e.args.c_str());
        e = press(ix + 6, sy + 6);
        CHECKV(e.name == "import-closed", "a click on the import backdrop closes: '%s'", e.name.c_str());
        // opened for an Airband preset (docs 6.61): a line naming the preset stands where "Import as" was and the dialog offers no I/Q file; no reason line here
        {
            auto pm = ui->get_m_import();
            pm.error = "";
            pm.preset = "Towers";
            pm.preset_line = "The rows will join the preset \"Towers\".";
            pm.kinds = std::make_shared<slint::VectorModel<SelectItem>>();
            ui->set_m_import(pm);
            const Frame pf = shot();
            int pTop = -1;
            for (int y = sy; y < sy + ih && pTop < 0; ++y) { if (pf.near(ipx + 100, y, knobRing, 0)) { pTop = y; } }
            CHECKV(pTop > 0, "the preset import dialog's panel top at %d", pTop);
            const int pContent = pTop + 31 + 12;
            int pZone = -1;
            for (int y = pContent + 4; y < pContent + 120 && pZone < 0; ++y) { if (pf.near(ipx + 200, y, muted, 2)) { pZone = y; } }
            CHECKV(pZone > 0, "the drop zone is still there (at %d)", pZone);
            CHECKV(pf.countNear(ipx + 12, pContent, ipx + ipanelW - 12, std::max(pContent + 1, pZone - 4), dim, 40) >= 60, "the line naming the preset is drawn dim above the drop zone");
            // no "Import as" dropdown: nothing at the right edge in the rows above the drop zone carries the key's line
            int keyLines = 0;
            for (int y = pContent; y < pZone - 2; ++y) { if (pf.near(ipx + ipanelW - 14, y, keyLine)) { ++keyLines; } }
            CHECKV(keyLines == 0, "no Import-as dropdown above the drop zone (%d key-line pixels)", keyLines);
            // and the plain dialog (the first picture) still has both: its drop zone sat lower by the kind row
            CHECKV(zoneTop - contentTop > pZone - pContent, "the plain dialog is taller by the Import-as row: %d against %d", zoneTop - contentTop, pZone - pContent);
            park();
        }
        park();
    }

    // =========================================================================================================
    // 12. Rule 11 again, under the French catalogue's longest words: the probes' cells stayed 254 or less
    // =========================================================================================================
    CHECK(ui->get_p3_w() > 0);
    const Frame last = shot();
    last.savePng(shotDir() + "signal-specimen-after.png");

    return testSummary("test_slint_signal");
}
