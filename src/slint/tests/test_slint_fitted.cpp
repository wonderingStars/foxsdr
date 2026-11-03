// The FITTED family of the plugin kit (docs 6.37): FittedModules (the Fitted modules page: the top bar, the verdict lines, the grouped dense
// rows, the empty text and the clean-up foot, in a body that scrolls) and ModulePage (one module's page, scrolling as a whole), rendered
// headlessly from ui/specimens/fitted_specimen.slint and probed against the PluginKit sheet ("State chips and row examples": the `.sec.s` rows,
// 25 px, 44 px with a note, the state chips and the `.btn.xs` keys; "Module page (tab) excerpt": the back key, the lamp and STOP, the 13 px name,
// the grant keys, the plate keys, the verdict) and the contract in COMPONENTS.md ("The plugin kit").
//
// The typefaces are the embedded Inter and JetBrains Mono, but text checks still assert INK in a region and its COLOUR, never glyph positions:
// boxes, borders, gaps and the pitch of the rows are drawn geometry and are asserted to the pixel. Clicks, double clicks, typing, the wheel and
// a dragged thumb go through the harness's dispatched events, so every callback is the real one. A list of runs is read through spanAt(), which
// answers an impossible span when the list is short, so a missing chip or key fails every check made on it.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "render_harness.hpp"
#include "test_check.hpp"

#include "fitted_specimen.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using namespace foxsdr::slinttest;
using std::chrono::milliseconds;

namespace {

// Prints the numbers behind a failed check, so a red run says what was measured.
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

// A run of non-ground pixels along a row: [l, r).
struct Span {
    int l = 0, r = 0;
    int w() const { return r - l; }
    int mid() const { return (l + r) / 2; }
};
Span spanAt(const std::vector<Span>& v, std::size_t i) { return i < v.size() ? v[i] : Span{-1000, -990}; }
std::vector<Span> runsOnRow(const Frame& f, int y, int x0, int x1, Rgb outside, int tol = 4) {
    std::vector<Span> out;
    bool in = false;
    int start = 0;
    for (int x = x0; x < x1; ++x) {
        const bool ground = f.near(x, y, outside, tol);
        if (!ground && !in) { in = true; start = x; }
        if (ground && in) { in = false; out.push_back({start, x}); }
    }
    if (in) { out.push_back({start, x1}); }
    return out;
}
// The cluster at the right of a row (the chip and the keys, 6 px apart): the runs from the right edge while each is 6 px from the next.
std::vector<Span> clusterOnRow(const Frame& f, int y, int x0, int x1, Rgb outside) {
    const std::vector<Span> all = runsOnRow(f, y, x0, x1, outside);
    std::vector<Span> out;
    for (std::size_t i = all.size(); i-- > 0;) {
        if (!out.empty() && out.front().l - all[i].r != 6) { break; }
        out.insert(out.begin(), all[i]);
    }
    return out;
}

// The bounding box and count of the ink (pixels differing from `bg`) in a rectangle.
struct Ink {
    int n = 0, x0 = 1 << 30, y0 = 1 << 30, x1 = -1, y1 = -1;
    int w() const { return x1 - x0 + 1; }
    int h() const { return y1 - y0 + 1; }
    double cy() const { return (y0 + y1 + 1) / 2.0; }
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
int differing(const Frame& a, const Frame& b, int x0, int y0, int x1, int y1) {
    int n = 0;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const Rgb p = a.at(x, y), q = b.at(x, y);
            if (p.r != q.r || p.g != q.g || p.b != q.b) { ++n; }
        }
    }
    return n;
}

// ---- the specimen's table (fitted_specimen.slint) ----
constexpr int kW = 1200, kH = 1660;
constexpr int kF1x = 10, kF1y = 24, kF1w = 1180, kF1h = 380;          // FM1: the wide page, a one-row bar
constexpr int kF2x = 10, kF2y = 430, kF2w = 700, kF2h = 300;          // FM2: 700 wide, the chips on a second line
constexpr int kF3x = 730, kF3y = 430, kF3w = 460, kF3h = 230;         // FM3: nothing fitted
constexpr int kM1x = 10, kM1y = 760, kM2x = 610, kMw = 580, kMh = 600;
constexpr int kM3x = 10, kM3y = 1390;                                  // MP3: the reach rows alone, 240 high
constexpr int kBar1 = 40, kLines1 = 50;                                // FM1's bar (8 + 24 + 8) and its verdict + report lines
constexpr int kHeadH = 28, kRowH = 25, kNoteRowH = 44, kChipH = 20;
// FM1's rows at scroll 0: the offset of each row's top rule in the body, whether it carries a note, and the headings' offsets
constexpr int kRowOff[12] = {28, 53, 78, 103, 175, 219, 244, 269, 322, 347, 372, 397};
constexpr bool kRowNote[12] = {false, false, false, true, true, false, false, false, false, false, false, false};
constexpr int kHeadOff[3] = {0, 147, 294};
constexpr int kBodyH1 = kF1h - kBar1 - kLines1;                        // 290
constexpr int kContent1 = 486;                                         // 422 of rows + the 64 px foot

}  // namespace

