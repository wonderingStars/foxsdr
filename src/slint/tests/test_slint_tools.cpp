// The TOOLS bank's bodies (ui/tools_bank.slint): RadarBody with the receiver-position offers, BookmarksBody with its row menu, ScannerBody, AirbandBody and the
// Markers sheet - rendered headlessly from ui/specimens/tools_specimen.slint (every body inside the real Section it is shown under) and probed against the Banks
// artboard (artboards/Workbench-Banks.dc.html, the TOOLS column A3) and specs/SCREENS.md A3 / A3b, which the artboard cuts short (the Radar and Airband bodies, the
// row menu).
//
// What is asserted to the pixel: the rows' pitch (24 px rows, 6 px between: the artboard's .rw and .sbd gap), the keys' heights (24 small, 20 xs) and edges, the fields'
// widths (the Scanner's three and four across, the search field and its 104 px dropdown), the 14 px boxes and which are lit, the star and the dot, the lamp, the
// slider, the tones (amber warning and star, red error, green status, the lit and the danger keys), the sheet's frame and its table. What is asserted as ink in a
// region: every word (the typefaces are the embedded Inter and JetBrains Mono, never glyph positions). Every callback is driven by a real pointer or key event and
// read back through the specimen's `Rec` global (the bodies with an override) or through ToolsEvents (the bodies that forward by default).
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "render_harness.hpp"
#include "test_check.hpp"

#include "tools_specimen.h"

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
// A debugging aid: with FOXSDR_SLINT_CROPS set, a region of a frame is saved as its own picture, magnified.
void crop(const Frame& f, const char* name, int x, int y, int w, int h, int z = 2) {
    if (std::getenv("FOXSDR_SLINT_CROPS") == nullptr) { return; }
    Frame c;
    c.width = static_cast<std::uint32_t>(w * z);
    c.height = static_cast<std::uint32_t>(h * z);
    c.px.resize(static_cast<std::size_t>(c.width) * c.height);
    for (int j = 0; j < h * z; ++j) {
        for (int i = 0; i < w * z; ++i) {
            const int sx = x + i / z, sy = y + j / z;
            if (sx < static_cast<int>(f.width) && sy < static_cast<int>(f.height)) { c.px[static_cast<std::size_t>(j) * c.width + static_cast<std::size_t>(i)] = f.px[static_cast<std::size_t>(sy) * f.width + static_cast<std::size_t>(sx)]; }
        }
    }
    c.savePng(shotDir() + name);
}

// ---- the specimen's table (tools_specimen.slint): columns at x 10 / 316 / 622 / 928, Sections 296 wide, bodies at +30, 254 wide ----
constexpr int kColA = 10, kColB = 316, kColC = 622, kColD = 928;
constexpr int kSecRow = 30, kBodyPad = 4, kBodyX = 30, kBodyW = 254;
constexpr int kRow = 24, kPitch = 30;          // the artboard's .rw (min-height 24) and the .sbd gap of 6
constexpr int kW = 1240, kH = 2080;

}  // namespace