int main() {
    installHeadlessPlatform();
    auto ui = FittedSpecimen::create();
    slint::Window& win = ui->window();

    // The tokens, read from the Workbench skin by name (the theme test pins their values).
    const Skin wb = ui->global<Workbench>().get_value();
    const Rgb bg = tok(wb.bg), strip = tok(wb.strip), panel = tok(wb.panel), panelAlt = tok(wb.panel_alt), field = tok(wb.field), well = tok(wb.well);
    const Rgb line = tok(wb.line), lineStrong = tok(wb.line_strong), fg = tok(wb.fg), fgSoft = tok(wb.fg_soft), muted = tok(wb.muted), dim = tok(wb.dim);
    const Rgb disabledInk = tok(wb.disabled), accent = tok(wb.accent), accentInk = tok(wb.accent_ink), accentSoft = tok(wb.accent_soft), errFg = tok(wb.chip_err_fg);
    const Rgb goodFg = tok(wb.chip_good_fg), goodBg = tok(wb.chip_good_bg), goodLine = tok(wb.chip_good_line), warnBg = tok(wb.chip_warn_bg), warnLine = tok(wb.chip_warn_line);
    const Rgb warnFg = tok(wb.chip_warn_fg), errBg = tok(wb.chip_err_bg), errLine = tok(wb.chip_err_line), chipBg = tok(wb.chip_bg), chipLine = tok(wb.chip_line), chipFg = tok(wb.chip_fg);
    const Rgb keyBg = tok(wb.key_bg), keyLine = tok(wb.key_line), keyFg = tok(wb.key_fg), plateLine = tok(wb.plate_line), plateFg = tok(wb.plate_fg);
    const Rgb dangerBg = tok(wb.danger_bg), dangerLine = tok(wb.danger_line), dangerFg = tok(wb.danger_fg), keyDisBg = tok(wb.key_disabled_bg), keyDisFg = tok(wb.key_disabled_fg);
    const Rgb lampGreen = tok(wb.lamp_green), lampAmber = tok(wb.lamp_amber), lampRed = tok(wb.lamp_red), lampOff = tok(wb.lamp_off), lampGreenCap = tok(wb.lamp_green_cap);
    const Rgb lampRedCap = tok(wb.lamp_red_cap), knobRing = tok(wb.knob_ring), menu = tok(wb.menu), focus = tok(wb.focus), faint = tok(wb.faint), cardBg = tok(wb.card);
    // the artboard's own values, once, so a drift of the Workbench filling is caught here too
    CHECK(nearColour(strip, rgb(0x191c21), 0) && nearColour(panelAlt, rgb(0x1a1d22), 0) && nearColour(well, rgb(0x0d0f12), 0));
    CHECK(nearColour(goodBg, rgb(0x163626), 0) && nearColour(errFg, rgb(0xff9b9b), 0) && nearColour(accentSoft, rgb(0xf5c66b), 0));

    const auto clk = [&](int x, int y) { click(win, static_cast<float>(x), static_cast<float>(y)); };
    const auto mv = [&](int x, int y) { moveTo(win, static_cast<float>(x), static_cast<float>(y)); };
    const auto tap = [&](std::u8string_view k) { keyTap(win, k); };
    const auto shot = [&] { return render(ui, kW, kH); };
    const auto reset = [&] {
        ui->set_row_clicks(0); ui->set_last_row(-1); ui->set_row_doubles(0); ui->set_last_double(-1); ui->set_runs(0); ui->set_last_run(-1);
        ui->set_removes(0); ui->set_last_remove(-1); ui->set_chip_toggles(0); ui->set_last_chip(-1); ui->set_search_edits(0); ui->set_last_search("");
        ui->set_scans(0); ui->set_resets(0); ui->set_cleanups(0); ui->set_last_source(""); ui->set_page_events(0); ui->set_last_page_event("");
        ui->set_last_page_value(false); ui->set_last_command(-1);
    };
    // Parks the pointer on the sheet's empty corner: closes any tooltip and ends any hover.
    const auto park = [&] { mv(1195, 1655); settle(milliseconds(30)); };
    const auto sval = [&](const slint::SharedString& s) { return std::string(s); };
    // Where every scrolling body of the sheet is now: the sum of their offsets, so that one number says "nothing is still moving"
    const auto scrolledNow = [&] {
        return static_cast<double>(ui->get_fm1_scrolled()) + ui->get_mp1_scrolled() + ui->get_mp2_scrolled() + ui->get_mp3_scrolled();
    };
    // A wheel notch over (x, y): the pointer move first, as the OS sends one, then the notch, then wait until the scroll animation has
    // stopped: the offsets alike in three reads 40 ms apart, after at least 120 ms (a notch the body has to animate to starts a beat late)
    const auto wheel = [&](int x, int y, float dy) {
        mv(x, y);
        scroll(win, static_cast<float>(x), static_cast<float>(y), 0.0f, dy);
        const auto start = std::chrono::steady_clock::now();
        double prev = -1e9;
        int still = 0;
        while (std::chrono::steady_clock::now() - start < std::chrono::seconds(4)) {
            settle(milliseconds(40));
            shot();
            const double cur = scrolledNow();
            still = std::abs(cur - prev) < 0.01 ? still + 1 : 0;
            prev = cur;
            if (still >= 3 && std::chrono::steady_clock::now() - start >= milliseconds(120)) { break; }
        }
    };

    // =========================================================================================================
    // 1. The sheet, before anything is touched
    // =========================================================================================================
    park();
    const Frame f0 = shot();
    CHECK(f0.width == kW && f0.height == kH);
    CHECK(f0.near(1195, 1655, bg));
    f0.savePng(shotDir() + "fitted-specimen.png");
    const int bodyTop1 = kF1y + static_cast<int>(ui->get_fm1_bar_height()) + static_cast<int>(ui->get_fm1_lines_height());
    const auto rowTop = [&](int i) { return bodyTop1 + kRowOff[i]; };
    const auto rowMid = [&](int i) { return rowTop(i) + (kRowNote[i] ? 22 : 13); };          // the middle of the chip and keys of row i

    // =========================================================================================================
    // 2. FM1's top bar: the strip ground and a rule, the search box, the six chips, the two keys at the right edge
    // =========================================================================================================
    const int fm1Mid = kF1y + 8 + 12;                                       // the middle of the bar's one line
    const std::vector<Span> chips1 = runsOnRow(f0, fm1Mid, kF1x + 250, kF1x + 890, strip);
    const std::vector<Span> keys1 = runsOnRow(f0, fm1Mid, kF1x + 890, kF1x + kF1w, strip);
    {
        CHECK_NEAR(ui->get_fm1_bar_height(), 40.0, 0.01);
        CHECK(!ui->get_fm1_two_rows());
        CHECK(f0.near(kF1x + 3, kF1y + 3, strip) && f0.near(kF1x + kF1w - 3, kF1y + 3, strip) && f0.near(kF1x + 600, kF1y + 30, strip));
        CHECK(f0.rowNear(kF1y + 39, kF1x, kF1x + kF1w, line));                                           // the rule under the bar: its last pixel row
        CHECK(f0.near(kF1x + 3, kF1y + 38, strip) && f0.near(kF1x + 3, kF1y + 40, panelAlt));
        CHECK(f0.near(kF1x - 1, kF1y + 3, bg) && f0.near(kF1x + 5, kF1y - 1, bg));                      // the page's own box: 1180 wide from x 10
        // the search box: 220 x 24 at 12 px in, the field ground, a strong border, the icon and the placeholder
        const int sx = kF1x + 12, sy = kF1y + 8;
        CHECK(f0.rowNear(sy, sx + 8, sx + 212, lineStrong) && f0.rowNear(sy + 23, sx + 8, sx + 212, lineStrong));
        CHECK(f0.near(sx, sy + 12, lineStrong) && f0.near(sx + 219, sy + 12, lineStrong) && f0.near(sx + 4, sy + 12, field));
        CHECK(f0.near(sx + 220, sy + 12, strip) && f0.near(sx + 20, sy - 1, strip) && f0.near(sx + 20, sy + 24, strip));
        CHECKV(f0.countNear(sx + 8, sy + 5, sx + 22, sy + 19, dim, 40) >= 6, "search icon in dim: %d", f0.countNear(sx + 8, sy + 5, sx + 22, sy + 19, dim, 40));
        const Ink ph = inkBox(f0, sx + 24, sy + 2, sx + 212, sy + 22, field);
        CHECKV(ph.n > 40 && ph.x0 >= sx + 28 && ph.x0 <= sx + 31, "placeholder ink n=%d starts %d px in", ph.n, ph.x0 - sx);
        CHECK(f0.countNear(sx + 24, sy + 2, sx + 212, sy + 22, disabledInk, 40) >= 12);
        // the chips: six pills 20 px high, 6 px apart, the first 18 px after the search box
        CHECKV(chips1.size() == 6, "%zu chips on the bar's line", chips1.size());
        CHECKV(spanAt(chips1, 0).l == sx + 220 + 18, "first chip at %d, wanted %d", spanAt(chips1, 0).l, sx + 238);
        for (std::size_t i = 1; i < 6; ++i) { CHECK(spanAt(chips1, i).l - spanAt(chips1, i - 1).r == 6); }
        CHECK(f0.near(spanAt(chips1, 0).mid(), kF1y + 10, goodLine, 5) && f0.near(spanAt(chips1, 0).mid(), kF1y + 10 + 19, goodLine, 5));   // 20 px high, 10 px under the bar's top
        CHECK(f0.near(spanAt(chips1, 0).l + 3, fm1Mid, goodBg, 5));
        CHECK(f0.near(spanAt(chips1, 5).l + 3, fm1Mid, errBg, 5));
        // the two keys: small (24 high), 10 px apart, the last 12 px from the right edge
        CHECKV(keys1.size() == 2, "%zu keys at the right of the bar", keys1.size());
        CHECK(spanAt(keys1, 1).r == kF1x + kF1w - 12 && spanAt(keys1, 1).l - spanAt(keys1, 0).r == 10);
        for (const Span& k : {spanAt(keys1, 0), spanAt(keys1, 1)}) {
            CHECK(f0.near(k.l, fm1Mid, keyLine) && f0.near(k.r - 1, fm1Mid, keyLine) && f0.near(k.l + 3, fm1Mid, keyBg));
            CHECK(f0.near(k.l + 8, kF1y + 8, keyLine) && f0.near(k.l + 8, kF1y + 8 + 23, keyLine) && f0.near(k.l + 8, kF1y + 7, strip));
            CHECK(f0.countNear(k.l + 2, kF1y + 10, k.r - 2, kF1y + 30, keyFg, 40) >= 25);
        }
        CHECKV(spanAt(keys1, 0).w() >= 70 && spanAt(keys1, 1).w() >= 120, "SCAN AGAIN is %d wide, RESET WINDOW SIZES %d", spanAt(keys1, 0).w(), spanAt(keys1, 1).w());
    }

    // =========================================================================================================
    // 3. FM1's verdict line and report: muted, one line each under the bar; the report in chip-good-fg
    // =========================================================================================================
    {
        CHECK_NEAR(ui->get_fm1_lines_height(), 50.0, 1.01);
        const int top = kF1y + kBar1;
        const Ink verdict = inkBox(f0, kF1x, top + 4, kF1x + kF1w - 14, top + 8 + 17, panelAlt);
        CHECKV(verdict.n > 600 && verdict.x0 >= kF1x + 12 && verdict.x0 <= kF1x + 14 && verdict.y0 >= top + 8, "verdict ink n=%d from (%d, %d)", verdict.n, verdict.x0, verdict.y0);
        CHECKV(verdict.x1 < kF1x + kF1w - 14 && verdict.h() <= 16, "the verdict is one line: x %d, %d rows", verdict.x1, verdict.h());
        CHECKV(f0.countNear(kF1x + 12, top + 6, kF1x + kF1w - 14, top + 25, muted, 40) >= 200, "verdict in muted: %d", f0.countNear(kF1x + 12, top + 6, kF1x + kF1w - 14, top + 25, muted, 40));
        // the directory follows after a middle dot, in the same line: ink reaches well past the first sentence
        CHECKV(verdict.x1 - verdict.x0 > 800, "verdict and directory share the line: %d px", verdict.x1 - verdict.x0);
        const Ink report = inkBox(f0, kF1x, top + 8 + 17, kF1x + kF1w - 14, top + 50, panelAlt);
        CHECKV(report.n > 50 && report.y0 >= top + 8 + 15 + 3 && report.x0 <= kF1x + 14, "report ink n=%d from row %d (the verdict ends at %d)", report.n, report.y0, top + 8 + 15);
        CHECK(f0.countNear(kF1x + 12, top + 8 + 17, kF1x + 400, top + 50, goodFg, 40) >= 40);
        CHECK(f0.countNear(kF1x + 12, top + 8 + 17, kF1x + 400, top + 50, errFg, 20) == 0);
        // an error is said instead of the report, in chip-err-fg
        ui->set_fm1_error("Not removed: the file is in use by another program.");
        const Frame withError = shot();
        CHECKV(withError.countNear(kF1x + 12, top + 8 + 17, kF1x + 600, top + 50, errFg, 40) >= 60 && withError.countNear(kF1x + 12, top + 8 + 17, kF1x + 600, top + 50, goodFg, 12) == 0, "the error line: %d red, %d green", withError.countNear(kF1x + 12, top + 8 + 17, kF1x + 600, top + 50, errFg, 40), withError.countNear(kF1x + 12, top + 8 + 17, kF1x + 600, top + 50, goodFg, 12));
        CHECK_NEAR(ui->get_fm1_lines_height(), 50.0, 1.01);                                               // one line replaces one line
        ui->set_fm1_error("");
        CHECK(shot().countNear(kF1x + 12, top + 8 + 17, kF1x + 400, top + 50, goodFg, 40) >= 40);        // and the report is back
    }

    // =========================================================================================================
    // 4. FM1's rows: 25 px (44 with a note) with a 1 px rule on top; the dot, the name and version, the chip and the xs keys at the right.
    //    The body shows the first seven rows at rest and the last five when it is scrolled to its end; every row is checked in one of them.
    // =========================================================================================================
    struct Expect { const char* what; Rgb dot; bool dotOn; Rgb chipGround, chipEdge, chipInk; int keys; bool lastKeyDanger; };
    const Expect ex[12] = {
        {"FED ADS-B", lampGreen, true, goodBg, goodLine, goodFg, 2, false},
        {"IDLE NOAA", lampOff, false, chipBg, chipLine, chipFg, 2, false},
        {"STOPPED POCSAG", lampOff, false, chipBg, chipLine, chipFg, 2, false},
        {"REFUSED FT8", lampRed, true, errBg, errLine, errFg, 1, false},
        {"TAKES NO SIGNAL maps", lampOff, false, chipBg, chipLine, chipFg, 2, false},
        {"NOT DECODING frame log", lampAmber, true, warnBg, warnLine, warnFg, 2, true},
        {"TAKES NO SIGNAL tracker", lampOff, false, chipBg, chipLine, chipFg, 2, false},
        {"FED long name", lampGreen, true, goodBg, goodLine, goodFg, 2, false},
        {"FED RDS", lampGreen, true, goodBg, goodLine, goodFg, 2, false},
        {"FED blanker", lampGreen, true, goodBg, goodLine, goodFg, 2, false},
        {"IDLE equaliser", lampOff, false, chipBg, chipLine, chipFg, 2, false},
        {"STOPPED vocoder", lampOff, false, chipBg, chipLine, chipFg, 2, false},
    };
    // One row of FM1 as drawn in frame `f` with the body scrolled by `scrolled` px; the row must lie wholly inside the body.
    const auto checkRow = [&](const Frame& f, int i, int scrolled) {
        const int top = rowTop(i) - scrolled, h = kRowNote[i] ? kNoteRowH : kRowH, mid = top + (kRowNote[i] ? 22 : 13), cy = kRowNote[i] ? 12 : 3;
        const Expect& e = ex[i];
        CHECKV(top >= bodyTop1 && top + h <= bodyTop1 + kBodyH1, "%s: row %d at %d..%d is not inside the body %d..%d", e.what, i, top, top + h, bodyTop1, bodyTop1 + kBodyH1);
        CHECKV(f.rowNear(top, kF1x + 2, kF1x + kF1w - 12, line), "%s: the rule on row %d", e.what, top);
        CHECK(f.near(kF1x + 600, top + 2, panelAlt) && f.near(kF1x + 600, top + h - 1, panelAlt));              // 25 px: the next rule is on row top + h
        // the state dot: 8 px, 8 px in, on the name's line
        const int dy = kRowNote[i] ? 15 : 13;
        if (e.dotOn) { CHECKV(f.near(kF1x + 12, top + dy, e.dot, 8), "%s: dot %s", e.what, hex(f.at(kF1x + 12, top + dy)).c_str()); }
        else { CHECKV(f.near(kF1x + 12, top + dy, lampOff, 10) && f.near(kF1x + 8, top + dy, tok(wb.lamp_ring), 10), "%s: unlit dot %s / %s", e.what, hex(f.at(kF1x + 12, top + dy)).c_str(), hex(f.at(kF1x + 8, top + dy)).c_str()); }
        // the name: fg, 12 px / 500, starting 23 px in
        const int nameTop = top + (kRowNote[i] ? 5 : 3), nameBot = top + (kRowNote[i] ? 23 : 22);
        const Ink name = inkBox(f, kF1x + 20, nameTop, kF1x + 700, nameBot, panelAlt);
        CHECKV(name.n > 40 && name.x0 >= kF1x + 23 && name.x0 <= kF1x + 26, "%s: name ink n=%d from x %d", e.what, name.n, name.x0 - kF1x);
        CHECKV(f.countNear(kF1x + 23, nameTop, kF1x + 400, nameBot, fg, 40) >= 15, "%s: name in fg", e.what);
        CHECKV(std::abs(name.cy() - (top + (kRowNote[i] ? 15.0 : 13.0))) <= 4.0, "%s: name centre %.1f vs the row's %d", e.what, name.cy(), top + (kRowNote[i] ? 15 : 13));
        // the cluster: the chip, then the keys, 6 px apart, ending 10 px short of the right edge, 20 px high
        const std::vector<Span> cl = clusterOnRow(f, mid, kF1x + 600, kF1x + kF1w - 9, panelAlt);
        CHECKV(static_cast<int>(cl.size()) == 1 + e.keys, "%s: %zu elements at the right (wanted %d)", e.what, cl.size(), 1 + e.keys);
        CHECKV(spanAt(cl, cl.size() - 1).r == kF1x + kF1w - 10, "%s: ends at %d, wanted %d", e.what, spanAt(cl, cl.size() - 1).r, kF1x + kF1w - 10);
        const Span chip = spanAt(cl, 0);
        CHECKV(chip.w() >= 34 && f.near(chip.l + 3, mid, e.chipGround, 5) && f.near(chip.mid(), top + cy, e.chipEdge, 5) && f.near(chip.mid(), top + cy + 19, e.chipEdge, 5),
               "%s: chip %d wide, ground %s edge %s", e.what, chip.w(), hex(f.at(chip.l + 3, mid)).c_str(), hex(f.at(chip.mid(), top + cy)).c_str());
        CHECK(f.near(chip.mid(), top + cy - 1, panelAlt) && f.near(chip.mid(), top + cy + 20, panelAlt));
        CHECKV(f.countNear(chip.l + 6, top + cy + 3, chip.r - 6, top + cy + 17, e.chipInk, 40) >= 8, "%s: chip word in its tone: %d", e.what, f.countNear(chip.l + 6, top + cy + 3, chip.r - 6, top + cy + 17, e.chipInk, 40));
        for (int k = 1; k <= e.keys; ++k) {
            const Span key = spanAt(cl, static_cast<std::size_t>(k));
            const bool danger = e.lastKeyDanger && k == e.keys;
            CHECKV(f.near(key.l, mid, danger ? dangerLine : keyLine) && f.near(key.l + 3, mid, danger ? dangerBg : keyBg), "%s: key %d ground %s edge %s", e.what, k, hex(f.at(key.l + 3, mid)).c_str(), hex(f.at(key.l, mid)).c_str());
            CHECK(f.near(key.l + 8, top + cy, danger ? dangerLine : keyLine) && f.near(key.l + 8, top + cy + 19, danger ? dangerLine : keyLine));      // .btn.xs: 20 px
            CHECK(f.near(key.l + 8, top + cy - 1, panelAlt) && f.near(key.l + 8, top + cy + 20, panelAlt));
            CHECKV(f.countNear(key.l + 2, top + cy + 3, key.r - 2, top + cy + 17, danger ? dangerFg : keyFg, 40) >= 8, "%s: key %d label", e.what, k);
            CHECK(key.w() >= 30);
        }
    };
    // A group's cap heading: a cap in muted 10 px in, nothing else on its line.
    const auto checkHeading = [&](const Frame& f, int g, int scrolled) {
        const int top = bodyTop1 + kHeadOff[g] - scrolled;
        CHECKV(top >= bodyTop1 && top + kHeadH <= bodyTop1 + kBodyH1, "heading %d at %d is not inside the body", g, top);
        const Ink cap = inkBox(f, kF1x, top + 1, kF1x + 400, top + kHeadH - 1, panelAlt);
        CHECKV(cap.n > 30 && cap.x0 >= kF1x + 10 && cap.x0 <= kF1x + 12 && cap.y0 >= top + 7 && cap.y1 <= top + 24, "heading %d ink n=%d x0 %d rows %d..%d", g, cap.n, cap.x0 - kF1x, cap.y0 - top, cap.y1 - top);
        CHECKV(f.countNear(kF1x + 10, top + 1, kF1x + 400, top + kHeadH - 1, muted, 40) >= 12, "heading %d in muted", g);
        CHECK(f.countInk(kF1x + 400, top + 1, kF1x + kF1w - 14, top + kHeadH - 1, panelAlt) == 0);                // nothing else on the heading's line
        return cap;
    };
    {
        for (int i = 0; i < 7; ++i) { checkRow(f0, i, 0); }                                                    // the rows the body shows at rest
        // the words of the keys: STOP for a module being fed, START for a stopped or idle one: START is wider than STOP by a letter
        const std::vector<Span> fedRow = clusterOnRow(f0, rowMid(0), kF1x + 600, kF1x + kF1w - 9, panelAlt);
        const std::vector<Span> idleRow = clusterOnRow(f0, rowMid(1), kF1x + 600, kF1x + kF1w - 9, panelAlt);
        CHECKV(spanAt(idleRow, 1).w() > spanAt(fedRow, 1).w(), "START key %d px, STOP key %d px", spanAt(idleRow, 1).w(), spanAt(fedRow, 1).w());
        // REMOVE FILE (a refused module) is wider than REMOVE
        const std::vector<Span> refusedRow = clusterOnRow(f0, rowMid(3), kF1x + 600, kF1x + kF1w - 9, panelAlt);
        CHECKV(spanAt(refusedRow, 1).w() > spanAt(fedRow, 2).w() + 20, "REMOVE FILE key %d px, REMOVE key %d px", spanAt(refusedRow, 1).w(), spanAt(fedRow, 2).w());
        // the version, muted, 8 px after the name
        const Ink first = inkBox(f0, kF1x + 23, rowTop(0) + 3, kF1x + 500, rowTop(0) + 22, panelAlt);
        CHECKV(first.x1 > kF1x + 150 && first.x1 < kF1x + 260, "ADS-B name and version end at %d", first.x1 - kF1x);
        CHECK(f0.countNear(kF1x + 170, rowTop(0) + 3, kF1x + 230, rowTop(0) + 22, muted, 40) >= 8);          // "1.4.1" in muted
        // the 44 px row with its note: the name on the first line, the note under it, 23 px down, in the note's tone
        const int t3 = rowTop(3);
        const Ink note = inkBox(f0, kF1x + 20, t3 + 22, kF1x + 700, t3 + 42, panelAlt);
        CHECKV(note.n > 60 && note.x0 >= kF1x + 23 && note.x0 <= kF1x + 26 && note.y0 >= t3 + 24, "note ink n=%d from (%d, %d)", note.n, note.x0 - kF1x, note.y0 - t3);
        CHECK(f0.countNear(kF1x + 23, t3 + 22, kF1x + 700, t3 + 42, errFg, 40) >= 40);                    // REFUSED: the note in chip-err-fg
        CHECK(f0.countNear(kF1x + 23, rowTop(4) + 22, kF1x + 700, rowTop(4) + 42, accentSoft, 40) >= 60); // a warning note: accent-soft
        CHECK(f0.countNear(kF1x + 23, rowTop(4) + 22, kF1x + 700, rowTop(4) + 42, errFg, 20) == 0);
        // a 44 px row is 44 px tall, not 25: the next group's heading follows it, and its first rule is that heading and 28 px further down
        CHECK(f0.rowNear(t3 + kNoteRowH + kHeadH, kF1x + 2, kF1x + kF1w - 12, line));
        CHECK(!f0.rowNear(t3 + kRowH, kF1x + 2, kF1x + kF1w - 12, line));
    }

    // =========================================================================================================
    // 5. The group headings: a cap above the first row of a group only
    // =========================================================================================================
    {
        const Ink cap1 = checkHeading(f0, 0, 0);
        const Ink cap2 = checkHeading(f0, 1, 0);
        // no heading between rows of one group: the rows after DECODERS' first are 25 px apart, rule to rule, with nothing between name and keys
        CHECK(f0.rowNear(rowTop(1), kF1x + 2, kF1x + kF1w - 12, line) && f0.rowNear(rowTop(2), kF1x + 2, kF1x + kF1w - 12, line));
        CHECKV(f0.countInk(kF1x + 400, rowTop(1) + 2, kF1x + 900, rowTop(1) + kRowH - 1, panelAlt) == 0, "ink between the name and the keys of a plain row");
        // the second heading is MAPS AND TOOLS, upper-case: wider than DECODERS, a cap height of 7-8 px, no descenders
        CHECKV(cap2.w() > cap1.w() && cap1.h() <= 9 && cap2.h() <= 9, "headings: DECODERS %d px wide, MAPS AND TOOLS %d, %d / %d rows tall", cap1.w(), cap2.w(), cap1.h(), cap2.h());
    }

    // =========================================================================================================
    // 6. The body scrolls under the bar and the lines: the wheel moves it, a 6 px thumb shows where it is and can be dragged
    // =========================================================================================================
    {
        CHECK_NEAR(ui->get_fm1_content(), kContent1, 1.01);
        const int thumbX = kF1x + kF1w - 9;
        // the thumb at rest: the top of the track, 6 wide, as tall as the body is of the content
        const double want = kBodyH1 * static_cast<double>(kBodyH1) / ui->get_fm1_content();
        CHECK(f0.colNear(thumbX + 3, bodyTop1 + 2, bodyTop1 + static_cast<int>(want) - 2, knobRing));
        CHECK(!f0.near(thumbX + 3, bodyTop1 + static_cast<int>(want) + 3, knobRing, 3));                      // it ends where the body's share of the content ends
        CHECK(!f0.near(thumbX - 1, bodyTop1 + 50, knobRing, 3) && !f0.near(thumbX + 6, bodyTop1 + 50, knobRing, 3));
        CHECKV(f0.rowNear(bodyTop1 + 50, thumbX, thumbX + 6, knobRing), "the thumb is 6 px wide at 9 px from the edge");
        // one small notch: everything moves by what the wheel said, the bar and the lines stay
        wheel(600, 250, -30.0f);
        const double a = ui->get_fm1_scrolled();
        CHECKV(a > 5 && a <= 60, "scrolled by %.1f after a small notch", a);
        const Frame s1 = shot();
        CHECK(s1.rowNear(rowTop(1) - static_cast<int>(std::lround(a)), kF1x + 2, kF1x + kF1w - 12, line));      // row 1's rule moved up by that much
        CHECK(differing(f0, s1, kF1x, kF1y, kF1x + kF1w - 14, kF1y + kBar1 + kLines1) == 0);                   // the bar and the lines did not
        const int thumbTop = s1.firstRowNear(thumbX + 3, bodyTop1, bodyTop1 + kBodyH1, knobRing);
        CHECKV(thumbTop > bodyTop1 && thumbTop <= bodyTop1 + 40, "the thumb moved down to row %d", thumbTop);
        // the end of the body: the last rows and the foot are in view, the thumb at the bottom of its track
        wheel(600, 250, -1000.0f);
        const int end = static_cast<int>(std::lround(ui->get_fm1_scrolled()));
        CHECK_NEAR(ui->get_fm1_scrolled(), ui->get_fm1_content() - kBodyH1, 0.6);
        const Frame s2 = shot();
        const int thumbBottom = [&] { int y = bodyTop1 + kBodyH1 - 1; while (y > bodyTop1 && !s2.near(thumbX + 3, y, knobRing)) { --y; } return y; }();
        CHECKV(thumbBottom >= bodyTop1 + kBodyH1 - 3, "the thumb ends at row %d, the body ends at %d", thumbBottom, bodyTop1 + kBodyH1);
        for (int i = 7; i < 12; ++i) { checkRow(s2, i, end); }                                                 // the rows the end of the body shows
        checkHeading(s2, 2, end);
        // the long name of row 7 is cut 8 px short of the keys and gives up its version
        {
            const int top = rowTop(7) - end;
            const std::vector<Span> cl = clusterOnRow(s2, top + 13, kF1x + 600, kF1x + kF1w - 9, panelAlt);
            const int left = spanAt(cl, 0).l;
            const Ink name = inkBox(s2, kF1x + 20, top + 3, left, top + 22, panelAlt);
            CHECKV(name.x1 <= left - 8 && name.x1 >= left - 26, "the cut name ends at %d, the keys start at %d", name.x1, left);
            CHECK(s2.countInk(left - 7, top + 3, left, top + 22, panelAlt) == 0);                              // 8 px of air before the keys
            CHECKV(name.w() > 700, "the long name is cut, not dropped: %d px", name.w());
        }
        // the foot: the CLEAN UP OLD VERSIONS key, small, 12 px in; beside it the three old copies, muted, one under the other
        const int footTop = bodyTop1 + kBodyH1 - 64;                                                    // 12 + 40 + 12
        CHECKV(s2.near(kF1x + 12, footTop + 12 + 12, keyLine) && s2.near(kF1x + 12 + 3, footTop + 12 + 12, keyBg), "foot key ground %s", hex(s2.at(kF1x + 15, footTop + 24)).c_str());
        CHECK(s2.near(kF1x + 12 + 8, footTop + 12, keyLine) && s2.near(kF1x + 12 + 8, footTop + 12 + 23, keyLine));
        const std::vector<Span> fk = runsOnRow(s2, footTop + 24, kF1x + 4, kF1x + 700, panelAlt);
        CHECKV(fk.size() >= 2 && fk[0].l == kF1x + 12 && fk[0].w() >= 150, "the foot key is %d wide from x %d", spanAt(fk, 0).w(), spanAt(fk, 0).l);
        const Ink old = inkBox(s2, spanAt(fk, 0).r + 8, footTop + 8, kF1x + 900, footTop + 60, panelAlt);
        CHECKV(old.n > 200 && old.x0 >= spanAt(fk, 0).r + 14 && old.x0 <= spanAt(fk, 0).r + 18 && old.h() >= 32, "old copies ink n=%d from x %d, %d rows tall", old.n, old.x0 - spanAt(fk, 0).r, old.h());
        CHECK(s2.countNear(spanAt(fk, 0).r + 14, footTop + 8, kF1x + 900, footTop + 60, muted, 40) >= 80);
        // the last row (STOPPED vocoder) ends 12 px above the foot
        CHECK(s2.rowNear(footTop - kRowH, kF1x + 2, kF1x + kF1w - 12, line));
        // what the last clean-up did is said under the key, in chip-good-fg, and the foot grows by it
        {
            const double before = ui->get_fm1_content();
            ui->set_fm1_cleanup_report("Removed 3 files: the old copies of adsb-decoder, noaa-apt and frame-log.");
            shot();
            CHECKV(ui->get_fm1_content() >= before + 20 && ui->get_fm1_content() <= before + 28, "the foot grew from %.1f to %.1f", before, ui->get_fm1_content());
            wheel(600, 250, -1000.0f);
            const Frame r2 = shot();
            const int ft = bodyTop1 + kBodyH1 - static_cast<int>(std::lround(ui->get_fm1_content() - before)) - 64;
            const int under = ft + 12 + 40 + 8;                                                              // the key row is as tall as the three old copies, then 8 px
            CHECKV(r2.countNear(kF1x + 12, under, kF1x + 700, under + 17, goodFg, 40) >= 40 && r2.near(kF1x + 12 + 3, ft + 24, keyBg), "the clean-up report in chip-good-fg: %d", r2.countNear(kF1x + 12, under, kF1x + 700, under + 17, goodFg, 40));
            ui->set_fm1_cleanup_report("");
            wheel(600, 250, -1000.0f);
        }
        // dragging the thumb moves the content in step: 50 px of thumb is 50 x overflow / travel of content
        const double travel = kBodyH1 - want, overflow = ui->get_fm1_content() - kBodyH1;
        wheel(600, 250, 2000.0f);
        press(win, static_cast<float>(thumbX + 3), static_cast<float>(bodyTop1 + 20));
        moveTo(win, static_cast<float>(thumbX + 3), static_cast<float>(bodyTop1 + 20 + 50));
        moveTo(win, static_cast<float>(thumbX + 3), static_cast<float>(bodyTop1 + 20 + 50));
        release(win, static_cast<float>(thumbX + 3), static_cast<float>(bodyTop1 + 20 + 50));
        const double dragged = ui->get_fm1_scrolled();
        CHECKV(std::abs(dragged - 50.0 * overflow / travel) <= 2.0, "dragged 50 px: scrolled %.1f, wanted %.1f", dragged, 50.0 * overflow / travel);
        wheel(600, 250, 2000.0f);
        CHECK_NEAR(ui->get_fm1_scrolled(), 0.0, 0.01);
        // a wheel over the bar scrolls nothing
        wheel(600, kF1y + 20, -300.0f);
        CHECK_NEAR(ui->get_fm1_scrolled(), 0.0, 0.01);
        park();
    }

    // =========================================================================================================
    // 8. FM2: a 700 px page puts the chips on a second line of the bar; an error line in chip-err-fg; rows without groups
    // =========================================================================================================
    {
        CHECK(ui->get_fm2_two_rows());
        CHECK_NEAR(ui->get_fm2_bar_height(), 68.0, 0.01);                                                 // 8 + 24 + 8 + 20 + 8
        CHECK(f0.near(kF2x + 3, kF2y + 3, strip) && f0.rowNear(kF2y + 67, kF2x, kF2x + kF2w, line) && f0.near(kF2x + 3, kF2y + 68, panelAlt));
        // line one: the search box and the keys; line two: the chips, at the left padding, 8 px under the first line
        const int l1 = kF2y + 8 + 12, l2 = kF2y + 8 + 24 + 8 + 10;
        const std::vector<Span> k2 = runsOnRow(f0, l1, kF2x + 300, kF2x + kF2w, strip);
        CHECKV(k2.size() == 2 && spanAt(k2, 1).r == kF2x + kF2w - 12, "FM2 keys: %zu, the last ends at %d", k2.size(), spanAt(k2, 1).r);
        const std::vector<Span> c2 = runsOnRow(f0, l2, kF2x, kF2x + kF2w, strip);
        CHECKV(c2.size() == 6 && spanAt(c2, 0).l == kF2x + 12, "FM2 chips: %zu, the first at %d", c2.size(), spanAt(c2, 0).l - kF2x);
        CHECK(f0.near(spanAt(c2, 0).mid(), kF2y + 8 + 24 + 8, goodLine, 5) && f0.near(spanAt(c2, 0).mid(), kF2y + 8 + 24 + 8 + 19, goodLine, 5));
        CHECK(f0.countInk(kF2x + 12, kF2y + 8 + 24 + 1, kF2x + 100, kF2y + 8 + 24 + 7, strip) == 0);   // 8 px between the lines
        CHECK(f0.near(kF2x + 12, kF2y + 8 + 12, lineStrong) && f0.near(kF2x + 12 + 219, kF2y + 8 + 12, lineStrong));   // the search box is 220 wide here too
        // the verdict wraps to two lines at this width; the error is red and under it
        const int top = kF2y + 68;
        CHECKV(ui->get_fm2_lines_height() >= 60 && ui->get_fm2_lines_height() <= 70, "FM2 lines are %.1f high", ui->get_fm2_lines_height());
        const Ink verdict = inkBox(f0, kF2x, top + 4, kF2x + kF2w - 4, top + 8 + 32, panelAlt);
        CHECKV(verdict.h() >= 24, "the verdict wraps: %d rows of ink", verdict.h());
        CHECK(f0.countNear(kF2x + 12, top + 8 + 30, kF2x + kF2w - 12, top + static_cast<int>(ui->get_fm2_lines_height()), errFg, 40) >= 80);
        CHECK(f0.countNear(kF2x + 12, top + 6, kF2x + kF2w - 12, top + 8 + 32, errFg, 20) == 0);
        // rows: no group, so no heading: the first rule is the first line of the body, 0 px under the lines
        const int bt = top + static_cast<int>(ui->get_fm2_lines_height());
        CHECK(f0.rowNear(bt, kF2x + 2, kF2x + kF2w - 12, line));
        CHECK(f0.rowNear(bt + kRowH, kF2x + 2, kF2x + kF2w - 12, line) && f0.rowNear(bt + 2 * kRowH, kF2x + 2, kF2x + kF2w - 12, line));
        // row 1 has its REMOVE armed: the last key is CONFIRM, red; row 2's name is empty so the file name stands in; row 3's long note is cut
        const std::vector<Span> armed = clusterOnRow(f0, bt + kRowH + 13, kF2x + 300, kF2x + kF2w - 9, panelAlt);
        CHECKV(armed.size() == 3 && f0.near(spanAt(armed, 2).l + 3, bt + kRowH + 13, dangerBg) && f0.near(spanAt(armed, 2).l, bt + kRowH + 13, dangerLine), "armed row: %zu elements, the last ground %s", armed.size(), hex(f0.at(spanAt(armed, 2).l + 3, bt + kRowH + 13)).c_str());
        const Ink fileName = inkBox(f0, kF2x + 20, bt + 2 * kRowH + 3, kF2x + 400, bt + 2 * kRowH + 23, panelAlt);
        CHECKV(fileName.n > 40 && fileName.x0 >= kF2x + 23, "the file name stands in for the empty name: ink n=%d", fileName.n);
        CHECK(f0.countNear(kF2x + 23, bt + 2 * kRowH + 3, kF2x + 400, bt + 2 * kRowH + 23, fg, 40) >= 15);
        const int t3 = bt + 2 * kRowH + kNoteRowH;                                                        // row 3: after the 44 px row
        const std::vector<Span> c3 = clusterOnRow(f0, t3 + 22, kF2x + 300, kF2x + kF2w - 9, panelAlt);
        const Ink longNote = inkBox(f0, kF2x + 20, t3 + 23, spanAt(c3, 0).l, t3 + 41, panelAlt);
        CHECKV(longNote.x1 <= spanAt(c3, 0).l - 8 && longNote.x1 >= spanAt(c3, 0).l - 24, "the long note is cut at %d, the chip starts at %d", longNote.x1, spanAt(c3, 0).l);
        CHECK(f0.countInk(spanAt(c3, 0).l - 7, t3 + 23, spanAt(c3, 0).l, t3 + 41, panelAlt) == 0);
        CHECK_NEAR(ui->get_fm2_content(), 138.0, 1.01);                                                    // 25 + 25 + 44 + 44, no foot, no heading
        // nothing to scroll: no thumb
        CHECK(f0.countNear(kF2x + kF2w - 9, bt, kF2x + kF2w - 3, kF2y + kF2h, knobRing, 3) == 0);
    }

    // =========================================================================================================
    // 9. FM3: nothing fitted - the verdict, the empty text in muted, no rows, no rules, no foot
    // =========================================================================================================
    {
        CHECK_NEAR(ui->get_fm3_bar_height(), 94.0, 0.01);                                                 // 8 + 24 + 8 + 46 + 8: the chips take two lines
        const int bt = kF3y + static_cast<int>(ui->get_fm3_bar_height()) + static_cast<int>(ui->get_fm3_lines_height());
        const Ink empty = inkBox(f0, kF3x, bt, kF3x + kF3w, kF3y + kF3h, panelAlt);
        CHECKV(empty.n > 400 && empty.y0 >= bt + 11 && empty.x0 >= kF3x + 12 && empty.x0 <= kF3x + 14, "empty text ink n=%d from (%d, %d)", empty.n, empty.x0 - kF3x, empty.y0 - bt);
        CHECKV(empty.h() >= 40, "the empty text wraps: %d rows", empty.h());
        CHECK(f0.countNear(kF3x + 12, bt, kF3x + kF3w - 12, kF3y + kF3h, muted, 40) >= 150);
        CHECK(f0.countNear(kF3x + 2, bt, kF3x + kF3w - 2, kF3y + kF3h, line, 0) == 0);                    // no rules: there are no rows
        CHECK(ui->get_fm3_content() > 60 && ui->get_fm3_content() < 100);
        CHECK(f0.near(kF3x + 5, kF3y + 20, strip) && f0.near(kF3x + 5, kF3y + 90, strip));
        // the search box gave way to the keys of the narrow page: three runs on the first line - the box, then the two keys - with the 18 px
        // the page keeps between the box and the keys, the keys 12 px from the right edge, and the box narrower than its 220 px
        const int l1 = kF3y + 8 + 3;                                                                      // 3 px under the boxes' top: no text on it
        const std::vector<Span> r1 = runsOnRow(f0, l1, kF3x + 4, kF3x + kF3w, strip);
        CHECKV(r1.size() == 3, "%zu runs on the first line (wanted the search box and two keys)", r1.size());
        CHECKV(spanAt(r1, 0).l == kF3x + 12 && spanAt(r1, 0).w() >= 100 && spanAt(r1, 0).w() < 220, "the search box is %d wide from x %d", spanAt(r1, 0).w(), spanAt(r1, 0).l - kF3x);
        CHECKV(spanAt(r1, 1).l - spanAt(r1, 0).r == 18 && spanAt(r1, 2).l - spanAt(r1, 1).r == 10 && spanAt(r1, 2).r == kF3x + kF3w - 12, "the gaps are %d and %d, the keys end %d from the right", spanAt(r1, 1).l - spanAt(r1, 0).r, spanAt(r1, 2).l - spanAt(r1, 1).r, kF3x + kF3w - spanAt(r1, 2).r);
    }

    // =========================================================================================================
    // 10. FM1's interaction: row-clicked on the dot or name only, row-double-clicked anywhere on the row, the keys, the chips, the search box
    // =========================================================================================================
    {
        reset();
        const int t0 = rowTop(0);
        clk(kF1x + 12, t0 + 13);                                                                          // the dot
        CHECKV(ui->get_row_clicks() == 1 && ui->get_last_row() == 0 && sval(ui->get_last_source()) == "FM1", "dot: %d clicks, row %d, %s", ui->get_row_clicks(), ui->get_last_row(), sval(ui->get_last_source()).c_str());
        park();
        clk(kF1x + 60, rowTop(2) + 13);                                                                   // the name of row 2
        CHECK(ui->get_row_clicks() == 2 && ui->get_last_row() == 2);
        park();
        clk(kF1x + 650, rowTop(2) + 13);                                                                  // the empty part of the row: nothing
        CHECKV(ui->get_row_clicks() == 2, "a click on the empty part of the row opened a page (%d)", ui->get_row_clicks());
        park();
        clk(kF1x + 60, rowTop(4) + 30);                                                                   // the note line of a 44 px row, under the name: still the name's column
        CHECKV(ui->get_row_clicks() == 3 && ui->get_last_row() == 4, "the note line: %d clicks, row %d", ui->get_row_clicks(), ui->get_last_row());
        park();
        // a double click anywhere on the row but its keys: two clicks in a row
        reset();
        clk(kF1x + 650, rowTop(1) + 13);
        clk(kF1x + 650, rowTop(1) + 13);
        CHECKV(ui->get_row_doubles() == 1 && ui->get_last_double() == 1 && ui->get_row_clicks() == 0, "double click on the blank: %d doubles (row %d), %d clicks", ui->get_row_doubles(), ui->get_last_double(), ui->get_row_clicks());
        park();
        reset();
        clk(kF1x + 60, rowTop(1) + 13);
        clk(kF1x + 60, rowTop(1) + 13);                                                                   // on the name: the page asked twice, then the window
        CHECKV(ui->get_row_doubles() == 1 && ui->get_last_double() == 1 && ui->get_row_clicks() == 2, "double click on the name: %d doubles, %d clicks", ui->get_row_doubles(), ui->get_row_clicks());
        park();
        // the keys: STOP of row 0 (the middle element), REMOVE of row 0, START of row 1; the keys are not the row's
        reset();
        const std::vector<Span> c0 = clusterOnRow(f0, rowMid(0), kF1x + 600, kF1x + kF1w - 9, panelAlt);
        const std::vector<Span> c1 = clusterOnRow(f0, rowMid(1), kF1x + 600, kF1x + kF1w - 9, panelAlt);
        const std::vector<Span> c3 = clusterOnRow(f0, rowMid(3), kF1x + 600, kF1x + kF1w - 9, panelAlt);
        const std::vector<Span> c5 = clusterOnRow(f0, rowMid(5), kF1x + 600, kF1x + kF1w - 9, panelAlt);
        clk(spanAt(c0, 1).mid(), rowMid(0));
        CHECKV(ui->get_runs() == 1 && ui->get_last_run() == 0 && ui->get_removes() == 0 && ui->get_row_clicks() == 0, "STOP: %d runs (row %d), %d removes, %d row clicks", ui->get_runs(), ui->get_last_run(), ui->get_removes(), ui->get_row_clicks());
        clk(spanAt(c0, 2).mid(), rowMid(0));
        CHECKV(ui->get_removes() == 1 && ui->get_last_remove() == 0 && ui->get_runs() == 1 && ui->get_row_clicks() == 0, "REMOVE: %d removes (row %d)", ui->get_removes(), ui->get_last_remove());
        clk(spanAt(c1, 1).mid(), rowMid(1));
        CHECK(ui->get_runs() == 2 && ui->get_last_run() == 1);                                            // START is the same callback
        clk(spanAt(c3, 1).mid(), rowMid(3));
        CHECKV(ui->get_removes() == 2 && ui->get_last_remove() == 3, "REMOVE FILE: row %d", ui->get_last_remove());
        clk(spanAt(c3, 0).mid(), rowMid(3));                                                              // the state chip is only a word: no key
        CHECK(ui->get_runs() == 2 && ui->get_removes() == 2 && ui->get_row_clicks() == 0);
        clk(spanAt(c5, 2).mid(), rowMid(5));                                                              // CONFIRM of row 5 (armed) is the remove callback too
        CHECKV(ui->get_removes() == 3 && ui->get_last_remove() == 5, "CONFIRM: row %d", ui->get_last_remove());
        park();
        // the host arms row 0: its REMOVE reads CONFIRM and goes red; the key is as wide as the word needs
        std::vector<FittedRowModel> rows;
        const auto base = ui->get_rows_fm1();
        for (std::size_t i = 0; i < base->row_count(); ++i) { rows.push_back(*base->row_data(i)); }
        const int plainRemove = spanAt(c0, 2).w();
        rows[0].armed = true;
        ui->set_rows_fm1(std::make_shared<slint::VectorModel<FittedRowModel>>(rows));
        const Frame armed = shot();
        const std::vector<Span> ca = clusterOnRow(armed, rowMid(0), kF1x + 600, kF1x + kF1w - 9, panelAlt);
        CHECKV(ca.size() == 3 && armed.near(spanAt(ca, 2).l + 3, rowMid(0), dangerBg) && armed.near(spanAt(ca, 2).l, rowMid(0), dangerLine) && armed.countNear(spanAt(ca, 2).l + 2, rowTop(0) + 6, spanAt(ca, 2).r - 2, rowTop(0) + 20, dangerFg, 40) >= 8, "armed REMOVE: %zu elements, ground %s", ca.size(), hex(armed.at(spanAt(ca, 2).l + 3, rowMid(0))).c_str());
        CHECKV(spanAt(ca, 2).w() >= plainRemove + 3, "CONFIRM is %d wide, REMOVE was %d: the word changed with the state", spanAt(ca, 2).w(), plainRemove);
        rows[0].armed = false;
        ui->set_rows_fm1(std::make_shared<slint::VectorModel<FittedRowModel>>(rows));
        // a key with a tooltip: REMOVE explains what it keeps (the ImGui sentence), 6 px under the 20 px key
        reset();
        mv(spanAt(c0, 2).mid(), rowMid(0));
        settle(milliseconds(750));
        const Frame tip = shot();
        const int keyBot = rowTop(0) + 3 + 20;
        // (the key is at the right edge of the page: the popup is moved left so that it stays inside the window, and still covers the key's x)
        CHECKV(tip.near(spanAt(c0, 2).l + 12, keyBot + 6, knobRing, 3) && tip.near(spanAt(c0, 2).l - 200, keyBot + 6, knobRing, 3) && tip.near(spanAt(c0, 2).l + 8, keyBot + 6 + 8, menu), "REMOVE tooltip border: %s / ground %s", hex(tip.at(spanAt(c0, 2).l + 12, keyBot + 6)).c_str(), hex(tip.at(spanAt(c0, 2).l + 8, keyBot + 6 + 8)).c_str());

        park();
    }

    // =========================================================================================================
    // 11. FM1: chips, keys and the search box
    // =========================================================================================================
    {
        reset();
        clk(spanAt(chips1, 2).mid(), fm1Mid);
        CHECKV(ui->get_chip_toggles() == 1 && ui->get_last_chip() == 2 && sval(ui->get_last_source()) == "FM1", "chip 2: %d toggles, index %d", ui->get_chip_toggles(), ui->get_last_chip());
        clk(spanAt(chips1, 5).mid(), fm1Mid);
        CHECK(ui->get_last_chip() == 5 && ui->get_chip_toggles() == 2);
        clk(spanAt(keys1, 0).mid(), fm1Mid);
        CHECKV(ui->get_scans() == 1 && ui->get_resets() == 0, "SCAN AGAIN: %d scans, %d resets", ui->get_scans(), ui->get_resets());
        clk(spanAt(keys1, 1).mid(), fm1Mid);
        CHECKV(ui->get_resets() == 1 && ui->get_scans() == 1, "RESET WINDOW SIZES: %d resets", ui->get_resets());
        // the search box: typing says search-changed with the whole text each time, and the in-out `search` follows
        const int sx = kF1x + 12, sy = kF1y + 8;
        clk(sx + 120, sy + 12);
        tap(u8"a");
        tap(u8"d");
        tap(u8"s");
        CHECKV(ui->get_search_edits() == 3 && sval(ui->get_last_search()) == "ads" && sval(ui->get_search_fm1()) == "ads", "typed ads: %d edits, last '%s', search '%s'", ui->get_search_edits(), sval(ui->get_last_search()).c_str(), sval(ui->get_search_fm1()).c_str());
        const Frame typed = shot();
        CHECK(typed.near(sx - 2, sy + 12, focus, 14));                                                    // the focus ring, 1 px clear
        CHECKV(typed.countInk(sx + 24, sy + 2, sx + 212, sy + 22, field) >= 12 && typed.countNear(sx + 24, sy + 2, sx + 212, sy + 22, fg, 40) >= 8, "the typed text is drawn in fg");
        const Ink ph0 = inkBox(f0, sx + 24, sy + 2, sx + 212, sy + 22, field), ph1 = inkBox(typed, sx + 24, sy + 2, sx + 212, sy + 22, field);
        CHECKV(ph1.w() < 40 && ph0.w() > 100 && ph1.x0 >= sx + 28 && ph1.x0 <= sx + 31, "the placeholder gave way: %d px of text where there were %d", ph1.w(), ph0.w());
        tap(slint::platform::key_codes::Backspace);
        CHECK(ui->get_search_edits() == 4 && sval(ui->get_last_search()) == "ad");
        ui->set_search_fm1("");                                                                           // the host clears it
        const Frame cleared = shot();
        CHECKV(cleared.countNear(sx + 24, sy + 2, sx + 212, sy + 22, disabledInk, 40) >= 12, "cleared: the placeholder is back");
        // clicking a row takes nothing from the box: the key presses leave the focus where it was
        clk(spanAt(keys1, 0).mid(), fm1Mid);
        tap(u8"x");
        CHECKV(sval(ui->get_search_fm1()) == "x", "typing after pressing a key: '%s'", sval(ui->get_search_fm1()).c_str());
        ui->set_search_fm1("");
        park();
        // the foot's key is in the scrolled-off part of the body: scroll to it and press it
        wheel(600, 250, -1000.0f);
        const int footTop = bodyTop1 + kBodyH1 - 64;
        clk(kF1x + 12 + 60, footTop + 24);
        CHECKV(ui->get_cleanups() == 1, "CLEAN UP OLD VERSIONS: %d", ui->get_cleanups());
        // a row's key works while the body is scrolled: STOP of the last row (row 11, START) at its scrolled place
        const int t11 = bodyTop1 + kBodyH1 - 64 - kRowH;
        const Frame sc = shot();
        const std::vector<Span> c11 = clusterOnRow(sc, t11 + 13, kF1x + 600, kF1x + kF1w - 9, panelAlt);
        CHECKV(c11.size() == 3, "row 11 at the bottom: %zu elements", c11.size());
        clk(spanAt(c11, 1).mid(), t11 + 13);
        CHECKV(ui->get_runs() == 1 && ui->get_last_run() == 11, "scrolled: START of row %d", ui->get_last_run());
        wheel(600, 250, 2000.0f);
        park();
    }

    // =========================================================================================================
    // 12. FM1 and FM2: hover tints a row; a cut note shows in full as a tooltip; the second page's callbacks carry its own source
    // =========================================================================================================
    {
        mv(kF1x + 650, rowTop(1) + 13);
        const Frame h = shot();
        CHECKV(h.near(kF1x + 400, rowTop(1) + 10, panel) && h.near(kF1x + 400, rowTop(0) + 10, panelAlt), "hovered row %s, the one above %s", hex(h.at(kF1x + 400, rowTop(1) + 10)).c_str(), hex(h.at(kF1x + 400, rowTop(0) + 10)).c_str());
        CHECK(h.rowNear(rowTop(1), kF1x + 2, kF1x + 100, line));                                          // the rule stays the rule over the tint
        park();
        // FM2's fourth row: its note is cut, so hovering the row shows the whole sentence 6 px under the row's top... and the third has none
        const int bt = kF2y + 68 + static_cast<int>(ui->get_fm2_lines_height());
        const int t3 = bt + 2 * kRowH + kNoteRowH;
        mv(kF2x + 300, t3 + 10);
        settle(milliseconds(750));
        const Frame tip = shot();
        CHECKV(tip.near(kF2x + 12, t3 + kNoteRowH + 6, knobRing, 3) && tip.near(kF2x + 5, t3 + kNoteRowH + 6 + 8, menu), "cut-note tooltip: %s / %s", hex(tip.at(kF2x + 12, t3 + kNoteRowH + 6)).c_str(), hex(tip.at(kF2x + 5, t3 + kNoteRowH + 6 + 8)).c_str());
        park();
        mv(kF2x + 300, bt + 2 * kRowH + 10);                                                              // the 44 px row above it: its note fits
        settle(milliseconds(750));
        CHECKV(!shot().near(kF2x + 12, bt + 2 * kRowH + kNoteRowH + 6, knobRing, 3), "a note that fits opened a tooltip");
        park();
        reset();
        clk(spanAt(runsOnRow(f0, kF2y + 8 + 24 + 8 + 10, kF2x, kF2x + kF2w, strip), 3).mid(), kF2y + 8 + 24 + 8 + 10);                       // FM2's fourth chip, on the second line
        CHECKV(ui->get_chip_toggles() == 1 && ui->get_last_chip() == 3 && sval(ui->get_last_source()) == "FM2", "FM2 chip: %d toggles, index %d, source %s", ui->get_chip_toggles(), ui->get_last_chip(), sval(ui->get_last_source()).c_str());
        clk(kF2x + 60, bt + 13);
        CHECK(ui->get_row_clicks() == 1 && ui->get_last_row() == 0 && sval(ui->get_last_source()) == "FM2");
        const std::vector<Span> c1 = clusterOnRow(f0, bt + kRowH + 13, kF2x + 300, kF2x + kF2w - 9, panelAlt);
        clk(spanAt(c1, 2).mid(), bt + kRowH + 13);                                                          // CONFIRM of FM2's second row
        CHECK(ui->get_removes() == 1 && ui->get_last_remove() == 1 && sval(ui->get_last_source()) == "FM2");
        park();
    }

    // =========================================================================================================
    // 13. A disabled page is dead: the keys, the chips, the search box and the rows do nothing, in the kit's disabled look
    // =========================================================================================================
    {
        ui->set_fm1_enabled(false);
        const Frame d = shot();
        reset();
        const int sx = kF1x + 12, sy = kF1y + 8;
        clk(spanAt(keys1, 0).mid(), fm1Mid);
        clk(spanAt(keys1, 1).mid(), fm1Mid);
        clk(spanAt(chips1, 0).mid(), fm1Mid);
        clk(kF1x + 60, rowTop(0) + 13);
        clk(kF1x + 12, rowTop(0) + 13);
        const std::vector<Span> c0 = clusterOnRow(f0, rowMid(0), kF1x + 600, kF1x + kF1w - 9, panelAlt);
        clk(spanAt(c0, 1).mid(), rowMid(0));
        clk(spanAt(c0, 2).mid(), rowMid(0));
        clk(sx + 120, sy + 12);
        tap(u8"q");
        CHECKV(ui->get_scans() == 0 && ui->get_resets() == 0 && ui->get_chip_toggles() == 0 && ui->get_row_clicks() == 0 && ui->get_runs() == 0 && ui->get_removes() == 0 && ui->get_search_edits() == 0,
               "disabled page fired: %d scans, %d resets, %d chips, %d rows, %d runs, %d removes, %d edits", ui->get_scans(), ui->get_resets(), ui->get_chip_toggles(), ui->get_row_clicks(), ui->get_runs(), ui->get_removes(), ui->get_search_edits());
        CHECKV(d.near(spanAt(keys1, 0).l + 3, fm1Mid, keyDisBg) && d.near(spanAt(keys1, 1).l + 3, fm1Mid, keyDisBg), "disabled key ground %s", hex(d.at(spanAt(keys1, 0).l + 3, fm1Mid)).c_str());
        CHECK(d.countNear(spanAt(keys1, 0).l + 2, kF1y + 10, spanAt(keys1, 0).r - 2, kF1y + 30, keyDisFg, 24) >= 12);
        CHECKV(d.near(sx + 4, sy + 12, keyDisBg), "disabled search ground %s", hex(d.at(sx + 4, sy + 12)).c_str());
        CHECKV(d.near(spanAt(chips1, 0).l + 3, fm1Mid, keyDisBg, 5), "disabled chip ground %s", hex(d.at(spanAt(chips1, 0).l + 3, fm1Mid)).c_str());
        CHECK(d.near(spanAt(c0, 1).l + 3, rowMid(0), keyDisBg));                                          // the row's keys too
        CHECK(d.countNear(kF1x + 23, rowTop(0) + 3, kF1x + 400, rowTop(0) + 22, fg, 12) == 0);            // the name is no longer fg
        ui->set_fm1_enabled(true);
        park();
    }

    // =========================================================================================================
    // 14. MP1, the module page: back key, lamp and STOP at the top, the name and the maker line, the grants, the plate keys
    // =========================================================================================================
    const int m1Pad = 16;
    {
        const int top = kM1y + 12, mid = top + 12;
        CHECK(f0.near(kM1x + 3, kM1y + 3, panelAlt));
        // the back key: small, key-bg, 16 px in
        CHECKV(f0.near(kM1x + m1Pad, mid, keyLine) && f0.near(kM1x + m1Pad + 3, mid, keyBg) && f0.near(kM1x + m1Pad + 8, top, keyLine) && f0.near(kM1x + m1Pad + 8, top + 23, keyLine), "back key at x %d: %s / %s", kM1x + m1Pad, hex(f0.at(kM1x + m1Pad, mid)).c_str(), hex(f0.at(kM1x + m1Pad + 3, mid)).c_str());
        const std::vector<Span> hdr = runsOnRow(f0, mid, kM1x + m1Pad, kM1x + kMw - 12, panelAlt);
        CHECKV(hdr.size() >= 3, "%zu runs on the header line", hdr.size());
        const Span back = spanAt(hdr, 0), stop = spanAt(hdr, hdr.size() - 1);
        CHECKV(back.w() >= 60 && back.w() <= 110, "back key is %d wide", back.w());
        CHECK(f0.countNear(back.l + 2, top + 3, back.r - 2, top + 21, keyFg, 40) >= 20);
        // STOP: danger, small, ending 16 px from the right edge
        CHECKV(stop.r == kM1x + kMw - m1Pad && f0.near(stop.l, mid, dangerLine) && f0.near(stop.l + 3, mid, dangerBg) && f0.near(stop.l + 8, top, dangerLine) && f0.near(stop.l + 8, top + 23, dangerLine), "STOP key %d..%d: %s", stop.l, stop.r, hex(f0.at(stop.l + 3, mid)).c_str());
        CHECK(f0.countNear(stop.l + 2, top + 3, stop.r - 2, top + 21, dangerFg, 40) >= 12);
        // the lamp before it: a green dot and the caption in lamp-green-cap, 8 px from the key
        const Ink lamp = inkBox(f0, back.r + 20, top + 2, stop.l - 2, top + 22, panelAlt);
        CHECKV(lamp.n > 40 && lamp.x1 <= stop.l - 8 && lamp.x1 >= stop.l - 12, "lamp ink n=%d ends %d px before STOP", lamp.n, stop.l - lamp.x1);
        CHECK(f0.near(lamp.x0 + 4, mid, lampGreen, 8));
        CHECK(f0.countNear(lamp.x0 + 12, top + 2, lamp.x1 + 1, top + 22, lampGreenCap, 40) >= 10);
        // the name: 13 px / 600 in fg at the padding; the maker line after it, dim, 8 px away, 11 px
        const int ny = top + 24 + 8;
        const Ink nameAll = inkBox(f0, kM1x + m1Pad - 2, ny, kM1x + kMw - 20, ny + 20, panelAlt);
        CHECKV(nameAll.n > 200 && nameAll.x0 >= kM1x + m1Pad && nameAll.x0 <= kM1x + m1Pad + 2, "name and maker ink n=%d from x %d", nameAll.n, nameAll.x0 - kM1x);
        // the name is the fg ink, 13 px / 600; the maker line the dim ink after it
        int nameEnd = -1, makerStart = -1;
        for (int x = kM1x + m1Pad; x < kM1x + kMw - 20; ++x) {
            if (f0.countNear(x, ny, x + 1, ny + 20, fg, 30) > 0) { nameEnd = x; }
        }
        for (int x = nameEnd + 1; x < kM1x + kMw - 20 && makerStart < 0; ++x) {
            if (f0.countNear(x, ny, x + 1, ny + 20, dim, 12) > 0) { makerStart = x; }
        }
        CHECKV(nameEnd > kM1x + m1Pad + 150 && makerStart >= nameEnd + 6 && makerStart <= nameEnd + 14, "the name ends at %d and the maker line starts at %d (8 px apart)", nameEnd - kM1x, makerStart - kM1x);
        CHECK(f0.countNear(makerStart, ny, kM1x + kMw - 20, ny + 20, dim, 40) >= 40);
        // the grant keys under it: GRANT RECEIVER CONTROL (plate), REVOKE RADIO SETTINGS (lit), GRANT TRANSMIT (plate)
        const int g = ny + 18 + 8;
        CHECKV(f0.near(kM1x + m1Pad, g + 12, plateLine) && f0.near(kM1x + m1Pad + 8, g, plateLine) && f0.near(kM1x + m1Pad + 8, g + 23, plateLine) && f0.near(kM1x + m1Pad + 4, g + 12, panelAlt), "GRANT RECEIVER CONTROL at row %d: %s", g, hex(f0.at(kM1x + m1Pad, g + 12)).c_str());
        CHECK(f0.near(kM1x + m1Pad, g + 30 + 12, accent) && f0.near(kM1x + m1Pad + 4, g + 30 + 12, accent));
        CHECK(f0.countNear(kM1x + m1Pad + 8, g + 30 + 3, kM1x + m1Pad + 120, g + 30 + 21, accentInk, 40) >= 25);
        // the plate keys: 6 labels, two to a row, cells (580 - 32 - 6) / 2 = 271 wide
        const int plate = f0.firstRowNear(kM1x + m1Pad + 8, g + 140, g + 260, plateLine);
        CHECKV(plate > g + 150 && plate < g + 250, "first command key row at %d (the grants start at %d)", plate, g);
        const int cell = 271;
        for (int i = 0; i < 6; ++i) {
            const int x = kM1x + m1Pad + (i % 2) * (cell + 6), y = plate + (i / 2) * 30;
            CHECKV(f0.near(x, y + 12, plateLine) && f0.near(x + cell - 1, y + 12, plateLine) && f0.near(x + 20, y, plateLine) && f0.near(x + 20, y + 23, plateLine), "command key %d at (%d, %d)", i, x, y);
            CHECK(f0.countNear(x + 8, y + 3, x + cell - 8, y + 21, plateFg, 40) >= 12);
        }
        CHECK(f0.near(kM1x + m1Pad + 20, plate + 24, panelAlt) && f0.near(kM1x + m1Pad + 20, plate + 29, panelAlt));       // 6 px between the rows
        // the page scrolls: 826 px of content in 600
        CHECK_NEAR(ui->get_mp1_content(), 826.0, 1.5);
        const int thumbX = kM1x + kMw - 9;
        CHECK(f0.colNear(thumbX + 3, kM1y + 4, kM1y + 430, knobRing));
        CHECK(f0.near(thumbX + 3, kM1y + 440, panelAlt));
    }

    // =========================================================================================================
    // 15. MP1 interaction: back, STOP, the grant keys (the new state), the command keys (their index); then scrolled: the tick and the foot
    // =========================================================================================================
    {
        reset();
        const int top = kM1y + 12, mid = top + 12;
        const std::vector<Span> hdr = runsOnRow(f0, mid, kM1x + m1Pad, kM1x + kMw - 12, panelAlt);
        clk(spanAt(hdr, 0).mid(), mid);
        CHECKV(ui->get_page_events() == 1 && sval(ui->get_last_page_event()) == "back" && sval(ui->get_last_source()) == "MP1", "back: %d events, '%s'", ui->get_page_events(), sval(ui->get_last_page_event()).c_str());
        clk(spanAt(hdr, hdr.size() - 1).mid(), mid);
        CHECKV(ui->get_page_events() == 2 && sval(ui->get_last_page_event()) == "run", "STOP: '%s'", sval(ui->get_last_page_event()).c_str());
        const int ny = top + 24 + 8, g = ny + 18 + 8;
        clk(kM1x + m1Pad + 50, g + 12);                                                                   // GRANT RECEIVER CONTROL
        CHECKV(ui->get_page_events() == 3 && sval(ui->get_last_page_event()) == "tune" && ui->get_last_page_value(), "tune: '%s' %d", sval(ui->get_last_page_event()).c_str(), ui->get_last_page_value());
        park();
        CHECK(shot().near(kM1x + m1Pad + 4, g + 12, accent));                                              // lit now: REVOKE
        clk(kM1x + m1Pad + 50, g + 12);
        CHECK(ui->get_page_events() == 4 && !ui->get_last_page_value());
        clk(kM1x + m1Pad + 50, g + 30 + 12);                                                              // REVOKE RADIO SETTINGS
        CHECKV(ui->get_page_events() == 5 && sval(ui->get_last_page_event()) == "settings" && !ui->get_last_page_value(), "settings: '%s' %d", sval(ui->get_last_page_event()).c_str(), ui->get_last_page_value());
        const int transmitKey = f0.firstRowNear(kM1x + m1Pad + 8, g + 60, g + 140, plateLine);
        CHECKV(transmitKey > g + 80 && transmitKey < g + 110, "GRANT TRANSMIT's top border at %d (the grants start at %d)", transmitKey, g);
        clk(kM1x + m1Pad + 50, transmitKey + 12);                                                         // GRANT TRANSMIT
        CHECKV(ui->get_page_events() == 6 && sval(ui->get_last_page_event()) == "transmit" && ui->get_last_page_value() && sval(ui->get_last_source()) == "MP1", "transmit: '%s' %d", sval(ui->get_last_page_event()).c_str(), ui->get_last_page_value());
        const int plate = f0.firstRowNear(kM1x + m1Pad + 8, g + 140, g + 260, plateLine);
        clk(kM1x + m1Pad + 271 + 6 + 100, plate + 30 + 12);                                               // the second row's right key: index 3
        CHECKV(ui->get_last_command() == 3 && sval(ui->get_last_page_event()) == "command" && sval(ui->get_last_source()) == "MP1", "command key: index %d", ui->get_last_command());
        clk(kM1x + m1Pad + 100, plate + 60 + 12);                                                         // the third row's left key: index 4
        CHECK(ui->get_last_command() == 4);
        const int events = ui->get_page_events();
        clk(kM1x + m1Pad + 271 + 3, plate + 12);                                                          // the 6 px between two command keys
        CHECK(ui->get_page_events() == events);
        park();
        // START instead of STOP when the module is stopped or idle: the key is a plain one and the callback is the same
        ui->set_mp1_offers_start(true);
        const Frame st = shot();
        const std::vector<Span> hdr2 = runsOnRow(st, mid, kM1x + m1Pad, kM1x + kMw - 12, panelAlt);
        const Span startKey = spanAt(hdr2, hdr2.size() - 1);
        CHECKV(startKey.r == kM1x + kMw - m1Pad && st.near(startKey.l, mid, keyLine) && st.near(startKey.l + 3, mid, keyBg) && st.countNear(startKey.l + 2, top + 3, startKey.r - 2, top + 21, dangerFg, 12) == 0, "START key %d..%d: %s", startKey.l, startKey.r, hex(st.at(startKey.l + 3, mid)).c_str());
        CHECKV(startKey.w() > spanAt(hdr, hdr.size() - 1).w(), "START (%d px) is wider than STOP (%d px)", startKey.w(), spanAt(hdr, hdr.size() - 1).w());
        reset();
        clk(startKey.mid(), mid);
        CHECK(ui->get_page_events() == 1 && sval(ui->get_last_page_event()) == "run");
        ui->set_mp1_offers_start(false);
        // a disabled page: the kit's disabled look, and no key acts
        ui->set_mp1_enabled(false);
        const Frame dis = shot();
        reset();
        clk(spanAt(hdr, 0).mid(), mid);
        clk(spanAt(hdr, hdr.size() - 1).mid(), mid);
        clk(kM1x + m1Pad + 50, g + 12);
        clk(kM1x + m1Pad + 50, transmitKey + 12);
        clk(kM1x + m1Pad + 100, plate + 12);
        CHECKV(ui->get_page_events() == 0, "a disabled page fired %d events (%s)", ui->get_page_events(), sval(ui->get_last_page_event()).c_str());
        CHECKV(dis.near(spanAt(hdr, 0).l + 3, mid, keyDisBg) && dis.near(kM1x + m1Pad + 4, g + 12, keyDisBg) && dis.near(kM1x + m1Pad + 20, plate + 12, keyDisBg), "disabled grounds %s / %s / %s", hex(dis.at(spanAt(hdr, 0).l + 3, mid)).c_str(), hex(dis.at(kM1x + m1Pad + 4, g + 12)).c_str(), hex(dis.at(kM1x + m1Pad + 20, plate + 12)).c_str());
        CHECK(dis.countNear(spanAt(hdr, 0).l + 2, top + 3, spanAt(hdr, 0).r - 2, top + 21, keyDisFg, 24) >= 12);
        ui->set_mp1_enabled(true);
        park();
        // scroll to the end: the tick, the notes, the facts, the catalogue place
        wheel(300, kM1y + 300, -1000.0f);
        const double end = ui->get_mp1_scrolled();
        CHECK_NEAR(end, ui->get_mp1_content() - kMh, 0.6);
        const Frame e = shot();
        // the catalogue block: well ground, a line border, 24 px above the content's end
        const int endY = kM1y + kMh - 24;
        CHECKV(e.near(kM1x + m1Pad + 4, endY - 4, well) && e.near(kM1x + m1Pad, endY - 20, line) && e.near(kM1x + m1Pad + 4, endY + 3, panelAlt), "catalogue block: %s / %s / %s", hex(e.at(kM1x + m1Pad + 4, endY - 4)).c_str(), hex(e.at(kM1x + m1Pad, endY - 20)).c_str(), hex(e.at(kM1x + m1Pad + 4, endY + 3)).c_str());
        CHECK(e.countNear(kM1x + m1Pad + 10, endY - 40, kM1x + kMw - m1Pad - 10, endY - 2, dim, 40) >= 60);
        // the KEEP RUNNING tick: found by its accent box in the left column below the grants
        int tick = -1;
        for (int y = kM1y + 2; y < kM1y + kMh - 30; ++y) { if (e.near(kM1x + m1Pad + 3, y, accent, 6) && e.near(kM1x + m1Pad + 3, y - 1, panelAlt, 6)) { tick = y; } }
        CHECKV(tick > 0 && e.near(kM1x + m1Pad, tick + 7, accent) && e.near(kM1x + m1Pad + 13, tick + 7, accent), "the Keep running box at row %d", tick);
        const Ink keep = inkBox(e, kM1x + m1Pad + 16, tick - 2, kM1x + 300, tick + 16, panelAlt);
        CHECKV(keep.n > 40 && keep.x0 >= kM1x + m1Pad + 14 + 6 && keep.x0 <= kM1x + m1Pad + 14 + 10, "Keep running label starts %d px after the box", keep.x0 - (kM1x + m1Pad + 14));
        // the facts: FILE and PLUGIN ABI with their caps at the left and the values 150 px in; the notice line in accent-soft above them
        CHECK(e.countNear(kM1x + m1Pad, kM1y + kMh - 24 - 47 - 8 - 60, kM1x + m1Pad + 140, kM1y + kMh - 24 - 47 - 8, muted, 40) >= 15);
        CHECK(e.countNear(kM1x + m1Pad, tick + 20, kM1x + kMw - m1Pad, tick + 80, accentSoft, 40) >= 40);
        // the tick toggles and says its new state; the page has scrolled, the click lands where the box is drawn
        reset();
        clk(kM1x + m1Pad + 7, tick + 7);
        CHECKV(ui->get_page_events() == 1 && sval(ui->get_last_page_event()) == "keep" && !ui->get_last_page_value(), "keep: %d events, '%s', %d", ui->get_page_events(), sval(ui->get_last_page_event()).c_str(), ui->get_last_page_value());
        park();
        const Frame k = shot();
        CHECK(k.near(kM1x + m1Pad + 3, tick + 3, field));                                                 // unticked
        clk(kM1x + m1Pad + 120, tick + 7);
        CHECK(ui->get_page_events() == 2 && ui->get_last_page_value());
        park();
        wheel(300, kM1y + 300, 2000.0f);
        CHECK_NEAR(ui->get_mp1_scrolled(), 0.0, 0.01);
    }

    // =========================================================================================================
    // 16. The reach list (MP3 shows it alone): a gold dot with a halo for what reaches outward, a hollow ring for what only publishes
    // =========================================================================================================
    {
        CHECK_NEAR(ui->get_mp3_scrolled(), 0.0, 0.01);
        CHECK(ui->get_mp3_content() < 240);                                                               // it fits: no scrolling, no thumb
        const int dx = kM3x + 16 + 6;                                                                    // the column through the middle of the dots
        // the cap: 24 px under the name row (the back key 24, the name 18, 8 px between), its text 10 px into its 24 px box
        const int cap = kM3y + 12 + 24 + 8 + 18 + 8 + 10;
        const Ink capInk = inkBox(f0, kM3x + 16, cap - 2, kM3x + 400, cap + 14, panelAlt);
        CHECKV(capInk.n > 100 && capInk.x0 >= kM3x + 16 && capInk.x0 <= kM3x + 17 && capInk.h() <= 9 && capInk.y0 >= cap, "the cap ink n=%d from (%d, %d), %d rows tall", capInk.n, capInk.x0 - kM3x, capInk.y0 - cap, capInk.h());
        CHECK(f0.countNear(kM3x + 16, cap - 2, kM3x + 400, cap + 14, muted, 40) >= 30);
        // the rows: segments of non-ground pixels down the dots column, each a ring (faint) or a gold dot on a halo
        struct Seg { int y0, y1; bool gold, ring; };
        std::vector<Seg> segs;
        for (int y = cap + 20; y < kM3y + 230; ++y) {
            if (f0.near(dx, y, panelAlt, 3)) { continue; }
            if (segs.empty() || y - segs.back().y1 > 6) { segs.push_back({y, y, false, false}); }
            segs.back().y1 = y;
            if (f0.near(dx, y, accentSoft, 12)) { segs.back().gold = true; }
            if (f0.near(dx, y, faint, 24)) { segs.back().ring = true; }
        }
        CHECKV(segs.size() == 3, "%zu markers down the dots column (wanted 3)", segs.size());
        const auto seg = [&](std::size_t i) { return i < segs.size() ? segs[i] : Seg{-1000, -1000, false, false}; };
        CHECKV(seg(0).ring && !seg(0).gold && !seg(1).ring && seg(1).gold && seg(2).ring && !seg(2).gold, "ring %d / gold %d, ring %d / gold %d, ring %d / gold %d", seg(0).ring, seg(0).gold, seg(1).ring, seg(1).gold, seg(2).ring, seg(2).gold);
        CHECKV(seg(1).y1 - seg(1).y0 + 1 == 10, "the halo is 10 px tall: %d", seg(1).y1 - seg(1).y0 + 1);
        CHECKV(seg(0).y1 - seg(0).y0 + 1 == 8 && seg(2).y1 - seg(2).y0 + 1 == 8, "the rings are 8 px tall: %d, %d", seg(0).y1 - seg(0).y0 + 1, seg(2).y1 - seg(2).y0 + 1);
        // the halo is accent-soft at 28 %, the core 6 px of accent-soft
        CHECKV(f0.near(dx, seg(1).y0 + 1, over(accentSoft, 0.28, panelAlt), 6) && f0.near(dx, seg(1).y0 + 8, over(accentSoft, 0.28, panelAlt), 6), "the halo %s / %s, wanted about %s", hex(f0.at(dx, seg(1).y0 + 1)).c_str(), hex(f0.at(dx, seg(1).y0 + 8)).c_str(), hex(over(accentSoft, 0.28, panelAlt)).c_str());
        CHECK(f0.near(dx, seg(1).y0 + 4, accentSoft, 6) && f0.near(kM3x + 16 + 4, seg(1).y0 + 5, accentSoft, 6) && f0.near(kM3x + 16 + 7, seg(1).y0 + 5, accentSoft, 6));
        // the rows are pitched by their own heights: 16 + 2 + one line of detail, 8 px between
        const int pitch1 = seg(1).y0 - seg(0).y0, pitch2 = seg(2).y0 - seg(1).y0;
        CHECKV(pitch1 >= 38 && pitch1 <= 42 && pitch2 >= 38 && pitch2 <= 42, "marker pitch %d and %d (a row of 16 + 2 + 13 + the 8 px between)", pitch1, pitch2);
        // the keys: the outward one in fg, the others in fg-soft; the details under them in muted
        const auto keyInk = [&](const Seg& g, Rgb want, int tol) { return f0.countNear(kM3x + 16 + 20, g.y0 - 4, kM3x + 300, g.y0 + 12, want, tol); };
        CHECKV(keyInk(seg(1), fg, 40) >= 15 && keyInk(seg(0), fgSoft, 12) >= 8 && keyInk(seg(2), fgSoft, 12) >= 8, "key ink: %d fg / %d, %d fg-soft", keyInk(seg(1), fg, 40), keyInk(seg(0), fgSoft, 12), keyInk(seg(2), fgSoft, 12));
        CHECK(f0.countNear(kM3x + 16 + 20, seg(0).y0 + 14, kM3x + 400, seg(0).y0 + 30, muted, 40) >= 12);
        const Ink detail = inkBox(f0, kM3x + 16 + 14, seg(0).y0 + 14, kM3x + 500, seg(0).y0 + 31, panelAlt);
        CHECKV(detail.n > 40 && detail.x0 >= kM3x + 16 + 20 && detail.x0 <= kM3x + 16 + 22, "the detail starts %d px in (the 20 px of the marker column)", detail.x0 - kM3x - 16);
        CHECK(f0.countNear(kM3x + 580 - 9, kM3y, kM3x + 580 - 3, kM3y + 240, knobRing, 3) == 0);
    }

    // =========================================================================================================
    // 17. MP2, a refused module: no STOP, the reason under its red cap, RE-ENABLE with the fault as its tip, the notes, FILE and PLUGIN ABI
    // =========================================================================================================
    {
        CHECK_NEAR(ui->get_mp2_scrolled(), 0.0, 0.01);
        CHECK(ui->get_mp2_content() < kMh);                                                               // it fits: no thumb
        CHECK(f0.countNear(kM2x + kMw - 9, kM1y, kM2x + kMw - 3, kM1y + kMh, knobRing, 3) == 0);
        const int top = kM1y + 12, mid = top + 12;
        const std::vector<Span> hdr = runsOnRow(f0, mid, kM2x + 16, kM2x + kMw - 12, panelAlt);
        CHECK(spanAt(hdr, 0).l == kM2x + 16 && f0.near(spanAt(hdr, 0).l, mid, keyLine));                 // the back key
        // the lamp is red and says REFUSED; there is no STOP / START key: nothing at the right edge but the lamp caption
        const Ink lamp = inkBox(f0, spanAt(hdr, 0).r + 20, top + 2, kM2x + kMw - 12, top + 22, panelAlt);
        CHECKV(lamp.n > 40 && lamp.x1 >= kM2x + kMw - 16 - 2 && lamp.x1 <= kM2x + kMw - 16, "lamp ink n=%d ends at %d (16 px in from the right: %d)", lamp.n, lamp.x1, kM2x + kMw - 16);
        CHECK(f0.near(lamp.x0 + 4, mid, lampRed, 8));
        CHECK(f0.countNear(lamp.x0 + 12, top + 2, lamp.x1 + 1, top + 22, lampRedCap, 40) >= 12);
        CHECK(f0.countNear(spanAt(hdr, 0).r + 20, top, kM2x + kMw - 12, top + 24, dangerBg, 3) == 0);
        // the bands of text down the page, each named by its brightest pixel: name (fg), cap + lead (muted), cap + reason (chip-err-fg),
        // RE-ENABLE (a key: fg), the orphan note (accent-soft), the integrity and notice notes (chip-err-fg), FILE (muted), PLUGIN ABI (fg)
        struct Band { int y0, y1; Rgb ink; };
        std::vector<Band> bands;
        {
            int gap = 99;
            for (int y = top + 24 + 4; y < kM1y + kMh - 4; ++y) {
                Rgb best{};
                int sum = -1;
                for (int x = kM2x + 16; x < kM2x + kMw - 16; ++x) {
                    const Rgb p = f0.at(x, y);
                    if (nearColour(p, panelAlt, 3)) { continue; }
                    const int sm = p.r + p.g + p.b;
                    if (sm > sum) { sum = sm; best = p; }
                }
                if (sum < 0) { ++gap; continue; }
                if (gap >= 4) { bands.push_back({y, y, best}); }
                Band& b = bands.back();
                b.y1 = y;
                if (best.r + best.g + best.b > b.ink.r + b.ink.g + b.ink.b) { b.ink = best; }
                gap = 0;
            }
        }
        CHECKV(bands.size() == 11, "%zu bands of ink down the page (wanted 11)", bands.size());
        const auto band = [&](std::size_t i) { return i < bands.size() ? bands[i] : Band{-1000, -1000, {}}; };
        const Rgb want[11] = {fg, muted, muted, errFg, errFg, fg, accentSoft, errFg, errFg, muted, fg};
        const char* what[11] = {"the name", "the reach cap", "the lead", "WHY IT IS NOT RUNNING", "the reason", "RE-ENABLE", "the orphan note", "the integrity note", "the notice", "FILE", "PLUGIN ABI"};
        for (std::size_t i = 0; i < 11; ++i) {
            CHECKV(nearColour(band(i).ink, want[i], 14), "%s (band %zu, rows %d..%d): brightest %s, wanted about %s", what[i], i, band(i).y0 - kM1y, band(i).y1 - kM1y, hex(band(i).ink).c_str(), hex(want[i]).c_str());
        }
        // the cap and the reason sit 8 px apart and everything is left-aligned at the padding
        CHECKV(band(4).y0 - band(3).y1 >= 6 && band(4).y0 - band(3).y1 <= 14, "the cap and the reason are %d px apart", band(4).y0 - band(3).y1);
        for (std::size_t i = 1; i < 11; ++i) {
            if (i == 5) { continue; }
            const Ink ink = inkBox(f0, kM2x + 4, band(i).y0, kM2x + kMw - 4, band(i).y1 + 1, panelAlt);
            CHECKV(ink.x0 >= kM2x + 16 && ink.x0 <= kM2x + 18, "%s starts %d px in", what[i], ink.x0 - kM2x);
        }
        // FILE: the cap at the left, the value 150 px in; PLUGIN ABI the same
        const Ink fileKey = inkBox(f0, kM2x + 16, band(9).y0, kM2x + 16 + 140, band(9).y1 + 1, panelAlt);
        const Ink fileVal = inkBox(f0, kM2x + 16 + 140, band(9).y0, kM2x + kMw - 16, band(9).y1 + 1, panelAlt);
        CHECKV(fileKey.n > 20 && fileVal.n > 80 && fileVal.x0 >= kM2x + 16 + 150 && fileVal.x0 <= kM2x + 16 + 152, "FILE: the value starts %d px in", fileVal.x0 - kM2x - 16);
        const Ink abiVal = inkBox(f0, kM2x + 16 + 140, band(10).y0, kM2x + kMw - 16, band(10).y1 + 1, panelAlt);
        CHECKV(abiVal.x0 >= kM2x + 16 + 150 && abiVal.x0 <= kM2x + 16 + 152, "PLUGIN ABI: the value starts %d px in", abiVal.x0 - kM2x - 16);
        CHECK(f0.countNear(kM2x + 16 + 150, band(10).y0, kM2x + kMw - 16, band(10).y1 + 1, fg, 40) >= 25);
        // RE-ENABLE is a plain small key, 24 px high
        const int ry = (band(5).y0 + band(5).y1) / 2;
        const std::vector<Span> rr = runsOnRow(f0, ry, kM2x + 4, kM2x + 300, panelAlt);
        CHECKV(!rr.empty() && spanAt(rr, 0).l == kM2x + 16 && f0.near(spanAt(rr, 0).l + 3, ry, keyBg) && band(5).y1 - band(5).y0 + 1 == 24, "RE-ENABLE: %d..%d, %d rows", spanAt(rr, 0).l, spanAt(rr, 0).r, band(5).y1 - band(5).y0 + 1);
        // its events, and the tip that carries the fault
        reset();
        clk(spanAt(rr, 0).mid(), ry);
        CHECKV(ui->get_page_events() == 1 && sval(ui->get_last_page_event()) == "reenable" && sval(ui->get_last_source()) == "MP2", "RE-ENABLE: '%s' from %s", sval(ui->get_last_page_event()).c_str(), sval(ui->get_last_source()).c_str());
        clk(spanAt(hdr, 0).mid(), mid);
        CHECK(ui->get_page_events() == 2 && sval(ui->get_last_page_event()) == "back" && sval(ui->get_last_source()) == "MP2");
        park();
        mv(spanAt(rr, 0).mid(), ry);
        settle(milliseconds(750));
        const Frame tip = shot();
        CHECKV(tip.near(spanAt(rr, 0).l + 12, band(5).y1 + 1 + 6, knobRing, 3), "RE-ENABLE tooltip border %s", hex(tip.at(spanAt(rr, 0).l + 12, band(5).y1 + 1 + 6)).c_str());
        park();
        // no fault, no key
        ui->set_mp2_fault("");
        const Frame nof = shot();
        CHECKV(nof.countNear(kM2x + 16, band(5).y0, kM2x + 200, band(5).y1 + 1, keyBg, 3) == 0, "RE-ENABLE is still drawn without a fault");
        ui->set_mp2_fault("The plugin guard switched this module off after it faulted twice this session.");
    }

    // =========================================================================================================
    // 18. The skin: a second filling reaches both pages with no component change
    // =========================================================================================================
    {
        Skin probe = ui->global<Workbench>().get_value();
        probe.strip = slint::Color::from_rgb_uint8(0x10, 0x20, 0x30);
        probe.line = slint::Color::from_rgb_uint8(0xff, 0x00, 0xff);
        probe.chip_good_bg = slint::Color::from_rgb_uint8(0x30, 0x00, 0x30);
        probe.danger_bg = slint::Color::from_rgb_uint8(0x00, 0x20, 0x40);
        probe.panel_alt = slint::Color::from_rgb_uint8(0x20, 0x30, 0x10);
        probe.lamp_green = slint::Color::from_rgb_uint8(0x00, 0xff, 0xff);
        probe.plate_line = slint::Color::from_rgb_uint8(0xff, 0xff, 0x00);
        ui->global<Theme>().set_skin(probe);
        const Frame g = shot();
        CHECK(g.near(kF1x + 3, kF1y + 3, rgb(0x102030)));                                                 // the bar
        CHECK(g.rowNear(rowTop(0), kF1x + 2, kF1x + kF1w - 12, rgb(0xff00ff)));                           // a row's rule
        CHECK(g.near(spanAt(chips1, 0).l + 3, fm1Mid, rgb(0x300030)));                                    // the FED chip
        CHECK(g.near(kF1x + 12, rowTop(0) + 13, rgb(0x00ffff)));                                          // the green dot
        CHECK(g.near(kM1x + 16 + 8, kM1y + 12 + 24 + 8 + 18 + 8, rgb(0xffff00)));                         // the plate key's border
        ui->global<Theme>().set_skin(ui->global<Workbench>().get_value());
        const Frame h = shot();
        CHECKV(differing(f0, h, kF1x + 250, kF1y, kF1x + kF1w, kF1y + kF1h) + differing(f0, h, kF1x, kF1y + 40, kF1x + 250, kF1y + kF1h) == 0, "the Workbench filling comes back: %d pixels of FM1 differ", differing(f0, h, kF1x + 250, kF1y, kF1x + kF1w, kF1y + kF1h) + differing(f0, h, kF1x, kF1y + 40, kF1x + 250, kF1y + kF1h));
        CHECKV(differing(f0, h, kM2x, kM1y, kM2x + kMw, kM1y + kMh) == 0, "...and MP2: %d", differing(f0, h, kM2x, kM1y, kM2x + kMw, kM1y + kMh));
    }

    return testSummary("test_slint_fitted");
}