int main() {
    installHeadlessPlatform();
    auto ui = ToolsSpecimen::create();
    slint::Window& win = ui->window();

    const Skin wb = ui->global<Workbench>().get_value();
    const Rgb bg = tok(wb.bg), panel = tok(wb.panel), field = tok(wb.field), lineStrong = tok(wb.line_strong);
    const Rgb fg = tok(wb.fg), fgSoft = tok(wb.fg_soft), dim = tok(wb.dim), accent = tok(wb.accent), accentInk = tok(wb.accent_ink), accentSoft = tok(wb.accent_soft);
    const Rgb errFg = tok(wb.chip_err_fg), warnFg = tok(wb.chip_warn_fg), goodFg = tok(wb.chip_good_fg);
    const Rgb keyBg = tok(wb.key_bg), keyLine = tok(wb.key_line), keyFg = tok(wb.key_fg), menu = tok(wb.menu), strip = tok(wb.strip);
    const Rgb dangerBg = tok(wb.danger_bg), dangerLine = tok(wb.danger_line);
    const Rgb lampGreen = tok(wb.lamp_green), knobRing = tok(wb.knob_ring), disabledInk = tok(wb.disabled);
    const Rgb sliderFill = tok(wb.slider_fill), sliderTrack = tok(wb.slider_track), sliderKnob = tok(wb.slider_knob);
    const Rgb dis = over(keyLine, 0.5, panel);                      // a disabled key: the ground is the panel's, the border the key's at half alpha
    CHECK(nearColour(panel, rgb(0x1d2026), 0) && nearColour(keyLine, rgb(0x363b45), 0) && nearColour(accent, rgb(0xf5a524), 0));
    CHECK(nearColour(errFg, rgb(0xff9b9b), 0) && nearColour(warnFg, rgb(0xf5c66b), 0) && nearColour(goodFg, rgb(0x7fe6ad), 0));

    std::vector<Ev> evs;
    ui->global<Rec>().on_fired([&](slint::SharedString n, slint::SharedString a) { evs.push_back({std::string(n), std::string(a)}); });
    std::vector<Ev> fwd;   // what the default-forwarding bodies send to ToolsEvents
    auto& te = ui->global<ToolsEvents>();
    te.on_radar_scope_clicked([&] { fwd.push_back({"radar-scope-clicked", ""}); });
    te.on_position_set([&] { fwd.push_back({"position-set", ""}); });
    te.on_bookmark_tune([&](int id) { fwd.push_back({"bookmark-tune", std::to_string(id)}); });
    te.on_bookmark_favourite([&](int id, bool v) { fwd.push_back({"bookmark-favourite", std::to_string(id) + " " + (v ? "1" : "0")}); });
    te.on_bookmark_delete([&](int id) { fwd.push_back({"bookmark-delete", std::to_string(id)}); });
    te.on_bookmark_import([&] { fwd.push_back({"bookmark-import", ""}); });
    te.on_bookmark_stack([&](bool v) { fwd.push_back({"bookmark-stack", v ? "1" : "0"}); });
    te.on_scanner_ticked([&](bool v) { fwd.push_back({"scanner-ticked", v ? "1" : "0"}); });
    te.on_scanner_run([&] { fwd.push_back({"scanner-run", ""}); });
    te.on_scanner_start([&](slint::SharedString t) { fwd.push_back({"scanner-start", std::string(t)}); });
    te.on_airband_listen([&] { fwd.push_back({"airband-listen", ""}); });
    te.on_airband_above_noise([&] { fwd.push_back({"airband-above-noise", ""}); });
    te.on_markers_closed([&] { fwd.push_back({"markers-closed", ""}); });
    te.on_markers_clear([&] { fwd.push_back({"markers-clear", ""}); });

    const auto clk = [&](int x, int y) { click(win, static_cast<float>(x), static_cast<float>(y)); };
    const auto rclk = [&](int x, int y) { click(win, static_cast<float>(x), static_cast<float>(y), slint::PointerEventButton::Right); };
    const auto mv = [&](int x, int y) { moveTo(win, static_cast<float>(x), static_cast<float>(y)); };
    const auto tap = [&](std::u8string_view k) { keyTap(win, k); };
    const auto shot = [&] { return render(ui, kW, kH); };
    const auto park = [&] { mv(1235, 2075); settle(milliseconds(30)); };
    const auto yOf = [&](double v) { return static_cast<int>(std::lround(v)); };
    // click a spot and report the events it fired (the Rec ones)
    const auto hit = [&](int x, int y) { evs.clear(); fwd.clear(); clk(x, y); shot(); return evs.empty() ? Ev{} : evs.back(); };
    const auto pressAll = [&](int x, int y) { evs.clear(); fwd.clear(); clk(x, y); shot(); return evs; };
    const auto names = [&](const std::vector<Ev>& v) { std::string s; for (const Ev& e : v) { s += (s.empty() ? "" : ", ") + e.name + (e.args.empty() ? "" : " " + e.args); } return s; };
    // type into the field at (x, y): select its text away, type `text`, optionally Enter
    const auto typeInto = [&](int x, int y, const std::string& text, bool enter, int erase = 24) {
        evs.clear();
        clk(x, y);
        tap(slint::platform::key_codes::End);
        for (int i = 0; i < erase; ++i) { tap(slint::platform::key_codes::Backspace); }
        for (const char c : text) { const std::u8string u(1, static_cast<char8_t>(c)); tap(u); }
        if (enter) { tap(slint::platform::key_codes::Return); }
        shot();
    };
    // the list a dropdown opens under a key at (keyX, keyY): row i's middle
    const auto popupRow = [&](int keyX, int keyY, int i) { return std::pair<int, int>{keyX + 30, keyY + kRow + 2 + 5 + i * 26 + 13}; };
    // a dropdown of width w whose right edge is `right`: its border pixels
    const auto dropAt = [&](const Frame& f, int y, int right, int w) {
        return f.near(right - 1, y + 12, keyLine) && f.near(right - w, y + 12, keyLine) && !f.near(right - w - 1, y + 12, keyLine) && !f.near(right + 1, y + 12, keyLine)
            && f.near(right - 40, y, keyLine) && f.near(right - 40, y + 23, keyLine) && !f.near(right - 40, y - 1, keyLine) && !f.near(right - 40, y + 24, keyLine);
    };
    // a 24 px key (or field) spanning [x, x + w) with its top at y: border all round, panel outside
    const auto box24 = [&](const Frame& f, int x, int y, int w, int h = 24) {
        return f.near(x + 12, y, keyLine) && f.near(x + 12, y + h - 1, keyLine) && f.near(x, y + h / 2, keyLine) && f.near(x + w - 1, y + h / 2, keyLine)
            && f.near(x + 12, y - 1, panel, 0) && f.near(x + 12, y + h, panel, 0) && f.near(x - 1, y + h / 2, panel, 0) && f.near(x + w, y + h / 2, panel, 0);
    };
    // the top border row of a 14 px checkbox whose left edge is x, searched in [y0, y1): ticked (the accent box) or empty (the field ground in a line-strong border); -1 when none
    const auto findBox = [&](const Frame& f, int x, int y0, int y1, bool ticked) {
        for (int y = y0; y < y1; ++y) {
            if (f.near(x + 7, y - 1, panel, 0) && (ticked ? (f.near(x + 7, y, accent, 0) && f.near(x + 7, y + 5, accent, 0)) : (f.near(x + 7, y, lineStrong, 0) && f.near(x + 7, y + 4, field, 0)))) { return y; }
        }
        return -1;
    };
    const auto lit = [&](const Frame& f, int x, int y, int w, int h) { return f.countNear(x, y, x + w, y + h, accent, 0); };

    // =========================================================================================================
    // 1. The sheet at rest
    // =========================================================================================================
    park();
    const Frame f0 = shot();
    CHECK(f0.width == kW && f0.height == kH);
    CHECK(f0.near(1235, 2075, bg, 0));
    f0.savePng(shotDir() + "tools-specimen.png");
    // rule 11: a bare body in a 254 px cell asks for no more than 254 px, the longest translations included
    CHECKV(ui->get_p1_w() <= 254.01 && ui->get_p2_w() <= 254.01 && ui->get_p3_w() <= 254.01 && ui->get_p4_w() <= 254.01 && ui->get_p5_w() <= 254.01 && ui->get_p6_w() <= 254.01
               && ui->get_p7_w() <= 254.01 && ui->get_p8_w() <= 254.01,
           "bare bodies in a 254 px cell: %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f", ui->get_p1_w(), ui->get_p2_w(), ui->get_p3_w(), ui->get_p4_w(), ui->get_p5_w(), ui->get_p6_w(),
           ui->get_p7_w(), ui->get_p8_w());
    CHECKV(ui->get_probe_y() >= kH, "the probes sit below the window (%.0f >= %d)", ui->get_probe_y(), kH);
    {
        // every Section is 296 wide with its body 254 wide inside: the panel ground is there, the column beyond it is the background
        CHECK(f0.near(kColA + 3, 80 + 40, panel, 0) || f0.near(kColA + 3, yOf(ui->get_a3_y()) + 40, panel, 0));
        CHECK(f0.near(kColA + 300, yOf(ui->get_a3_y()) + 40, bg, 0));
    }

    // =========================================================================================================
    // 2. Demod scope and Radar: the closed rows, then the open fold (a3: no position yet) and the known-position one (a4)
    // =========================================================================================================
    const int x0a = kColA + kBodyX, Ra = x0a + kBodyW;
    const int T3 = yOf(ui->get_a3_y()) + kSecRow + kBodyPad;
    crop(f0, "crop-radar.png", kColA, yOf(ui->get_a3_y()), 300, 480);
    {
        // the rows with nothing under them: Demod scope (a1) and the closed Radar (a2) are one 30 px row each, 0 px apart
        CHECKV(yOf(ui->get_a2_y()) - yOf(ui->get_a1_y()) == 30, "the closed rows are 30 px: %d", yOf(ui->get_a2_y()) - yOf(ui->get_a1_y()));
        CHECKV(yOf(ui->get_a3_y()) - yOf(ui->get_a2_y()) == 40, "the open Radar starts 10 px under the closed row: %d", yOf(ui->get_a3_y()) - yOf(ui->get_a2_y()));
        CHECKV(f0.countNear(kColA + 200, yOf(ui->get_a1_y()) + 4, kColA + 290, yOf(ui->get_a1_y()) + 26, goodFg, 40) >= 20, "the Demod scope chip is green (AUDIO)");
        // the warning, amber
        CHECKV(f0.countNear(x0a, T3, Ra, T3 + 14, warnFg, 40) >= 80, "the no-position warning is amber: %d", f0.countNear(x0a, T3, Ra, T3 + 14, warnFg, 40));
        // the two offers, full width keys that hold a wrapped sentence: 36 high, 6 px apart
        int k1 = -1;
        for (int y = T3 + 10; y < T3 + 30; ++y) { if (f0.near(x0a + 12, y, keyLine, 0) && f0.near(x0a + 12, y - 1, panel, 0)) { k1 = y; break; } }
        CHECKV(k1 > 0 && k1 - T3 >= 15 && k1 - T3 <= 25, "the first offer key starts under the warning (%d)", k1 - T3);
        int k1b = -1;
        for (int y = k1 + 20; y < k1 + 60; ++y) { if (f0.near(x0a + 12, y, keyLine, 0) && f0.near(x0a + 12, y + 1, panel, 0)) { k1b = y; break; } }
        const int h1 = k1b - k1 + 1;
        CHECKV(h1 >= 33 && h1 <= 40, "the offer key holds two lines: %d high", h1);
        CHECKV(f0.near(x0a, k1 + h1 / 2, keyLine) && f0.near(Ra - 1, k1 + h1 / 2, keyLine) && f0.near(x0a - 1, k1 + h1 / 2, panel, 0), "the offer key spans the body");
        CHECKV(f0.countNear(x0a + 8, k1 + 4, Ra - 8, k1 + h1 - 4, keyFg, 60) >= 150, "the offer's sentence is drawn");
        const int k2 = k1b + 7;
        CHECKV(f0.near(x0a + 12, k2, keyLine, 0) && f0.near(x0a + 12, k2 - 1, panel, 0), "the map-centre offer is 6 px under it");
        const int hintY = k2 + h1 + 6;
        CHECKV(f0.countNear(x0a, hintY, Ra, hintY + 27, dim, 30) >= 100, "the hint is dim");
        // the typed position: two fields (124 + 6 + 124), mono, right-aligned
        const int fy = hintY + 27 + 6;
        CHECKV(box24(f0, x0a, fy, 124) && box24(f0, x0a + 130, fy, 124), "latitude and longitude: two 24 px fields of 124, 6 px apart");
        CHECKV(f0.countNear(x0a + 60, fy + 4, x0a + 118, fy + 20, fg, 60) >= 25 && f0.countNear(x0a + 190, fy + 4, x0a + 248, fy + 20, fg, 60) >= 25, "the figures are drawn");
        CHECKV(f0.near(x0a + 4, fy + 12, field, 0), "a field is the field ground");
        // Set RX here: a small key at the left, 24 high
        const int sy = fy + kPitch;
        CHECKV(f0.near(x0a + 12, sy, keyLine) && f0.near(x0a + 12, sy + 23, keyLine) && f0.near(x0a, sy + 12, keyLine) && f0.near(x0a + 12, sy - 1, panel, 0), "Set RX here is a 24 px key at the left");
        CHECKV(f0.countNear(x0a + 6, sy + 6, x0a + 70, sy + 18, keyFg, 60) >= 25, "Set RX here: its words");
        // the refusal of the typed pair, red
        CHECKV(f0.countNear(x0a, sy + 28, Ra, sy + 46, errFg, 40) >= 80, "the position error is red");
        // the GPS row: the hint, the port dropdown (stretch) and the 84 px baud one, the key across the body, the status and the refusal in amber
        const int gy = sy + 24 + 6 + 14 + 6 + 14 + 6;
        CHECKV(dropAt(f0, gy, Ra, 84) && f0.near(x0a, gy + 12, keyLine) && f0.near(x0a + 12, gy, keyLine), "the GPS port and baud dropdowns (baud 84 wide, flush right)");
        const int gk = gy + kPitch;
        CHECKV(box24(f0, x0a, gk, kBodyW), "Read position from GPS: a key across the body");
        CHECKV(f0.countNear(x0a, gk + 30, Ra, gk + 44, warnFg, 40) >= 50, "the GPS status is amber");
        CHECKV(f0.countNear(x0a, gk + 50, Ra, gk + 78, warnFg, 40) >= 100, "the GPS refusal is amber, on two lines");
        // the footnote (dim, two lines) and then the Radar scope key, 6 px under it, and its note: nothing overlaps
        const int footY = gk + 24 + 6 + 14 + 6 + 27 + 6;
        CHECKV(f0.countNear(x0a, footY, Ra, footY + 27, dim, 30) >= 120, "the footnote is dim and two lines (%d)", f0.countNear(x0a, footY, Ra, footY + 27, dim, 30));
        const int rk = footY + 27 + 6;
        CHECKV(box24(f0, x0a, rk, kBodyW), "Radar scope: a key across the body, clear of the footnote");
        CHECKV(f0.near(x0a + 12, rk + 12, keyBg, 0), "the scope key is not lit while the scope is off");
        CHECKV(f0.countNear(x0a + 40, rk + 5, Ra - 40, rk + 19, keyFg, 60) >= 40, "Radar scope: its words");
        const Ink note = inkBox(f0, x0a, rk + 30, Ra + 4, rk + 90, panel);
        CHECKV(note.n >= 150 && note.y1 - note.y0 >= 30 && note.x1 <= Ra, "the scope note is three dim lines inside the body (n %d, y %d..%d, x1 %d)", note.n, note.y0, note.y1, note.x1);
        CHECKV(f0.countNear(x0a, rk + 30, Ra, rk + 90, dim, 30) >= 150, "the scope note is dim");
        const int bodyEnd = yOf(ui->get_a3_y()) + kSecRow + yOf(ui->get_a3_body_h());
        CHECKV(note.y1 + 12 <= bodyEnd, "the note ends %d above the body's edge", bodyEnd - note.y1);
        CHECKV(f0.countNear(x0a, note.y1 + 2, Ra, bodyEnd - 11, accent, 0) == 0, "no stray ink under the note");

        // ---- the gestures ----
        Ev e = hit(x0a + 100, k1 + h1 / 2);
        CHECKV(e.name == "position-heard", "the traffic-centre offer: '%s'", e.name.c_str());
        e = hit(x0a + 100, k2 + h1 / 2);
        CHECKV(e.name == "position-map", "the map-centre offer: '%s'", e.name.c_str());
        // an offer that cannot be taken greys and does not answer
        {
            auto m = ui->get_m_radar();
            m.offers.heard_enabled = false;
            ui->set_m_radar(m);
            const Frame g = shot();
            CHECKV(g.near(x0a + 12, k1, dis, 6) && g.near(x0a + 12, k1 + 12, panel, 0), "a disabled offer: the panel ground in a half-alpha border");
            e = hit(x0a + 100, k1 + h1 / 2);
            CHECKV(e.name.empty(), "a disabled offer does not answer: '%s'", e.name.c_str());
            m.offers.heard_enabled = true;
            ui->set_m_radar(m);
            shot();
        }
        // a typed latitude and Enter
        evs.clear();
        typeInto(x0a + 60, fy + 12, "52.25", true);
        CHECKV(!evs.empty() && evs.back().name == "position-lat" && evs.back().args == "52.25", "Enter commits the latitude: '%s'", names(evs).c_str());
        // a typed longitude and no Enter: Set RX here commits it first, then asks
        evs.clear();
        typeInto(x0a + 190, fy + 12, "-1.5", false);
        const auto both = pressAll(x0a + 20, sy + 12);
        CHECKV(both.size() == 2 && both[0].name == "position-lon" && both[0].args == "-1.5" && both[1].name == "position-set", "Set RX here commits the typed longitude, then sets: %s", names(both).c_str());
        e = hit(x0a + 100, gk + 12);
        CHECKV(e.name == "gps-toggle", "Read position from GPS: '%s'", e.name.c_str());
        // the GPS dropdowns
        clk(x0a + 60, gy + 12);
        shot();
        auto pr = popupRow(x0a, gy, 1);
        evs.clear();
        clk(pr.first, pr.second);
        shot();
        CHECKV(!evs.empty() && evs.back().name == "gps-port" && evs.back().args == "1", "the GPS port list: '%s'", names(evs).c_str());
        clk(Ra - 40, gy + 12);
        shot();
        pr = popupRow(Ra - 84, gy, 0);
        evs.clear();
        clk(pr.first, pr.second);
        shot();
        CHECKV(!evs.empty() && evs.back().name == "gps-baud" && evs.back().args == "0", "the GPS baud list: '%s'", names(evs).c_str());
        e = hit(x0a + 100, rk + 12);
        CHECKV(e.name == "radar-scope-clicked", "Radar scope: '%s'", e.name.c_str());
        park();
    }
    // a4: the position is known - the line, the typed fields, Set (greyed: the typed pair is refused), the scope key lit
    const int T4 = yOf(ui->get_a4_y()) + kSecRow + kBodyPad;
    {
        const Frame f = shot();
        CHECKV(f.countNear(x0a, T4, Ra, T4 + 14, goodFg, 40) >= 80, "the known-position line is green");
        CHECKV(box24(f, x0a, T4 + 20, 124) && box24(f, x0a + 130, T4 + 20, 124), "known: the typed fields stay");
        CHECKV(f.near(x0a + 12, T4 + 50, dis, 6) && f.near(x0a + 12, T4 + 62, panel, 0), "Set RX here is greyed: the panel ground in a half-alpha border");
        CHECKV(f.countNear(x0a + 2, T4 + 82, Ra - 2, T4 + 102, accent, 0) >= 800, "the scope key is lit (Leave radar scope): %d", f.countNear(x0a + 2, T4 + 82, Ra - 2, T4 + 102, accent, 0));
        CHECKV(f.countNear(x0a + 60, T4 + 86, Ra - 60, T4 + 98, accentInk, 60) >= 40, "Leave radar scope: its words in accent-ink");
        // no offer keys while the position is known: the first thing under the line is the field
        CHECKV(!f.near(x0a + 12, T4 + 14, keyLine, 0) && f.near(x0a + 12, T4 + 20, keyLine, 0), "known: no offer keys");
        fwd.clear();
        clk(x0a + 100, T4 + 92);
        shot();
        CHECKV(fwd.size() == 1 && fwd[0].name == "radar-scope-clicked", "Leave radar scope forwards to ToolsEvents (%zu)", fwd.size());
        fwd.clear();
        clk(x0a + 20, T4 + 62);
        shot();
        CHECKV(fwd.empty(), "a greyed Set RX here does not answer");
        park();
    }

    // =========================================================================================================
    // 3. Bookmarks (b1: seven rows, the group keys, the menu; b2: no groups, default-forwarding; b3: fourteen rows)
    // =========================================================================================================
    const int x0b = kColB + kBodyX, Rb = x0b + kBodyW;
    const int Tb = yOf(ui->get_b1_y()) + kSecRow + kBodyPad;
    int specX = -1, spec = -1;                                  // where the second tick of the Bookmarks row sits
    const int rt = Tb + 290;                                      // the first row's top (the keys wrapped on two lines)
    crop(f0, "crop-bookmarks.png", kColB, yOf(ui->get_b1_y()), 300, 560);
    {
        // name + Add current, group + the arrow, import + Import: three rows 6 px apart, keys at the right
        CHECKV(f0.near(x0b + 12, Tb, keyLine, 0) && f0.near(x0b + 12, Tb + 23, keyLine, 0) && f0.near(x0b, Tb + 12, keyLine, 0), "the name field starts at the left edge");
        CHECKV(f0.near(Rb - 1, Tb + 12, keyLine) && f0.near(Rb - 30, Tb, keyLine) && f0.near(Rb - 30, Tb + 23, keyLine) && f0.near(Rb - 30, Tb - 1, panel, 0), "Add current: a 24 px key flush right");
        CHECKV(f0.countInk(x0b + 8, Tb + 5, x0b + 60, Tb + 19, field) >= 12, "the name field's placeholder");
        CHECKV(f0.countNear(Rb - 90, Tb + 5, Rb - 6, Tb + 19, keyFg, 60) >= 40, "Add current: its words");
        const int gy = Tb + kPitch;
        CHECKV(f0.near(Rb - 1, gy + 12, keyLine) && f0.near(Rb - 28, gy + 12, keyLine) && !f0.near(Rb - 29, gy + 12, keyLine) && f0.near(Rb - 14, gy, keyLine) && f0.near(Rb - 14, gy + 23, keyLine),
               "the group arrow: a 28 px dropdown flush right");
        CHECKV(f0.near(Rb - 30, gy + 12, field, 0) || f0.near(Rb - 31, gy + 12, panel, 0) || f0.near(Rb - 32, gy + 12, panel, 0), "the group field ends 4 px before it");
        CHECKV(f0.countInk(Rb - 24, gy + 6, Rb - 4, gy + 18, keyBg) >= 8, "the arrow's chevron");
        const int iy = Tb + 2 * kPitch;
        CHECKV(f0.near(Rb - 1, iy + 12, keyLine) && f0.near(Rb - 20, iy, keyLine) && f0.near(Rb - 20, iy + 23, keyLine), "Import: a 24 px key flush right");
        CHECKV(f0.countInk(x0b + 8, iy + 5, x0b + 120, iy + 19, field) >= 20, "the import path's placeholder");
        // the two notes under it, dim, one line each
        CHECKV(f0.countNear(x0b, Tb + 90, Rb, Tb + 104, dim, 30) >= 80, "the import note is dim");
        CHECKV(f0.countNear(x0b, Tb + 104, Rb, Tb + 119, dim, 30) >= 80, "the disk-wait line is dim");
        // the search field and the 104 px group filter, then the three ticks
        CHECKV(dropAt(f0, Tb + 130, Rb, 104), "the group filter: a 104 px dropdown flush right");
        CHECKV(f0.near(x0b + 12, Tb + 130, keyLine, 0) && f0.near(Rb - 110, Tb + 142, panel, 0) && f0.near(Rb - 107, Tb + 142, panel, 0), "the search field shares the row, 8 px from it");
        const int fav = findBox(f0, x0b, Tb + 155, Tb + 190, false);
        for (int x = x0b + 90; x < x0b + 170 && spec < 0; ++x) { spec = findBox(f0, x, Tb + 155, Tb + 190, true); specX = x; }
        CHECKV(fav == Tb + 165, "Favourites only: an empty 14 px box (top %d, want %d)", fav, Tb + 165);
        CHECKV(spec == Tb + 165, "On the spectrum: ticked, beside it (top %d, x %d)", spec, specX);
        const int stack = findBox(f0, x0b, Tb + 185, Tb + 220, true);
        CHECKV(stack == Tb + 195, "Stack close names: ticked, on the next line (top %d)", stack);
        CHECKV(f0.countNear(x0b + 22, Tb + 160, x0b + 120, Tb + 184, fg, 60) >= 40, "Favourites only: its words");
        CHECKV(f0.countNear(x0b, Tb + 220, Rb, Tb + 235, dim, 30) >= 40, "the count line is dim");
        // the three xs keys wrap: Export for SDR# and Remove this group on the first line, Rename this group under them, 4 px apart
        const int ky = Tb + 240;
        CHECKV(f0.near(x0b + 12, ky, keyLine) && f0.near(x0b + 12, ky + 19, keyLine) && !f0.near(x0b + 12, ky + 20, keyLine) && f0.near(x0b + 12, ky - 1, panel, 0), "Export for SDR#: an xs key (20 high)");
        CHECKV(f0.near(x0b + 150, ky, keyLine) && f0.near(x0b + 150, ky + 19, keyLine), "Remove this group: beside it on the first line");
        CHECKV(f0.near(x0b + 12, ky + 24, keyLine) && f0.near(x0b + 12, ky + 43, keyLine) && !f0.near(x0b + 150, ky + 24, keyLine), "Rename this group wraps under them (4 px between the lines)");
        CHECKV(f0.countNear(x0b + 6, ky + 4, x0b + 100, ky + 16, keyFg, 60) >= 25, "Export for SDR#: its words");
        // the rows: 24 px at a pitch of 30; a tick, the star or the dot, the text, the delete key
        for (int i = 0; i < 7; ++i) {
            const int ry = rt + i * kPitch;
            const bool ticked = i == 0 || i == 2 || i == 5;
            const bool star = i == 0 || i == 5;
            CHECKV(findBox(f0, x0b, ry + 2, ry + 10, ticked) == ry + 5, "row %d: the tick is %s", i, ticked ? "lit" : "empty");
            const int amber = f0.countNear(x0b + 20, ry + 4, x0b + 36, ry + 20, warnFg, 40);
            CHECKV(star ? amber >= 5 : amber == 0, "row %d: %s (%d amber pixels)", i, star ? "the amber star" : "no star", amber);
            CHECKV(f0.countInk(x0b + 20, ry + 4, x0b + 36, ry + 20, panel) >= 3, "row %d: the star or the dot is drawn", i);
            CHECKV(f0.countNear(x0b + 44, ry + 4, Rb - 28, ry + 20, fg, 60) >= 60, "row %d: its text in the mono face", i);
            CHECKV(f0.near(Rb - 10, ry + 2, keyLine) && f0.near(Rb - 10, ry + 21, keyLine) && f0.near(Rb - 20, ry + 12, keyLine) && f0.near(Rb - 1, ry + 12, panel, 0) == false && f0.near(Rb - 21, ry + 12, panel, 0),
                   "row %d: the delete key is 20 px, flush right", i);
            CHECKV(f0.countInk(Rb - 16, ry + 6, Rb - 4, ry + 18, keyBg) >= 8, "row %d: the delete key's cross", i);
        }
        CHECKV(!f0.near(Rb - 10, rt + 7 * kPitch + 2, keyLine, 0), "there is no eighth row");
        CHECKV(f0.countNear(x0b, rt + 7 * kPitch - 6 + 6, Rb, rt + 7 * kPitch + 14, errFg, 40) >= 80, "the save error is red under the rows");
        const int bodyEnd = yOf(ui->get_b1_y()) + kSecRow + yOf(ui->get_b1_body_h());
        CHECKV(bodyEnd - 12 - (rt + 7 * kPitch + 14) >= -1 && bodyEnd - 12 - (rt + 7 * kPitch + 14) <= 2, "the body ends 12 px under the error line (%d)", bodyEnd - 12 - (rt + 7 * kPitch + 14));
    }
    {
        // ---- the gestures: name + Add current, the group arrow, import, search, the filter, the ticks ----
        typeInto(x0b + 40, Tb + 12, "abc", false);
        auto got = pressAll(Rb - 30, Tb + 12);
        CHECKV(got.size() == 2 && got[0].name == "bookmark-name" && got[0].args == "abc" && got[1].name == "bookmark-add", "name, then Add current: %s", names(got).c_str());
        got = pressAll(Rb - 30, Tb + 12);
        CHECKV(got.size() == 1 && got[0].name == "bookmark-add", "Add current with nothing left to commit: %s", names(got).c_str());
        typeInto(x0b + 40, Tb + 42, "Mine", true);
        CHECKV(!evs.empty() && evs.back().name == "bookmark-group" && evs.back().args == "Mine", "Enter commits the group draft: %s", names(evs).c_str());
        clk(Rb - 14, Tb + kPitch + 12);
        shot();
        auto pr = popupRow(Rb - 28, Tb + kPitch, 1);
        evs.clear();
        clk(pr.first, pr.second);
        shot();
        CHECKV(!evs.empty() && evs.back().name == "bookmark-group" && evs.back().args == "Space", "the group arrow's list names the group: %s", names(evs).c_str());
        typeInto(x0b + 40, Tb + 72, "c:/a.csv", false);
        got = pressAll(Rb - 20, Tb + 72);
        CHECKV(got.size() == 2 && got[0].name == "bookmark-import-path" && got[0].args == "c:/a.csv" && got[1].name == "bookmark-import", "path, then Import: %s", names(got).c_str());
        evs.clear();
        clk(x0b + 40, Tb + 142);
        tap(u8"g");
        shot();
        CHECKV(!evs.empty() && evs.back().name == "bookmark-search" && evs.back().args == "g", "the search follows the typing: %s", names(evs).c_str());
        clk(Rb - 50, Tb + 142);
        shot();
        pr = popupRow(Rb - 104, Tb + 130, 2);
        evs.clear();
        clk(pr.first, pr.second);
        shot();
        CHECKV(!evs.empty() && evs.back().name == "bookmark-filter" && evs.back().args == "2", "the group filter: %s", names(evs).c_str());
        Ev e = hit(x0b + 60, Tb + 172);
        CHECKV(e.name == "bookmark-favourites" && e.args == "1", "Favourites only asks for on: '%s' '%s'", e.name.c_str(), e.args.c_str());
        e = hit(x0b + 160, Tb + 172);
        CHECKV(e.name == "bookmark-on-spectrum" && e.args == "0", "On the spectrum (lit) asks for off: '%s' '%s'", e.name.c_str(), e.args.c_str());
        e = hit(x0b + 60, Tb + 202);
        CHECKV(e.name == "bookmark-stack" && e.args == "0", "Stack close names (lit) asks for off: '%s' '%s'", e.name.c_str(), e.args.c_str());
        // the model owns the ticks: the pixels still show what the model says (the pointer away: a hovered lit box is accent-hover)
        park();
        const Frame g = shot();
        CHECKV(findBox(g, x0b, Tb + 155, Tb + 190, false) == Tb + 165 && findBox(g, specX, Tb + 155, Tb + 190, true) == Tb + 165 && findBox(g, x0b, Tb + 185, Tb + 220, true) == Tb + 195,
               "a refused tick springs back (%d %d %d)", findBox(g, x0b, Tb + 155, Tb + 190, false), findBox(g, specX, Tb + 155, Tb + 190, true), findBox(g, x0b, Tb + 185, Tb + 220, true));
        // the group keys
        e = hit(x0b + 40, Tb + 250);
        CHECKV(e.name == "bookmark-export", "Export for SDR#: '%s'", e.name.c_str());
        e = hit(x0b + 150, Tb + 250);
        CHECKV(e.name == "bookmark-remove-group", "Remove this group: '%s'", e.name.c_str());
        // ---- the rows ----
        e = hit(x0b + 28, rt + kPitch + 12);
        CHECKV(e.name == "bookmark-favourite" && e.args == "12 1", "a dot asks for a star: '%s' '%s'", e.name.c_str(), e.args.c_str());
        e = hit(x0b + 28, rt + 12);
        CHECKV(e.name == "bookmark-favourite" && e.args == "11 0", "a star asks for a dot: '%s' '%s'", e.name.c_str(), e.args.c_str());
        e = hit(x0b + 7, rt + kPitch + 12);
        CHECKV(e.name == "bookmark-tick" && e.args == "12 1", "an empty tick asks for lit: '%s' '%s'", e.name.c_str(), e.args.c_str());
        e = hit(x0b + 7, rt + 12);
        CHECKV(e.name == "bookmark-tick" && e.args == "11 0", "a lit tick asks for empty: '%s' '%s'", e.name.c_str(), e.args.c_str());
        e = hit(x0b + 120, rt + 2 * kPitch + 12);
        CHECKV(e.name == "bookmark-tune" && e.args == "13", "the row's text tunes it: '%s' '%s'", e.name.c_str(), e.args.c_str());
        e = hit(Rb - 10, rt + kPitch + 12);
        CHECKV(e.name == "bookmark-delete" && e.args == "12", "the delete key: '%s' '%s'", e.name.c_str(), e.args.c_str());
        park();
        // the text row lights while the pointer is on it
        mv(x0b + 120, rt + 3 * kPitch + 12);
        settle(milliseconds(30));
        const Frame h = shot();
        CHECKV(h.near(x0b + 200, rt + 3 * kPitch + 3, tok(wb.menu_hot), 0), "the row under the pointer lights (menu-hot)");
        park();
    }
    {
        // ---- the row menu: a PopupWindow beside the row, opened by a right-click ----
        const int rowTop = rt + 2 * kPitch;                       // "Heathrow Tower", group EGLL (the third in the list)
        const int px = Rb + 12, ptop = rowTop - 5;
        evs.clear();
        rclk(x0b + 120, rowTop + 12);
        const Frame m = shot();
        CHECKV(!evs.empty() && evs.back().name == "bookmark-menu" && evs.back().args == "13", "a right-click opens the menu and says which row: %s", names(evs).c_str());
        m.savePng(shotDir() + "tools-specimen-menu.png");
        crop(m, "crop-menu.png", Rb - 40, rowTop - 20, 300, 330);
        CHECKV(m.near(px + 195, ptop + 60, menu, 0) && m.near(px + 100, ptop, knobRing, 0), "the menu: the menu ground in a knob-ring border, beside the row");
        CHECKV(m.countNear(px + 12, ptop + 5, px + 190, ptop + 31, fg, 60) >= 40, "the menu's header is the row's name");
        CHECKV(m.countNear(px + 12, ptop + 31, px + 190, ptop + 53, tok(wb.muted), 60) >= 25, "Move to group: the cap");
        const int ycap = ptop + 53, ylist = ptop + 79;
        CHECKV(m.countNear(px + 26, ycap + 3, px + 190, ycap + 23, fg, 60) >= 25, "No group: its words");
        CHECKV(m.countNear(px + 26, ylist + 3, px + 190, ylist + 23, fg, 60) >= 20 && m.countNear(px + 26, ylist + 26 * 5 + 3, px + 190, ylist + 26 * 5 + 23, fg, 60) >= 15, "the six groups follow, 26 px apart");
        CHECKV(m.countNear(px + 5, ylist + 26 * 2 + 4, px + 19, ylist + 26 * 2 + 22, accent, 60) >= 8, "the row's own group (EGLL) carries the tick");
        CHECKV(m.countNear(px + 5, ylist + 4, px + 19, ylist + 22, accent, 60) == 0, "and the others do not");
        CHECKV(m.countNear(px + 8, ylist + 26 * 6 + 8, px + 192, ylist + 26 * 6 + 40, field, 0) >= 3000, "the new-group field closes the menu");
        // a pick of a group
        evs.clear();
        clk(px + 100, ylist + 26 * 1 + 13);
        shot();
        CHECKV(!evs.empty() && evs.back().name == "bookmark-move" && evs.back().args == "13 [Space]", "the menu's group row moves the bookmark: %s", names(evs).c_str());
        const Frame closed = shot();
        CHECKV(closed.near(px + 195, ptop + 60, bg, 0) || !closed.near(px + 195, ptop + 60, menu, 0), "the menu closes after a pick");
        // No group
        rclk(x0b + 120, rowTop + 12);
        shot();
        evs.clear();
        clk(px + 100, ycap + 13);
        shot();
        CHECKV(!evs.empty() && evs.back().name == "bookmark-move" && evs.back().args == "13 []", "No group moves it out of its group: %s", names(evs).c_str());
        // a new group, typed
        rclk(x0b + 120, rowTop + 12);
        shot();
        evs.clear();
        clk(px + 100, ylist + 26 * 6 + 4 + 1 + 5 + 12);
        tap(u8"M"); tap(u8"i"); tap(u8"n"); tap(u8"e");
        tap(slint::platform::key_codes::Return);
        shot();
        CHECKV(!evs.empty() && evs.back().name == "bookmark-move" && evs.back().args == "13 [Mine]", "a new group name and Enter: %s", names(evs).c_str());
        // the popup is modal: the click that dismisses it is eaten, and says nothing
        rclk(x0b + 120, rowTop + 12);
        shot();
        evs.clear();
        clk(x0b + 120, rt + 4 * kPitch + 12);
        shot();
        CHECKV(evs.empty(), "the click that dismisses a menu is eaten by it: %s", names(evs).c_str());
        park();
        shot();
    }
    {
        // ---- Rename this group opens the rename row; its field and key ----
        const int ky = Tb + 240;
        evs.clear();
        clk(x0b + 40, ky + 24 + 10);
        const Frame r = shot();
        // opening the row asks the host once (docs 6.61): it seeds the field with the group's own name, as the ImGui key does
        CHECKV(evs.size() == 1 && evs[0].name == "bookmark-rename-open" && evs[0].args == "1", "opening the rename row asks the host to seed the field: %s", names(evs).c_str());
        const int ry = ky + 44 + 6;
        CHECKV(r.near(x0b + 12, ry, keyLine, 0) && r.near(Rb - 1, ry + 12, keyLine) && r.near(Rb - 40, ry, keyLine), "the rename row: the field and the Rename key");
        typeInto(x0b + 40, ry + 12, "Beacons", false);
        auto got = pressAll(Rb - 25, ry + 12);
        CHECKV(got.size() == 2 && got[0].name == "bookmark-rename-draft" && got[0].args == "Beacons" && got[1].name == "bookmark-rename", "the draft, then Rename: %s", names(got).c_str());
        const Frame c = shot();
        CHECKV(c.near(x0b + 12, ry, panel, 0) == false || findBox(c, x0b, rt - 8 + 0, rt + 10, true) == rt + 5, "Rename closes the row (the first row is back at %d)", rt);
        park();
    }
    // b2: no groups (no arrow, no filter, no group keys), Import busy, Stack close names greyed; every callback forwarded to ToolsEvents
    const int Tb2 = yOf(ui->get_b2_y()) + kSecRow + kBodyPad;
    {
        const Frame f = shot();
        CHECKV(f.near(Rb - 1, Tb2 + 30 + 12, lineStrong, 0) && f.near(Rb - 14, Tb2 + 30, lineStrong, 0), "b2: with no groups the group field takes the whole row");
        CHECKV(f.near(Rb - 1, Tb2 + 90 + 12, lineStrong, 0) && f.near(Rb - 14, Tb2 + 90, lineStrong, 0), "b2: with no groups the search field takes the whole row");
        CHECKV(f.near(Rb - 20, Tb2 + 60, dis, 6) && f.near(Rb - 20, Tb2 + 63, panel, 0) && f.near(Rb - 1, Tb2 + 72, dis, 6), "b2: Import is greyed while an import runs");
        const int stackBox = Tb2 + 150 + 5;
        CHECKV(f.near(x0b + 7, stackBox + 7, tok(wb.key_disabled_bg), 0) && f.near(x0b, stackBox + 7, dis, 6), "b2: Stack close names is greyed (the disabled ground, a half-alpha border)");
        CHECKV(f.countNear(x0b + 22, stackBox - 3, x0b + 120, stackBox + 17, tok(wb.key_disabled_fg), 40) >= 20, "b2: and so are its words");
        CHECKV(f.countNear(x0b, Tb2 + 180, Rb, Tb2 + 194, dim, 30) >= 30, "b2: the count line");
        const int rr = Tb2 + 200;
        CHECKV(f.near(Rb - 10, rr + 2, keyLine) && f.near(Rb - 10, rr + 30 + 2, keyLine) && !f.near(Rb - 10, rr + 60 + 2, keyLine, 0), "b2: two rows (no key flow, so none of its 6 px gap)");
        fwd.clear();
        clk(Rb - 20, Tb2 + 72);
        shot();
        CHECKV(fwd.empty(), "b2: a greyed Import does not answer");
        clk(x0b + 120, rr + 12);
        shot();
        CHECKV(fwd.size() == 1 && fwd[0].name == "bookmark-tune" && fwd[0].args == "21", "b2: the row forwards bookmark-tune to ToolsEvents (%zu)", fwd.size());
        fwd.clear();
        clk(x0b + 28, rr + 12);
        shot();
        CHECKV(fwd.size() == 1 && fwd[0].name == "bookmark-favourite" && fwd[0].args == "21 1", "b2: the star forwards (%zu)", fwd.size());
        fwd.clear();
        clk(Rb - 10, rr + 12);
        shot();
        CHECKV(fwd.size() == 1 && fwd[0].name == "bookmark-delete" && fwd[0].args == "21", "b2: the delete key forwards");
        fwd.clear();
        clk(x0b + 60, Tb2 + 150 + 12);
        shot();
        CHECKV(fwd.empty(), "b2: a greyed Stack close names does not answer");
        park();
    }
    // b3: fourteen rows and the longest labels - the list stops at twelve rows and scrolls
    {
        const int T3b = yOf(ui->get_b3_y()) + kSecRow + kBodyPad;
        const int full = yOf(ui->get_b3_body_h());
        auto m = ui->get_m_bm_long();
        const auto rows = m.rows;
        std::vector<BookmarkRow> two;
        for (int i = 0; i < 2; ++i) { two.push_back(*rows->row_data(static_cast<std::size_t>(i))); }
        m.rows = std::make_shared<slint::VectorModel<BookmarkRow>>(two);
        ui->set_m_bm_long(m);
        shot();
        const int small = yOf(ui->get_b3_body_h());
        CHECKV(full - small == 12 * kPitch - 6 - (2 * kPitch - 6), "14 rows stand as 12: %d px more than 2 rows (want %d)", full - small, 10 * kPitch);
        m.rows = rows;
        ui->set_m_bm_long(m);
        const Frame f = shot();
        CHECKV(yOf(ui->get_b3_body_h()) == full, "back to fourteen rows");
        // the thumb: 6 px wide, 9 px from the right edge, knob-ring, over the rows
        int thumb = 0;
        for (int y = T3b; y < T3b + full; ++y) { if (f.near(Rb - 6, y, knobRing, 0)) { ++thumb; } }
        CHECKV(thumb >= 20, "the scroll thumb is drawn (%d px)", thumb);
        // a wheel over the list scrolls it: the first row's text leaves the first line
        // rule 11 at the longest: the add key and the import key give their words up rather than widen the row
        CHECKV(f.near(Rb - 1, T3b + 12, keyLine) && f.near(x0b, T3b + 12, keyLine), "b3: the long Add current stays inside the body");
        park();
    }

    // =========================================================================================================
    // 4. Scanner (c1 idle, c2 scanning, c3 ticked mode)
    // =========================================================================================================
    const int x0c = kColC + kBodyX, Rc = x0c + kBodyW;
    const int Tc1 = yOf(ui->get_c1_y()) + kSecRow + kBodyPad, Tc2 = yOf(ui->get_c2_y()) + kSecRow + kBodyPad, Tc3 = yOf(ui->get_c3_y()) + kSecRow + kBodyPad;
    crop(f0, "crop-scanner.png", kColC, yOf(ui->get_c1_y()), 300, 680);
    {
        // the tick, then the captioned fields: 3 across and 4 across, 6 px apart, each field 24 high with its caption (dim) above
        CHECKV(findBox(f0, x0c, Tc1, Tc1 + 12, false) == Tc1 + 5, "Ticked frequencies: an empty 14 px box (top %d)", findBox(f0, x0c, Tc1, Tc1 + 12, false));
        CHECKV(f0.countNear(x0c + 22, Tc1 + 4, x0c + 190, Tc1 + 20, fg, 60) >= 60, "Ticked frequencies (3): its words");
        const int f1 = Tc1 + 46, f2 = Tc1 + 92;                // the input tops: a caption of 14 and 2 px of air under the row's top
        // the field boxes along a row: the runs of the border colour on the box's top line
        const auto boxes = [&](const Frame& f, int y) {
            std::vector<std::pair<int, int>> out;
            for (int x = x0c; x < Rc; ++x) {
                if (f.near(x, y + 12, keyLine, 0) && !f.near(x - 1, y + 12, keyLine, 0) && f.near(x + 12, y, keyLine, 0)) {
                    int e = x + 1;
                    while (e < Rc && !(f.near(e, y + 12, keyLine, 0) && f.near(e - 1, y + 12, field, 0))) { ++e; }
                    out.push_back({x, e + 1});
                }
            }
            return out;
        };
        const auto r1 = boxes(f0, f1);
        CHECKV(r1.size() == 3 && r1[0].first == x0c && r1[2].second == Rc, "the first row holds three fields from edge to edge (%zu)", r1.size());
        for (std::size_t i = 0; i < r1.size(); ++i) {
            const int x = r1[i].first, w = r1[i].second - r1[i].first;
            CHECKV(f0.near(x + 12, f1 + 23, keyLine, 0) && w >= 70 && w <= 90, "field %zu of the first row: a 24 px box at x %d, %d wide", i, x, w);
            CHECKV(f0.near(x + 4, f1 + 12, field, 0), "field %zu: the field ground", i);
            CHECKV(f0.countNear(x, f1 - 17, x + 70, f1 - 3, dim, 40) >= 15, "field %zu: its caption is dim, above", i);
            CHECKV(f0.countNear(x + 20, f1 + 5, x + w - 6, f1 + 19, fg, 60) >= 25, "field %zu: the figure, mono", i);
            if (i > 0) { CHECKV(x - r1[i - 1].second == 6, "fields %zu and %zu are 6 px apart (%d)", i - 1, i, x - r1[i - 1].second); }
        }
        const auto r2 = boxes(f0, f2);
        CHECKV(r2.size() == 4 && r2[0].first == x0c && r2[3].second == Rc, "the second row holds four fields from edge to edge (%zu)", r2.size());
        for (std::size_t i = 0; i < r2.size(); ++i) {
            const int x = r2[i].first;
            CHECKV(f0.near(x + 12, f2 + 23, keyLine, 0) && r2[i].second - x >= 50, "field %zu of the second row: a 24 px box at x %d", i + 3, x);
            CHECKV(f0.countNear(x, f2 - 17, x + 50, f2 - 3, dim, 40) >= 12, "field %zu: its caption", i + 3);
        }
        // Start scan: a key across the body, 6 px under the fields
        const int sk = Tc1 + 122;
        CHECKV(box24(f0, x0c, sk, kBodyW) && f0.near(x0c + 12, sk + 12, keyBg, 0), "Start scan: a 24 px key across the body (not lit)");
        CHECKV(f0.countNear(x0c + 80, sk + 5, Rc - 80, sk + 19, keyFg, 60) >= 40, "Start scan: its words");
        CHECKV(f0.countNear(x0c, sk + 30, Rc, sk + 44, dim, 30) >= 50, "the status line (Idle) is dim");
        // c2: scanning - Stop scan lit at the left half, Skip at the right, 6 px apart; the status green
        const int sk2 = Tc2 + 122, half = (kBodyW - 6) / 2;
        const Frame f = f0;
        CHECKV(f.countNear(x0c + 2, sk2 + 2, x0c + half - 2, sk2 + 22, accent, 0) >= 700, "Stop scan is lit: %d", f.countNear(x0c + 2, sk2 + 2, x0c + half - 2, sk2 + 22, accent, 0));
        CHECKV(f.near(x0c + half + 2, sk2 + 6, panel, 0) && f.near(x0c + half + 5, sk2 + 6, panel, 0) && f.near(x0c + half + 6, sk2 + 6, keyLine, 0), "a 6 px gap");
        CHECKV(box24(f, x0c + half + 6, sk2, half) && f.near(x0c + half + 6 + 12, sk2 + 12, keyBg, 0), "Skip: the other half, unlit");
        CHECKV(f.countNear(x0c + 40, sk2 + 5, x0c + half - 40, sk2 + 19, accentInk, 60) >= 30, "Stop scan: its words in accent-ink");
        CHECKV(f.countNear(x0c, sk2 + 30, Rc, sk2 + 44, goodFg, 40) >= 80, "Scanning | 145.2125 MHz is green");
        // c3: ticked mode - the first three fields greyed, the others not, Start scan greyed, the status amber, the tick lit
        const int g1 = Tc3 + 46, g2 = Tc3 + 92;
        CHECKV(findBox(f, x0c, Tc3, Tc3 + 12, true) == Tc3 + 5, "c3: Ticked frequencies (0) is ticked");
        const int gx[3] = {x0c, x0c + 87, x0c + 173};
        for (int i = 0; i < 3; ++i) {
            const int x = gx[i];
            CHECKV(f.near(x + 4, g1 + 12, tok(wb.key_disabled_bg), 0) && f.near(x + 12, g1, dis, 6), "c3: range field %d is greyed", i);
        }
        CHECKV(f.near(x0c + 4, g2 + 12, field, 0), "c3: Dwell is not greyed");
        CHECKV(f.near(x0c + 12, Tc3 + 122, dis, 6) && f.near(x0c + 12, Tc3 + 122 + 12, tok(wb.key_disabled_bg), 0), "c3: Start scan is greyed (nothing ticked)");
        CHECKV(f.countNear(x0c, Tc3 + 152, Rc, Tc3 + 166, warnFg, 40) >= 80, "c3: Paused (signal) is amber");

        // ---- the gestures ----
        auto got = pressAll(x0c + 100, sk + 12);
        CHECKV(got.size() == 1 && got[0].name == "scanner-run", "Start scan: %s", names(got).c_str());
        typeInto(x0c + 30, f1 + 12, "145", false);
        got = pressAll(x0c + 100, sk + 12);
        CHECKV(got.size() == 2 && got[0].name == "scanner-start" && got[0].args == "145" && got[1].name == "scanner-run", "a typed range start is committed before the run: %s", names(got).c_str());
        typeInto(x0c + 100, f1 + 12, "146.5", true);
        CHECKV(!evs.empty() && evs.back().name == "scanner-stop" && evs.back().args == "146.5", "Enter in Stop MHz: %s", names(evs).c_str());
        typeInto(x0c + 190, f1 + 12, "25", true);
        CHECKV(!evs.empty() && evs.back().name == "scanner-step" && evs.back().args == "25", "Enter in Step kHz: %s", names(evs).c_str());
        const struct { int i; const char* name; } timing[4] = {{0, "scanner-dwell"}, {1, "scanner-hold"}, {2, "scanner-resume"}, {3, "scanner-listen"}};
        for (const auto& t : timing) {
            const int x = static_cast<std::size_t>(t.i) < r2.size() ? r2[static_cast<std::size_t>(t.i)].first : x0c + t.i * 60;   // a missing box is a failure above, not an out-of-bounds read here
            typeInto(x + 20, f2 + 12, "77", true);
            CHECKV(!evs.empty() && evs.back().name == t.name && evs.back().args == "77", "Enter in %s: %s", t.name, names(evs).c_str());
        }
        Ev e = hit(x0c + 60, Tc1 + 12);
        CHECKV(e.name == "scanner-ticked" && e.args == "1", "Ticked frequencies asks for on: '%s' '%s'", e.name.c_str(), e.args.c_str());
        e = hit(x0c + 40, sk2 + 12);
        CHECKV(e.name == "scanner-halt", "Stop scan: '%s'", e.name.c_str());
        e = hit(Rc - 40, sk2 + 12);
        CHECKV(e.name == "scanner-skip", "Skip: '%s'", e.name.c_str());
        // c3 forwards to ToolsEvents
        fwd.clear();
        clk(x0c + 60, Tc3 + 12);
        shot();
        CHECKV(fwd.size() == 1 && fwd[0].name == "scanner-ticked" && fwd[0].args == "0", "c3: the tick (lit) asks for off through ToolsEvents (%zu)", fwd.size());
        fwd.clear();
        clk(x0c + 100, Tc3 + 122 + 12);
        shot();
        CHECKV(fwd.empty(), "c3: a greyed Start scan does not answer");
        clk(x0c + 30, g1 + 12);
        const Frame ring = shot();
        CHECKV(ring.countNear(x0c - 4, g1 - 4, x0c + 90, g1 + 28, tok(wb.focus), 0) == 0, "c3: a greyed field takes no focus ring");
        park();
    }

    // =========================================================================================================
    // 5. Airband (c4 idle, d1 listening, d2 plain)
    // =========================================================================================================
    const int Tc4 = yOf(ui->get_c4_y()) + kSecRow + kBodyPad;
    crop(f0, "crop-airband.png", kColC, yOf(ui->get_c4_y()), 300, 740);
    {
        // the lookup row: the code field, Look up (greyed while the code is empty); Nearest airports and Hide
        CHECKV(f0.near(x0c + 12, Tc4, keyLine, 0) && f0.near(Rc - 12, Tc4, dis, 6) && f0.near(Rc - 12, Tc4 + 3, panel, 0), "Look up is greyed while the code is empty");
        CHECKV(f0.countInk(x0c + 8, Tc4 + 5, x0c + 150, Tc4 + 19, field) >= 30, "the code field's placeholder");
        CHECKV(f0.near(x0c + 12, Tc4 + 30, keyLine, 0) && f0.near(x0c + 12, Tc4 + 53, keyLine, 0) && f0.near(x0c + 12, Tc4 + 29, panel, 0), "Nearest airports: a 24 px key at the left");
        CHECKV(f0.near(x0c + 150, Tc4 + 30, keyLine, 0) || f0.near(x0c + 130, Tc4 + 30, keyLine, 0), "Hide beside it");
        // the nearest list (two rows) and the ambiguous one (one), selectable text lines of 24 px
        for (int i = 0; i < 3; ++i) { CHECKV(f0.countNear(x0c + 4, Tc4 + 60 + i * kPitch + 4, Rc - 4, Tc4 + 60 + i * kPitch + 20, fg, 60) >= 40, "airport line %d: its words", i); }
        // the preset rows: "Preset" + the field + the arrow, the frequency + the Mode dropdown, the name + Add (greyed until a frequency is typed)
        const int py = Tc4 + 150;
        CHECKV(f0.near(Rc - 1, py + 12, keyLine) && f0.near(Rc - 28, py + 12, keyLine) && f0.near(Rc - 14, py, keyLine), "Preset: the arrow is a 28 px dropdown flush right");
        CHECKV(f0.countNear(x0c, py + 5, x0c + 40, py + 19, fgSoft, 60) >= 25, "Preset: the caption");
        CHECKV(f0.near(x0c + 50, py, keyLine, 0) && f0.near(x0c + 50, py + 23, keyLine, 0), "Preset: the name field beside it");
        CHECKV(dropAt(f0, Tc4 + 180, Rc, 76), "Mode: a 76 px dropdown flush right");
        CHECKV(f0.countInk(x0c + 8, Tc4 + 185, x0c + 80, Tc4 + 199, field) >= 15, "Frequency (MHz): the placeholder");
        CHECKV(f0.near(Rc - 20, Tc4 + 210, dis, 6) && f0.near(Rc - 20, Tc4 + 213, panel, 0), "Add is greyed until a frequency is typed");
        CHECKV(f0.countNear(x0c, Tc4 + 240, Rc, Tc4 + 254, dim, 30) >= 60, "the preset note is dim");
        // the import row and the three small keys (Export CSV, Remove preset, Open the folder), the group dropdown, the count and the two tick keys
        CHECKV(f0.near(Rc - 1, Tc4 + 272, keyLine) && f0.near(Rc - 20, Tc4 + 260, keyLine), "Import: a key flush right");
        CHECKV(f0.near(x0c + 12, Tc4 + 290, keyLine, 0) && f0.near(x0c + 12, Tc4 + 313, keyLine, 0) && f0.near(x0c + 140, Tc4 + 290, keyLine, 0), "Export CSV and Remove preset: small keys side by side");
        CHECKV(f0.countNear(x0c, Tc4 + 320, Rc, Tc4 + 334, dim, 30) >= 60, "the export note");
        CHECKV(f0.near(x0c + 12, Tc4 + 340, keyLine, 0) && f0.near(x0c + 12, Tc4 + 363, keyLine, 0), "Open the folder: a small key");
        CHECKV(box24(f0, x0c, Tc4 + 370, kBodyW), "the group dropdown fills the row");
        CHECKV(f0.countNear(x0c + 8, Tc4 + 375, Rc - 30, Tc4 + 389, fg, 60) >= 60, "the group dropdown's closed text");
        CHECKV(f0.countNear(x0c, Tc4 + 400, Rc, Tc4 + 414, dim, 30) >= 40, "6 ticked of 9");
        CHECKV(f0.near(x0c + 12, Tc4 + 420, keyLine, 0) && f0.near(x0c + 12, Tc4 + 443, keyLine, 0) && f0.near(x0c + 100, Tc4 + 420, keyLine, 0), "Tick all and Untick all: small keys");
        CHECKV(findBox(f0, x0c, Tc4 + 450, Tc4 + 465, false) == Tc4 + 455, "Busiest first: an empty box (top %d)", findBox(f0, x0c, Tc4 + 450, Tc4 + 465, false));
        // three channels: a tick, a lamp, the frequency and name, the amber time at the right; the lamps are dark (nothing is heard)
        for (int i = 0; i < 3; ++i) {
            const int ry = Tc4 + 480 + i * kPitch;
            const bool ticked = i < 2;
            CHECKV(findBox(f0, x0c, ry + 2, ry + 10, ticked) == ry + 5, "channel %d: the tick is %s", i, ticked ? "lit" : "empty");
            CHECKV(f0.countNear(x0c + 20, ry + 8, x0c + 30, ry + 16, lampGreen, 0) == 0, "channel %d: the lamp is dark", i);
            CHECKV(f0.countInk(x0c + 20, ry + 8, x0c + 30, ry + 16, panel) >= 20, "channel %d: the lamp's ring is there", i);
            CHECKV(f0.countNear(x0c + 36, ry + 4, Rc - 64, ry + 20, fg, 60) >= 60, "channel %d: its frequency and name", i);
        }
        CHECKV(f0.countNear(Rc - 60, Tc4 + 484, Rc, Tc4 + 500, warnFg, 40) >= 20, "channel 0: the heard time is amber");
        CHECKV(f0.countNear(Rc - 60, Tc4 + 514, Rc, Tc4 + 530, warnFg, 40) == 0, "channel 1: no time, nothing amber");
        // the squelch slider: the caption, the track with its fill to the model's 0.436, the readout, then Above the noise (greyed), the hold field and its unit, LISTEN
        const int sq = Tc4 + 570;
        CHECKV(f0.countNear(x0c, sq + 5, x0c + 56, sq + 19, tok(wb.fg_soft), 60) >= 25, "Squelch: the caption");
        int ts = -1, te2 = -1;
        for (int x = x0c; x < Rc; ++x) { if (f0.near(x, sq + 12, sliderTrack, 0) || f0.near(x, sq + 12, sliderFill, 0)) { if (ts < 0) { ts = x; } te2 = x; } }
        CHECKV(ts >= x0c + 56 && te2 <= Rc - 62 && te2 - ts > 80, "the slider track runs between the caption and the readout (%d..%d)", ts, te2);
        const int kx = ts + static_cast<int>(std::lround(0.436 * (te2 - ts + 1)));
        CHECKV(f0.near(kx, sq + 12, sliderKnob, 8), "the knob sits at 0.436 of the track (x %d)", kx);
        CHECKV(f0.near(ts + 10, sq + 12, sliderFill, 0) && f0.near(te2 - 10, sq + 12, sliderTrack, 0), "filled to the knob, bare beyond it");
        CHECKV(f0.countNear(Rc - 60, sq + 5, Rc, sq + 19, fg, 60) >= 15, "-62 dB: the readout, flush right");
        CHECKV(f0.near(x0c + 12, Tc4 + 600, dis, 6) && f0.near(x0c + 12, Tc4 + 603, panel, 0), "Above the noise is greyed while nothing is playing");
        CHECKV(f0.near(Rc - 40, Tc4 + 630, lineStrong, 0) && f0.near(Rc - 40, Tc4 + 653, lineStrong, 0) && f0.near(Rc - 77, Tc4 + 640, lineStrong, 0) == false, "hold: a 64 px field before its unit");
        CHECKV(box24(f0, x0c, Tc4 + 660, kBodyW) && f0.near(x0c + 12, Tc4 + 672, keyBg, 0), "LISTEN: a key across the body, not lit");
        CHECKV(f0.countNear(x0c + 90, Tc4 + 665, Rc - 90, Tc4 + 679, keyFg, 60) >= 30, "LISTEN: its words");
        const int bodyEnd = yOf(ui->get_c4_y()) + kSecRow + yOf(ui->get_c4_body_h());
        CHECKV(bodyEnd - 12 == Tc4 + 660 + 24, "the body ends 12 px under LISTEN (%d)", bodyEnd - 12 - (Tc4 + 684));

        // ---- the gestures, in the order the sheet is read ----
        typeInto(x0c + 40, Tc4 + 12, "egll", true);
        CHECKV(evs.size() == 2 && evs[0].name == "airband-code" && evs[0].args == "egll" && evs[1].name == "airband-lookup", "Enter in the code field looks it up: %s", names(evs).c_str());
        const Frame typed = shot();
        CHECKV(typed.near(Rc - 12, Tc4, keyLine, 0), "Look up answers once there is a code");
        auto got = pressAll(Rc - 20, Tc4 + 12);
        CHECKV(got.size() == 1 && got[0].name == "airband-lookup", "Look up: %s", names(got).c_str());
        typeInto(x0c + 40, Tc4 + 12, "kord", false);
        got = pressAll(Rc - 20, Tc4 + 12);
        CHECKV(got.size() == 2 && got[0].name == "airband-code" && got[0].args == "kord" && got[1].name == "airband-lookup", "a typed code is committed before the lookup: %s", names(got).c_str());
        Ev e = hit(x0c + 40, Tc4 + 42);
        CHECKV(e.name == "airband-nearest", "Nearest airports: '%s'", e.name.c_str());
        e = hit(x0c + 128, Tc4 + 42);
        CHECKV(e.name == "airband-hide", "Hide: '%s'", e.name.c_str());
        e = hit(x0c + 60, Tc4 + 72);
        CHECKV(e.name == "airband-airport" && e.args == "0", "the first nearest airport: '%s' '%s'", e.name.c_str(), e.args.c_str());
        e = hit(x0c + 60, Tc4 + 102);
        CHECKV(e.name == "airband-airport" && e.args == "1", "the second: '%s' '%s'", e.name.c_str(), e.args.c_str());
        e = hit(x0c + 60, Tc4 + 132);
        CHECKV(e.name == "airband-ambiguous" && e.args == "0", "an ambiguous choice: '%s' '%s'", e.name.c_str(), e.args.c_str());
        clk(Rc - 14, py + 12);
        shot();
        auto pr = popupRow(Rc - 28, py, 1);
        evs.clear();
        clk(pr.first, pr.second);
        shot();
        CHECKV(!evs.empty() && evs.back().name == "airband-preset" && evs.back().args == "1", "the preset arrow: %s", names(evs).c_str());
        typeInto(x0c + 100, py + 12, "Stansted", true);
        CHECKV(!evs.empty() && evs.back().name == "airband-preset-name" && evs.back().args == "Stansted", "Enter in the preset name: %s", names(evs).c_str());
        typeInto(x0c + 40, Tc4 + 192, "118.7", true);
        CHECKV(!evs.empty() && evs.back().name == "airband-freq" && evs.back().args == "118.7", "Enter in Frequency (MHz): %s", names(evs).c_str());
        clk(Rc - 38, Tc4 + 192);
        shot();
        pr = popupRow(Rc - 76, Tc4 + 180, 1);
        evs.clear();
        clk(pr.first, pr.second);
        shot();
        CHECKV(!evs.empty() && evs.back().name == "airband-mode" && evs.back().args == "1", "the Mode list: %s", names(evs).c_str());
        const Frame withFreq = shot();
        CHECKV(withFreq.near(Rc - 20, Tc4 + 210, keyLine, 0), "Add answers once a frequency is typed");
        typeInto(x0c + 40, Tc4 + 222, "Tower", true);
        CHECKV(!evs.empty() && evs.back().name == "airband-name" && evs.back().args == "Tower", "Enter in the name: %s", names(evs).c_str());
        e = hit(Rc - 20, Tc4 + 222);
        CHECKV(e.name == "airband-add", "Add: '%s'", e.name.c_str());
        typeInto(x0c + 40, Tc4 + 272, "d:/x.csv", false);
        got = pressAll(Rc - 20, Tc4 + 272);
        CHECKV(got.size() == 2 && got[0].name == "airband-import-path" && got[0].args == "d:/x.csv" && got[1].name == "airband-import", "path, then Import: %s", names(got).c_str());
        e = hit(x0c + 30, Tc4 + 302);
        CHECKV(e.name == "airband-export", "Export CSV: '%s'", e.name.c_str());
        e = hit(x0c + 130, Tc4 + 302);
        CHECKV(e.name == "airband-remove-preset", "Remove preset: '%s'", e.name.c_str());
        e = hit(x0c + 40, Tc4 + 352);
        CHECKV(e.name == "airband-folder", "Open the folder: '%s'", e.name.c_str());
        clk(x0c + 100, Tc4 + 382);
        shot();
        pr = popupRow(x0c, Tc4 + 370, 0);
        evs.clear();
        clk(pr.first, pr.second);
        shot();
        CHECKV(!evs.empty() && evs.back().name == "airband-group" && evs.back().args == "0", "the group list: %s", names(evs).c_str());
        e = hit(x0c + 30, Tc4 + 432);
        CHECKV(e.name == "airband-tick-all", "Tick all: '%s'", e.name.c_str());
        e = hit(x0c + 100, Tc4 + 432);
        CHECKV(e.name == "airband-untick-all", "Untick all: '%s'", e.name.c_str());
        e = hit(x0c + 60, Tc4 + 462);
        CHECKV(e.name == "airband-busiest" && e.args == "1", "Busiest first asks for on: '%s' '%s'", e.name.c_str(), e.args.c_str());
        e = hit(x0c + 7, Tc4 + 492);
        CHECKV(e.name == "airband-tick" && e.args == "31 0", "a lit channel tick asks for empty: '%s' '%s'", e.name.c_str(), e.args.c_str());
        e = hit(x0c + 7, Tc4 + 552);
        CHECKV(e.name == "airband-tick" && e.args == "33 1", "an empty one asks for lit: '%s' '%s'", e.name.c_str(), e.args.c_str());
        e = hit(x0c + 90, Tc4 + 522);
        CHECKV(e.name == "airband-tune" && e.args == "32", "a channel's text tunes it: '%s' '%s'", e.name.c_str(), e.args.c_str());
        // the squelch: a drag over the track moves it, the knob asks for the new fraction
        evs.clear();
        mv(kx, sq + 12);
        press(win, static_cast<float>(kx), static_cast<float>(sq + 12));
        moveTo(win, static_cast<float>(kx + 30), static_cast<float>(sq + 12));
        release(win, static_cast<float>(kx + 30), static_cast<float>(sq + 12));
        shot();
        CHECKV(!evs.empty() && evs.back().name == "airband-squelch", "dragging the squelch: %s", names(evs).c_str());
        const double dragged = evs.empty() ? -1.0 : std::atof(evs.back().args.c_str());
        CHECKV(dragged > 0.55 && dragged < 0.9, "the knob moved right of 0.436 (%.3f)", dragged);
        // the squelch is the model's: its readout does not change under the drag
        CHECKV(shot().near(kx + 30, sq + 12, sliderKnob, 8), "the knob follows the drag until the host says otherwise");
        // Above the noise is greyed: no answer
        e = hit(x0c + 40, Tc4 + 612);
        CHECKV(e.name.empty(), "Above the noise (greyed) does not answer: '%s'", e.name.c_str());
        typeInto(Rc - 40, Tc4 + 642, "6", true);
        CHECKV(!evs.empty() && evs.back().name == "airband-hold" && evs.back().args == "6", "Enter in hold: %s", names(evs).c_str());
        e = hit(x0c + 100, Tc4 + 672);
        CHECKV(e.name == "airband-listen", "LISTEN: '%s'", e.name.c_str());
        park();
    }
    const int Td1 = yOf(ui->get_d1_y()) + kSecRow + kBodyPad;
    const int x0d = kColD + kBodyX, Rd = x0d + kBodyW;
    crop(f0, "crop-airband-run.png", kColD, yOf(ui->get_d1_y()), 300, 560);
    {
        const Frame f = shot();
        CHECKV(f.countNear(x0d, Td1 + 320, Rd, Td1 + 334, fgSoft, 40) >= 50, "Hearing: 118.700, in fg-soft");
        // two channels: the first heard (a green lamp, the amber time), the second quiet
        const int r1 = Td1 + 340, r2 = Td1 + 370;
        CHECKV(f.countNear(x0d + 20, r1 + 8, x0d + 30, r1 + 16, lampGreen, 0) >= 20, "channel 1 (heard): the lamp is lit green (%d)", f.countNear(x0d + 20, r1 + 8, x0d + 30, r1 + 16, lampGreen, 0));
        CHECKV(f.countNear(x0d + 20, r2 + 8, x0d + 30, r2 + 16, lampGreen, 0) == 0, "channel 2: the lamp is dark");
        CHECKV(f.countNear(Rd - 60, r1 + 4, Rd, r1 + 20, warnFg, 40) >= 20, "channel 1: the heard time is amber");
        CHECKV(findBox(f, x0d, r1 + 2, r1 + 10, true) == r1 + 5 && findBox(f, x0d, r2 + 2, r2 + 10, true) == r2 + 5, "both channels are ticked");
        CHECKV(findBox(f, x0d, Td1 + 290, Td1 + 305, true) == Td1 + 295, "Busiest first is ticked here");
        // Above the noise answers while listening; STOP (danger) and Next block share the last row
        CHECKV(f.near(x0d + 12, Td1 + 430, keyLine, 0) && f.near(x0d + 12, Td1 + 453, keyLine, 0), "Above the noise: a live key");
        const int sk = Td1 + 490, half = (kBodyW - 6) / 2;
        CHECKV(f.near(x0d + 12, sk, dangerLine, 0) && f.near(x0d + 12, sk + 12, dangerBg, 0) && f.near(x0d + half - 1, sk + 12, dangerLine, 0), "STOP: the danger key on the left half");
        CHECKV(f.countNear(x0d + 40, sk + 5, x0d + half - 40, sk + 19, tok(wb.danger_fg), 60) >= 20, "STOP: its words in danger-fg");
        CHECKV(box24(f, x0d + half + 6, sk, half), "Next block: the right half");
        Ev e = hit(x0d + 40, Td1 + 442);
        CHECKV(e.name == "airband-above-noise", "Above the noise: '%s'", e.name.c_str());
        e = hit(x0d + 40, sk + 12);
        CHECKV(e.name == "airband-listen", "STOP asks the monitor to stop: '%s'", e.name.c_str());
        e = hit(Rd - 40, sk + 12);
        CHECKV(e.name == "airband-next", "Next block: '%s'", e.name.c_str());
        park();
    }
    const int Td2 = yOf(ui->get_d2_y()) + kSecRow + kBodyPad;
    {
        const Frame f = shot();
        CHECKV(box24(f, x0d, Td2 + 210, kBodyW) || f.near(x0d + 12, Td2 + 210, keyLine, 0), "d2: the group dropdown fills the row even with nothing listed");
        // LISTEN is the last key of the body: find it by its top border at the left edge, searching upward from the body's end
        const int bodyEnd = yOf(ui->get_d2_y()) + kSecRow + yOf(ui->get_d2_body_h()) - 12;
        CHECKV(box24(f, x0d, bodyEnd - 24, kBodyW), "d2: LISTEN is a key across the body at its foot");
        fwd.clear();
        clk(x0d + 100, bodyEnd - 12);
        shot();
        CHECKV(fwd.size() == 1 && fwd[0].name == "airband-listen", "d2: LISTEN forwards to ToolsEvents (%zu)", fwd.size());
        fwd.clear();
        clk(x0d + 40, bodyEnd - 24 - 6 - 24 - 6 - 24 + 12);
        shot();
        CHECKV(fwd.empty(), "d2: Above the noise (greyed) does not forward");
        park();
    }

    // =========================================================================================================
    // 6. The Markers sheets
    // =========================================================================================================
    const int sy = yOf(ui->get_sheets_y());
    const int panelX = 10 + (660 - 620) / 2, panelY = sy + (380 - 340) / 2;
    const int cx = panelX + 1 + 12;
    {
        const Frame f = shot();
        crop(f, "crop-markers.png", 10, sy, 660, 380);
        f.savePng(shotDir() + "tools-specimen-markers.png");
        CHECKV(f.near(panelX + 100, panelY + 150, panel, 0) && f.near(panelX + 300, panelY, knobRing, 0) && f.near(panelX, panelY + 150, knobRing, 0) && f.near(panelX + 619, panelY + 150, knobRing, 0), "the sheet: the panel ground in a knob-ring border, 620 wide");
        CHECKV(panelY + 340 >= 0 && f.near(panelX + 300, panelY + 339, knobRing, 0) && !f.near(panelX + 300, panelY + 341, knobRing, 0), "the sheet is 340 high");
        CHECKV(f.near(panelX + 100, panelY + 15, menu, 0) && f.near(panelX + 100, panelY + 30, tok(wb.line_strong), 0), "the title strip: the menu ground with a rule under it, 30 high");
        CHECKV(f.countNear(panelX + 12, panelY + 8, panelX + 120, panelY + 24, fg, 60) >= 40, "the title (MARKERS) in fg");
        CHECKV(f.countInk(panelX + 590, panelY + 6, panelX + 616, panelY + 26, menu) >= 12, "the close key's cross");
        // the heading strip: 22 high on the strip ground, caps in muted, a rule under it
        int hy = panelY + 31;
        while (hy < panelY + 60 && !f.near(cx + 300, hy, strip, 0)) { ++hy; }
        CHECKV(f.near(cx + 300, hy + 4, strip, 0) && f.near(cx + 300, hy + 21, lineStrong, 0), "the table heading is on the strip ground with a rule under it (22 high)");
        CHECKV(f.countNear(cx + 8, hy + 5, cx + 590, hy + 17, tok(wb.muted), 60) >= 80, "the headings (#, FREQUENCY, NOTED, NOTE) are muted caps");
        // the rows: 28 high, M1 in accent-soft, the frequency, the time (dim), the note field, three keys
        for (int i = 0; i < 3; ++i) {
            const int ry = hy + 22 + i * 28;
            CHECKV(f.countNear(cx + 8, ry + 6, cx + 40, ry + 22, accentSoft, 40) >= 5, "marker %d: its tag in accent-soft (%d)", i + 1, f.countNear(cx + 8, ry + 6, cx + 40, ry + 22, accentSoft, 40));
            CHECKV(f.countNear(cx + 52, ry + 6, cx + 160, ry + 22, fg, 60) >= 40, "marker %d: its frequency", i + 1);
            CHECKV(f.countNear(cx + 176, ry + 6, cx + 240, ry + 22, dim, 40) >= 25, "marker %d: when it was dropped (dim)", i + 1);
            CHECKV(f.near(cx + 262, ry + 2, lineStrong, 0) && f.near(cx + 262, ry + 25, lineStrong, 0), "marker %d: the note is a 24 px field", i + 1);
            CHECKV(f.near(cx + 452, ry + 4, keyLine, 0) && f.near(cx + 452, ry + 23, keyLine, 0) && f.near(cx + 452, ry + 3, panel, 0), "marker %d: Tune, an xs key (20 high)", i + 1);
        }
        CHECKV(f.countNear(cx + 262 + 6, hy + 22 + 28 + 6, cx + 400, hy + 22 + 28 + 22, tok(wb.disabled), 40) >= 15, "marker 2: an empty note shows 'add a note'");
        CHECKV(f.countNear(cx + 262 + 6, hy + 22 + 6, cx + 400, hy + 22 + 22, fg, 60) >= 30, "marker 1: its note");
        // the foot: two small keys at the left, 12 px above the sheet's edge
        const int fy = panelY + 340 - 1 - 12 - 24;
        CHECKV(f.near(cx + 12, fy, keyLine, 0) && f.near(cx + 12, fy + 23, keyLine, 0), "Copy markers to clipboard: a small key at the left, at the foot");
        CHECKV(f.countNear(cx + 8, fy + 5, cx + 160, fy + 19, keyFg, 60) >= 60, "Copy markers to clipboard: its words");
        CHECKV(f.countNear(cx + 168, fy + 5, cx + 300, fy + 19, keyFg, 60) >= 40, "Clear all markers: its words");

        // ---- the gestures ----
        const int r1 = hy + 22, r2 = hy + 22 + 28, r3 = hy + 22 + 56;
        evs.clear();
        clk(cx + 340, r2 + 14);
        tap(u8"h"); tap(u8"i");
        shot();
        CHECKV(!evs.empty() && evs.back().name == "markers-note" && evs.back().args == "2 hi", "a note is saved as it is typed: %s", names(evs).c_str());
        Ev e = hit(cx + 452 + 10, r1 + 14);
        CHECKV(e.name == "markers-tune" && e.args == "1", "Tune: '%s' '%s'", e.name.c_str(), e.args.c_str());
        e = hit(cx + 452 + 60, r3 + 14);
        CHECKV(e.name == "markers-bookmark" && e.args == "3", "Bookmark: '%s' '%s'", e.name.c_str(), e.args.c_str());
        e = hit(cx + 594 - 8 - 10, r2 + 14);
        CHECKV(e.name == "markers-remove" && e.args == "2", "the remove key: '%s' '%s'", e.name.c_str(), e.args.c_str());
        e = hit(cx + 60, fy + 12);
        CHECKV(e.name == "markers-copy", "Copy markers to clipboard: '%s'", e.name.c_str());
        // Clear all markers asks twice
        e = hit(cx + 220, fy + 12);
        CHECKV(e.name.empty(), "the first press of Clear all markers only asks: '%s'", e.name.c_str());
        const Frame armed = shot();
        CHECKV(armed.near(cx + 170 + 6, fy, dangerLine, 0) || armed.countNear(cx + 160, fy + 2, cx + 340, fy + 22, dangerBg, 0) >= 300, "Yes, remove all 3: the key turns to danger (%d)", armed.countNear(cx + 160, fy + 2, cx + 340, fy + 22, dangerBg, 0));
        e = hit(cx + 220, fy + 12);
        CHECKV(e.name == "markers-clear", "the second press clears: '%s'", e.name.c_str());
        e = hit(panelX + 620 - 4 - 13, panelY + 15);
        CHECKV(e.name == "markers-closed", "the close key: '%s'", e.name.c_str());
        e = hit(14, sy + 6);
        CHECKV(e.name == "markers-closed", "a click on the backdrop dismisses: '%s'", e.name.c_str());
        e = hit(panelX + 300, panelY + 330 - 40);
        CHECKV(e.name.empty(), "a click on the panel itself does not: '%s'", e.name.c_str());
        park();
    }
    {
        // the empty sheet (default-forwarding): the red error, the sentence, the two keys greyed
        const int ex = 690, ey = sy, ePX = ex + 12, ePY = ey + 20;
        const Frame f = shot();
        CHECKV(f.near(ePX + 100, ePY + 150, panel, 0) || f.near(ePX + 12 + 100, ePY + 150, panel, 0), "the empty sheet is drawn");
        const int cx2 = ePX + 13;
        CHECKV(f.countNear(cx2, ePY + 44, cx2 + 520, ePY + 60, errFg, 40) >= 80, "the error is red");
        CHECKV(f.countNear(cx2, ePY + 64, cx2 + 540, ePY + 80, dim, 30) >= 80, "No markers yet...: the empty sentence is dim");
        const int fy = ePY + 340 - 1 - 12 - 24;
        CHECKV(f.near(cx2 + 12, fy, dis, 6) && f.near(cx2 + 12, fy + 3, panel, 0), "with no markers Copy and Clear are greyed (%d %d)", fy, ePY);
        fwd.clear();
        clk(cx2 + 60, fy + 12);
        shot();
        CHECKV(fwd.empty(), "a greyed key does not answer");
        clk(ex + 4, ey + 4);
        shot();
        CHECKV(fwd.size() == 1 && fwd[0].name == "markers-closed", "a click on the backdrop forwards markers-closed (%zu)", fwd.size());
        park();
    }

    return testSummary("test_slint_tools");
}
