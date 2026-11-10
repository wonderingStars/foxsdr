// The EXTEND bank's bodies and the Plugin store page (ui/extend_bank.slint): WebBody and CatBody under their real Sections and StorePage in four states -
// rendered headlessly from ui/specimens/extend_specimen.slint and probed against the Banks artboard (artboards/Workbench-Banks.dc.html, the EXTEND column A4)
// and the Pages artboard (Workbench-Pages.dc.html, the PLUGIN STORE tab: top bar, SITES panel, Browse grid, foot, Updates pane, clean-up confirm, plugin page).
//
// What is asserted to the pixel: the bodies' rows (24 px fields and keys at a pitch of 6, the 150 px dropdown flush right, the 70 px Port beside the User name,
// the 20 px SHOW key, the keys' wrap, the 80 px CAT port), the tones in order (red error, green serving, red warning, amber plain-HTTP note, dim counts), the
// page's top bar (the strip ground and its rule, the latched tab key, the keys), the cards (1, 2 or 3 to a row by the page's width; the column width held by a
// partly filled last row; the glyph tile; the one key in each of its kinds; the progress bar), the foot, the Updates pane, the clean-up confirm's place and the
// plugin page. What is asserted as ink in a region: every word (the typefaces are the embedded Inter and JetBrains Mono, never glyph positions). Every callback is
// driven by a real pointer or key event and read back through the specimen's `Rec` global (the components with an override) or through ExtendEvents (the plain
// ones).
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "render_harness.hpp"
#include "test_check.hpp"

#include "extend_specimen.h"

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

// the first row of each run of `want` down the column x in [y0, y1)
std::vector<int> vruns(const Frame& f, int x, int y0, int y1, Rgb want, int tol = 0) {
    std::vector<int> r;
    bool in = false;
    for (int y = y0; y < y1; ++y) {
        const bool hit = f.near(x, y, want, tol);
        if (hit && !in) { r.push_back(y); }
        in = hit;
    }
    return r;
}
// the first column of each run of `want` along the row y in [x0, x1)
std::vector<int> hruns(const Frame& f, int y, int x0, int x1, Rgb want, int tol = 0) {
    std::vector<int> r;
    bool in = false;
    for (int x = x0; x < x1; ++x) {
        const bool hit = f.near(x, y, want, tol);
        if (hit && !in) { r.push_back(x); }
        in = hit;
    }
    return r;
}
std::string list(const std::vector<int>& v) {
    std::string s;
    for (const int i : v) { s += (s.empty() ? "" : " ") + std::to_string(i); }
    return "[" + s + "]";
}

// ---- the specimen's table (extend_specimen.slint): columns at x 10 / 316, Sections 296 wide, bodies at +30, 254 wide ----
constexpr int kColA = 10, kColB = 316;
constexpr int kSecRow = 30, kBodyPad = 4, kBodyX = 30, kBodyW = 254;
constexpr int kRow = 24, kPitch = 30;          // the artboard's .rw (min-height 24) and the .sbd gap of 6
constexpr int kW = 1240, kH = 3230;
constexpr int kNarrowX = 622, kNarrowY = 10, kNarrowW = 500, kNarrowH = 700;
constexpr int kStX = 10, kBrowseY = 1070, kUpdatesY = 1790, kPluginY = 2510, kStW = 1000, kStH = 700;

}  // namespace

int main() {
    installHeadlessPlatform();
    auto ui = ExtendSpecimen::create();
    slint::Window& win = ui->window();

    const Skin wb = ui->global<Workbench>().get_value();
    const Rgb bg = tok(wb.bg), panel = tok(wb.panel), field = tok(wb.field), lineStrong = tok(wb.line_strong), line = tok(wb.line), strip = tok(wb.strip);
    const Rgb fg = tok(wb.fg), fgSoft = tok(wb.fg_soft), dim = tok(wb.dim), muted = tok(wb.muted), accent = tok(wb.accent), accentInk = tok(wb.accent_ink), accentSoft = tok(wb.accent_soft);
    const Rgb errFg = tok(wb.chip_err_fg), warnFg = tok(wb.chip_warn_fg), goodFg = tok(wb.chip_good_fg);
    const Rgb warnBg = tok(wb.chip_warn_bg), chipBg = tok(wb.chip_bg);
    const Rgb keyBg = tok(wb.key_bg), keyLine = tok(wb.key_line), keyFg = tok(wb.key_fg), card = tok(wb.card), menu = tok(wb.menu);
    const Rgb dangerBg = tok(wb.danger_bg), cardWarn = tok(wb.card_warn), cardWarnLine = tok(wb.card_warn_line), knobRing = tok(wb.knob_ring);
    const Rgb lampGreen = tok(wb.lamp_green), lampRed = tok(wb.lamp_red);
    const Rgb sliderTrack = tok(wb.slider_track), sliderFill = tok(wb.slider_fill), well = tok(wb.well);
    const Rgb dis = over(keyLine, 0.5, panel);                      // a disabled key: the ground is the panel's, the border the key's at half alpha
    CHECK(nearColour(panel, rgb(0x1d2026), 0) && nearColour(keyLine, rgb(0x363b45), 0) && nearColour(accent, rgb(0xf5a524), 0));
    CHECK(nearColour(errFg, rgb(0xff9b9b), 0) && nearColour(warnFg, rgb(0xf5c66b), 0) && nearColour(goodFg, rgb(0x7fe6ad), 0));
    CHECK(nearColour(lineStrong, keyLine, 0));                      // the field border and the key border are one colour on the sheet

    std::vector<Ev> evs;
    ui->global<Rec>().on_fired([&](slint::SharedString n, slint::SharedString a) { evs.push_back({std::string(n), std::string(a)}); });
    std::vector<Ev> fwd;   // what the default-forwarding components send to ExtendEvents
    auto& ee = ui->global<ExtendEvents>();
    ee.on_web_serve([&](bool v) { fwd.push_back({"web-serve", v ? "1" : "0"}); });
    ee.on_web_reach([&](int i) { fwd.push_back({"web-reach", std::to_string(i)}); });
    ee.on_cat_reachable([&](bool v) { fwd.push_back({"cat-reachable", v ? "1" : "0"}); });
    ee.on_store_tab([&](int i) { fwd.push_back({"store-tab", std::to_string(i)}); });
    ee.on_store_check([&] { fwd.push_back({"store-check", ""}); });
    ee.on_store_sites_toggle([&] { fwd.push_back({"store-sites-toggle", ""}); });
    ee.on_store_cancel([&] { fwd.push_back({"store-cancel", ""}); });
    ee.on_store_site_remove([&](int i) { fwd.push_back({"store-site-remove", std::to_string(i)}); });
    ee.on_store_site_draft([&](slint::SharedString t) { fwd.push_back({"store-site-draft", std::string(t)}); });
    ee.on_store_consent([&](bool v) { fwd.push_back({"store-consent", v ? "1" : "0"}); });
    ee.on_store_site_add([&] { fwd.push_back({"store-site-add", ""}); });
    ee.on_store_card_key([&](slint::SharedString id) { fwd.push_back({"store-card-key", std::string(id)}); });
    ee.on_store_card_open([&](slint::SharedString id) { fwd.push_back({"store-card-open", std::string(id)}); });

    const auto clk = [&](int x, int y) { click(win, static_cast<float>(x), static_cast<float>(y)); };
    const auto mv = [&](int x, int y) { moveTo(win, static_cast<float>(x), static_cast<float>(y)); };
    const auto tap = [&](std::u8string_view k) { keyTap(win, k); };
    const auto shot = [&] { return render(ui, kW, kH); };
    const auto park = [&] { mv(1235, 3225); settle(milliseconds(30)); };
    const auto yOf = [&](double v) { return static_cast<int>(std::lround(v)); };
    // click a spot and report the last event it fired (the Rec ones)
    const auto hit = [&](int x, int y) { evs.clear(); fwd.clear(); clk(x, y); shot(); return evs.empty() ? Ev{} : evs.back(); };
    const auto hitFwd = [&](int x, int y) { evs.clear(); fwd.clear(); clk(x, y); shot(); return fwd.empty() ? Ev{} : fwd.back(); };
    const auto names = [&](const std::vector<Ev>& v) { std::string s; for (const Ev& e : v) { s += (s.empty() ? "" : ", ") + e.name + (e.args.empty() ? "" : " " + e.args); } return s; };
    // type into the field at (x, y): select its text away, type `text`, optionally Enter
    const auto typeInto = [&](int x, int y, const std::string& text, bool enter, int erase = 24) {
        evs.clear();
        fwd.clear();
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
    // the same on a ground other than the panel's (the notice box's)
    const auto findBoxOn = [&](const Frame& f, int x, int y0, int y1, bool ticked, Rgb ground) {
        for (int y = y0; y < y1; ++y) {
            if (f.near(x + 7, y - 1, ground, 0) && (ticked ? (f.near(x + 7, y, accent, 0) && f.near(x + 7, y + 5, accent, 0)) : (f.near(x + 7, y, lineStrong, 0) && f.near(x + 7, y + 4, field, 0)))) { return y; }
        }
        return -1;
    };
    // the tone of the text in each row of [y0, y1) inside [x0, x1): 'e' red, 'g' green, 'a' amber, 'd' dim, 'f' fg-soft/fg, '.' none; consecutive rows of one tone are one band
    const auto toneAt = [&](const Frame& f, int y, int x0, int x1) {
        const int ce = f.countNear(x0, y, x1, y + 1, errFg, 40), cg = f.countNear(x0, y, x1, y + 1, goodFg, 40), ca = f.countNear(x0, y, x1, y + 1, warnFg, 40);
        const int cd = f.countNear(x0, y, x1, y + 1, dim, 24), cf = f.countNear(x0, y, x1, y + 1, fg, 40);
        const int best = std::max({ce, cg, ca, cd, cf});
        if (best < 3) { return '.'; }
        return best == ce ? 'e' : best == cg ? 'g' : best == ca ? 'a' : best == cd ? 'd' : 'f';
    };
    const auto tones = [&](const Frame& f, int y0, int y1, int x0, int x1) {
        std::string s;
        char last = '.';
        int gap = 99;
        for (int y = y0; y < y1; ++y) {
            const char t = toneAt(f, y, x0, x1);
            if (t == '.') { ++gap; continue; }
            if (s.empty() || s.back() != t || gap > 7) { s += t; }
            last = t;
            gap = 0;
        }
        return s;
    };

    // =========================================================================================================
    // 1. The sheet at rest
    // =========================================================================================================
    park();
    const Frame f0 = shot();
    CHECK(f0.width == kW && f0.height == kH);
    CHECK(f0.near(1235, 3225, bg, 0));
    f0.savePng(shotDir() + "extend-specimen.png");
    // rule 11: a bare body in a 254 px cell asks for no more than 254 px, the longest translations included
    CHECKV(ui->get_p1_w() <= 254.01 && ui->get_p2_w() <= 254.01 && ui->get_p3_w() <= 254.01 && ui->get_p4_w() <= 254.01, "bare bodies in a 254 px cell: %.1f %.1f %.1f %.1f",
           ui->get_p1_w(), ui->get_p2_w(), ui->get_p3_w(), ui->get_p4_w());
    CHECKV(ui->get_probe_y() >= kH, "the probes sit below the window (%.0f >= %d)", ui->get_probe_y(), kH);

    // =========================================================================================================
    // 2. Web access: a1 (ON, network warning, error), a2 (REFUSED), a3 (OFF, a specific address, default-forwarding)
    // =========================================================================================================
    const int x0a = kColA + kBodyX, Ra = x0a + kBodyW;
    const int A1 = yOf(ui->get_a1_y()), T1 = A1 + kSecRow + kBodyPad;
    crop(f0, "crop-web-on.png", kColA, A1, 300, 520);
    {
        CHECKV(f0.near(kColA + 300, A1 + 60, bg, 0) && f0.near(kColA + 3, A1 + 60, panel, 0), "the Section is 296 wide on the column's background");
        CHECKV(f0.countNear(kColA + 200, A1 + 4, kColA + 290, A1 + 26, goodFg, 40) >= 12, "the Web access chip is green (ON)");
        CHECKV(f0.countNear(kColA + 28, A1 + 10, kColA + 40, A1 + 20, lampGreen, 0) >= 20, "the Web access lamp is lit green");
        // "Serve a browser page": a ticked 14 px box in the first row
        CHECKV(findBox(f0, x0a, T1 + 1, T1 + 12, true) == T1 + 5, "Serve a browser page: a ticked 14 px box centred in its row (top %d, want %d)", findBox(f0, x0a, T1 + 1, T1 + 12, true), T1 + 5);
        CHECKV(f0.countNear(x0a + 22, T1 + 5, x0a + 150, T1 + 19, fg, 60) >= 30, "Serve a browser page: its words");
        // Reachable from: the label at the left, the 150 px dropdown flush right
        CHECKV(dropAt(f0, T1 + kPitch, Ra, 150), "Reachable from: a 150 px dropdown flush right");
        CHECKV(f0.countNear(x0a, T1 + kPitch + 5, x0a + 98, T1 + kPitch + 19, fgSoft, 60) >= 25, "Reachable from: its label");
        CHECKV(f0.countInk(Ra - 140, T1 + kPitch + 5, Ra - 20, T1 + kPitch + 19, keyBg) >= 30, "Reachable from: the chosen word");
        // the Port (70 wide, right-aligned mono) beside the User name, captions above: two fields 24 high
        const std::vector<int> col = vruns(f0, x0a + 120, T1 + 2 * kPitch - 4, T1 + 330, keyLine);
        crop(f0, "crop-web-on-col.png", x0a, T1 + 55, 254, 260, 3);
        CHECKV(col.size() >= 8, "the boxes down the body: %s", list(col).c_str());
        const int cap = col.size() > 0 ? col[0] - (T1 + 2 * kPitch) : 0;
        CHECKV(cap >= 14 && cap <= 18, "the caption (11 px) and 2 px sit above the fields (%d)", cap);
        const int fy = T1 + 2 * kPitch + cap;
        CHECKV(box24(f0, x0a, fy, 70) && box24(f0, x0a + 76, fy, kBodyW - 76), "Port 70 wide beside User name, 6 px apart (fields at y %d)", fy);
        CHECKV(f0.countNear(x0a + 30, fy + 5, x0a + 64, fy + 19, fg, 60) >= 25, "Port: 8080 in the mono face");
        CHECKV(f0.countNear(x0a + 84, fy + 5, x0a + 140, fy + 19, fg, 60) >= 20, "User name: fox");
        CHECKV(f0.countNear(x0a, fy - cap + 1, x0a + 60, fy - 3, dim, 40) >= 20 && f0.countNear(x0a + 76, fy - cap + 1, x0a + 150, fy - 3, dim, 40) >= 25, "the two captions are dim");
        // the password status: green because a password is set
        const int sy = fy + kRow + 6;
        CHECKV(f0.countNear(x0a, sy, x0a + 140, sy + 14, goodFg, 40) >= 40, "A password is set: green");
        // Password: its caption, the masked field with the SHOW key (xs, 20 high) beside it
        const int py = sy + 14 + 6 + 14 + 2;
        CHECKV(box24(f0, x0a, py, 254 - 40) || f0.near(x0a + 12, py, keyLine, 0), "the Password field starts at the left edge (top %d)", py);
        CHECKV(f0.near(Ra - 1, py + 12, keyLine) && f0.near(Ra - 25, py + 2, keyLine) && f0.near(Ra - 25, py + 21, keyLine) && !f0.near(Ra - 25, py + 1, keyLine) && !f0.near(Ra - 25, py + 22, keyLine),
               "SHOW: a 20 px xs key flush right, centred on the field");
        CHECKV(f0.countInk(x0a + 8, py + 5, x0a + 100, py + 19, field) >= 40, "the masked draft is drawn as dots");
        CHECKV(f0.near(x0a + 6, py + 12, field, 0), "a field is the field ground");
        // Confirm: its caption and a full-width masked field
        const int cy = py + kRow + 6 + 14 + 2;
        CHECKV(box24(f0, x0a, cy, kBodyW), "Confirm: a full-width masked field (top %d)", cy);
        // Set password, Clear password, then Apply on the next line when the three do not fit 254 px, 24 high, 6 px apart
        const int ky = cy + kRow + 6;
        CHECKV(f0.near(x0a + 12, ky, keyLine) && f0.near(x0a + 12, ky + 23, keyLine) && f0.near(x0a, ky + 12, keyLine) && f0.near(x0a + 12, ky - 1, panel, 0), "Set password: a 24 px key at the left (top %d)", ky);
        CHECKV(f0.countNear(x0a + 6, ky + 5, x0a + 88, ky + 19, keyFg, 60) >= 40, "Set password: its words");
        const std::vector<int> kx = hruns(f0, ky + 12, x0a, Ra, keyLine);
        CHECKV(kx.size() >= 4, "Set password and Clear password are two keys on the line: %s", list(kx).c_str());
        // Apply: lit (accent), a 24 px key
        const int lastRow = ky + kPitch;
        const int applyLit = f0.countNear(x0a, ky, Ra, lastRow + 24, accent, 0);
        CHECKV(applyLit >= 500, "Apply is lit while there is something to apply (%d accent pixels)", applyLit);
        // the outcome lines, in order: red error, green serving note, red warning, the address, amber plain HTTP, the dim counts
        const std::string t = tones(f0, lastRow + 24, T1 + yOf(ui->get_a1_body_h()) - 4, x0a, Ra);
        CHECKV(t == "egedadd", "the outcome lines' tones: red error, green serving, red warning, dim address, amber plain HTTP, the two dim counts: '%s'", t.c_str());
        const int bodyEnd = A1 + kSecRow + yOf(ui->get_a1_body_h());
        CHECKV(bodyEnd - 12 - (T1 + yOf(ui->get_a1_body_h()) - 4) < 20, "the body ends under its last line (%d)", bodyEnd);

        // ---- the gestures (a1 overrides every callback, the specimen counts them) ----
        Ev e = hit(x0a + 7, T1 + 12);
        CHECKV(e.name == "web-serve" && e.args == "0", "Serve a browser page asks to turn it off: '%s' '%s'", e.name.c_str(), e.args.c_str());
        {
            // the dropdown: the list opens under the key, the chosen row is ticked, a pick asks web-reach(i)
            clk(Ra - 60, T1 + kPitch + 12);
            const Frame o = shot();
            const auto pr1 = popupRow(Ra - 150, T1 + kPitch, 1);
            CHECKV(o.countNear(Ra - 150 + 5, pr1.second - 7, Ra - 150 + 19, pr1.second + 7, accent, 60) >= 6, "the open list ticks the chosen row (Any computer on the network)");
            const auto pr = popupRow(Ra - 150, T1 + kPitch, 0);
            evs.clear();
            clk(pr.first, pr.second);
            shot();
            CHECKV(!evs.empty() && evs.back().name == "web-reach" && evs.back().args == "0", "picking This computer only: '%s'", names(evs).c_str());
        }
        typeInto(x0a + 35, fy + 12, "9090", true);
        CHECKV(evs.size() == 1 && evs[0].name == "web-port" && evs[0].args == "9090", "Enter commits the port: '%s'", names(evs).c_str());
        typeInto(x0a + 120, fy + 12, "bob", true);
        CHECKV(evs.size() == 1 && evs[0].name == "web-user" && evs[0].args == "bob", "Enter commits the user name: '%s'", names(evs).c_str());
        typeInto(x0a + 60, py + 12, "pw1", true);
        CHECKV(evs.size() == 1 && evs[0].name == "web-password" && evs[0].args == "pw1", "Enter commits the password: '%s'", names(evs).c_str());
        typeInto(x0a + 60, cy + 12, "pw1", true);
        CHECKV(evs.size() == 1 && evs[0].name == "web-confirm" && evs[0].args == "pw1", "Enter commits the confirmation: '%s'", names(evs).c_str());
        // SHOW lights and the dots become letters; HIDE puts the dots back
        {
            clk(x0a + 150, py + 12);                    // the keyboard goes to the Password field, so no caret blinks in Confirm
            park();
            const Frame before = shot();
            const int dots = before.countInk(x0a + 8, py + 5, x0a + 200, py + 19, field);
            clk(Ra - 25, py + 12);
            mv(1235, 3225);
            settle(milliseconds(30));
            const Frame shown = shot();
            CHECKV(shown.countNear(Ra - 40, py + 2, Ra - 2, py + 22, accent, 0) >= 400, "SHOW is lit while the password shows (%d)", shown.countNear(Ra - 40, py + 2, Ra - 2, py + 22, accent, 0));
            CHECKV(shown.countInk(x0a + 8, py + 5, x0a + 200, py + 19, field) != dots, "the dots are letters now (%d, was %d)", shown.countInk(x0a + 8, py + 5, x0a + 200, py + 19, field), dots);
            CHECKV(shown.countNear(Ra - 40, py + 2, Ra - 2, py + 22, accentInk, 60) >= 15, "HIDE: its words, in accent-ink");
            clk(Ra - 25, py + 12);
            mv(1235, 3225);
            settle(milliseconds(30));
            const Frame hidden = shot();
            CHECKV(hidden.countInk(x0a + 8, py + 5, x0a + 200, py + 19, field) == dots && hidden.countNear(Ra - 40, py + 2, Ra - 2, py + 22, accent, 0) == 0, "HIDE puts the dots back");
            CHECKV(hidden.countInk(x0a + 8, cy + 5, x0a + 200, cy + 19, field) != shown.countInk(x0a + 8, cy + 5, x0a + 200, cy + 19, field), "Confirm shows and hides with the Password, so the two can be compared");
        }
        // Set password commits the typed draft first, then asks
        typeInto(x0a + 60, py + 12, "abc", false);
        evs.clear();
        clk(x0a + 40, ky + 12);
        shot();
        CHECKV(evs.size() == 2 && evs[0].name == "web-password" && evs[0].args == "abc" && evs[1].name == "web-set-password", "Set password commits the draft, then sets: '%s'", names(evs).c_str());
        e = hit((kx.size() >= 4 ? (kx[2] + kx[3]) / 2 : x0a + 130), ky + 12);
        CHECKV(e.name == "web-clear-password", "Clear password: '%s'", e.name.c_str());
        typeInto(x0a + 35, fy + 12, "9091", false);
        evs.clear();
        clk(x0a + 20, lastRow + 12);
        shot();
        CHECKV(evs.size() == 2 && evs[0].name == "web-port" && evs[0].args == "9091" && evs[1].name == "web-apply", "Apply commits the typed port, then applies: '%s'", names(evs).c_str());
        park();
    }
    // a2: refused - the red lamp and chip, no password, no Clear key, Apply greyed as "Applied", the refusal in red and nothing else
    const int A2 = yOf(ui->get_a2_y()), T2 = A2 + kSecRow + kBodyPad;
    const int x0b = kColB + kBodyX, Rb = x0b + kBodyW;
    {
        const Frame f = shot();
        crop(f, "crop-web-refused.png", kColB, A2, 300, 400);
        CHECKV(f.countNear(kColB + 200, A2 + 4, kColB + 290, A2 + 26, errFg, 40) >= 12, "the REFUSED chip is red");
        CHECKV(f.countNear(kColB + 28, A2 + 10, kColB + 40, A2 + 20, lampRed, 0) >= 20, "the Web access lamp is lit red");
        CHECKV(findBox(f, x0b, T2 + 1, T2 + 12, true) == T2 + 5, "Serve a browser page stays ticked: it was switched on and refused");
        CHECKV(dropAt(f, T2 + kPitch, Rb, 150), "This computer only: the same 150 px dropdown");
        const int fy2 = T2 + 2 * kPitch + 16;
        CHECKV(f.countNear(x0b, fy2 + kRow + 6, x0b + 140, fy2 + kRow + 6 + 14, dim, 24) >= 40 && f.countNear(x0b, fy2 + kRow + 6, x0b + 140, fy2 + kRow + 6 + 14, goodFg, 40) == 0, "No password set: dim, not green");
        // the key row: Set password and the greyed Applied; no Clear
        const int ky2 = fy2 + kRow + 6 + 14 + 6 + 14 + 2 + kRow + 6 + 14 + 2 + kRow + 6;
        CHECKV(f.near(x0b + 12, ky2, keyLine) && f.near(x0b + 12, ky2 + 23, keyLine), "Set password: a 24 px key (top %d)", ky2);
        const std::vector<int> kx2 = hruns(f, ky2 + 12, x0b, Rb, keyLine);
        CHECKV(kx2.size() == 2, "only Set password is a plain key on the line (Clear password is not drawn): %s", list(kx2).c_str());
        const int ax = kx2.size() >= 2 ? kx2[1] + 7 : x0b + 100;
        CHECKV(f.near(ax + 12, ky2, dis, 6) && f.near(ax + 3, ky2 + 12, panel, 0), "Applied: a greyed key, the panel ground in a half-alpha border");
        CHECKV(f.countNear(x0b, ky2, Rb, ky2 + 24, accent, 0) == 0, "Applied is not lit");
        CHECKV(f.countNear(ax + 6, ky2 + 5, ax + 60, ky2 + 19, keyFg, 60) < 5 && f.countInk(ax + 6, ky2 + 5, ax + 60, ky2 + 19, panel) >= 20, "Applied: its words, in the disabled ink");
        const std::string t2 = tones(f, ky2 + 26, T2 + yOf(ui->get_a2_body_h()) - 4, x0b, Rb);
        CHECKV(t2 == "e", "the refusal is the one line under the keys, red: '%s'", t2.c_str());
        evs.clear();
        clk(ax + 20, ky2 + 12);
        shot();
        CHECKV(evs.empty(), "a greyed Applied does not answer: '%s'", names(evs).c_str());
        park();
    }
    // a3: off, "A specific address" with its address readout; plain, default-forwarding to ExtendEvents
    const int A3 = yOf(ui->get_a3_y()), T3 = A3 + kSecRow + kBodyPad;
    {
        const Frame f = shot();
        crop(f, "crop-web-off.png", kColA, A3, 300, 420);
        CHECKV(f.countNear(kColA + 200, A3 + 4, kColA + 290, A3 + 26, goodFg, 40) < 5 && f.countInk(kColA + 200, A3 + 4, kColA + 290, A3 + 26, panel) >= 30, "the OFF chip is the neutral one");
        CHECKV(findBox(f, x0a, T3 + 1, T3 + 12, false) == T3 + 5, "Serve a browser page: an empty box when off");
        CHECKV(dropAt(f, T3 + kPitch, Ra, 150), "A specific address: the same 150 px dropdown");
        // the address readout: the word at the left, the mono address at the right, one row of 24
        CHECKV(f.countNear(x0a, T3 + 2 * kPitch + 5, x0a + 60, T3 + 2 * kPitch + 19, dim, 24) >= 15, "address: its word, dim");
        CHECKV(f.countNear(Ra - 100, T3 + 2 * kPitch + 5, Ra, T3 + 2 * kPitch + 19, fg, 60) >= 40, "the address in the mono face at the right");
        const std::string t3 = tones(f, T3 + 3 * kPitch + 70, T3 + yOf(ui->get_a3_body_h()) - 4, x0a, Ra);
        CHECKV(t3.find('e') == std::string::npos, "nothing red when off: '%s'", t3.c_str());
        fwd.clear();
        Ev g = hitFwd(x0a + 7, T3 + 12);
        CHECKV(g.name == "web-serve" && g.args == "1", "Serve a browser page forwards to ExtendEvents: '%s' '%s'", g.name.c_str(), g.args.c_str());
        clk(Ra - 60, T3 + kPitch + 12);
        shot();
        const auto pr = popupRow(Ra - 150, T3 + kPitch, 1);
        fwd.clear();
        clk(pr.first, pr.second);
        shot();
        CHECKV(fwd.size() == 1 && fwd[0].name == "web-reach" && fwd[0].args == "1", "the dropdown forwards web-reach: '%s'", names(fwd).c_str());
        park();
    }

    // =========================================================================================================
    // 3. CAT control: b1 (1 CLIENT, accept ticked, reachable ticked, the amber warning, the green status), b2 (FAILED, plain)
    // =========================================================================================================
    const int B1 = yOf(ui->get_b1_y()), TB1 = B1 + kSecRow + kBodyPad;
    {
        const Frame f = shot();
        crop(f, "crop-cat-ok.png", kColB, B1, 300, 300);
        CHECKV(f.countNear(kColB + 200, B1 + 4, kColB + 290, B1 + 26, goodFg, 40) >= 12, "the 1 CLIENT chip is green");
        CHECKV(f.countNear(kColB + 28, B1 + 10, kColB + 40, B1 + 20, lampGreen, 0) >= 20, "the CAT lamp is lit green");
        // the intro, dim, wrapped
        CHECKV(f.countNear(x0b, TB1, Rb, TB1 + 60, dim, 24) >= 150, "the intro is dim text");
        const int accept = findBox(f, x0b, TB1 + 20, TB1 + 120, true);
        CHECKV(accept > 0, "Accept CAT connections: a ticked box (top %d)", accept);
        const int arow = accept - 5;
        CHECKV(f.countNear(x0b + 22, arow + 5, x0b + 200, arow + 19, fg, 60) >= 40, "Accept CAT connections: its words");
        // Port: the label at the left, the 80 px mono field flush right, right-aligned
        const int prow = arow + kPitch;
        CHECKV(box24(f, Rb - 80, prow, 80), "Port: an 80 px field flush right (row top %d)", prow);
        CHECKV(f.countNear(x0b, prow + 5, x0b + 40, prow + 19, fgSoft, 60) >= 15, "Port: its label");
        CHECKV(f.countNear(Rb - 60, prow + 5, Rb - 6, prow + 19, fg, 60) >= 25 && f.countNear(Rb - 78, prow + 5, Rb - 60, prow + 19, fg, 60) == 0, "4532, right-aligned in the mono face");
        const int reach = findBox(f, x0b, prow + kPitch, prow + kPitch + 12, true);
        CHECKV(reach == prow + kPitch + 5, "Reachable from other machines: a ticked box in the next row (%d)", reach);
        const std::string tc = tones(f, prow + 2 * kPitch, B1 + kSecRow + yOf(ui->get_b1_body_h()) - 4, x0b, Rb);
        CHECKV(tc == "ag", "the amber warning, then the green status: '%s'", tc.c_str());
        // gestures
        Ev e = hit(x0b + 7, arow + 12);
        CHECKV(e.name == "cat-accept" && e.args == "0", "Accept CAT connections asks to stop: '%s' '%s'", e.name.c_str(), e.args.c_str());
        typeInto(Rb - 40, prow + 12, "5000", true);
        CHECKV(evs.size() == 1 && evs[0].name == "cat-port" && evs[0].args == "5000", "Enter commits the CAT port: '%s'", names(evs).c_str());
        e = hit(x0b + 7, prow + kPitch + 12);
        CHECKV(e.name == "cat-reachable" && e.args == "0", "Reachable from other machines asks to close: '%s' '%s'", e.name.c_str(), e.args.c_str());
        park();
    }
    const int B2 = yOf(ui->get_b2_y()), TB2 = B2 + kSecRow + kBodyPad;
    {
        const Frame f = shot();
        crop(f, "crop-cat-failed.png", kColB, B2, 300, 250);
        CHECKV(f.countNear(kColB + 200, B2 + 4, kColB + 290, B2 + 26, errFg, 40) >= 12, "the FAILED chip is red");
        CHECKV(f.countNear(kColB + 28, B2 + 10, kColB + 40, B2 + 20, lampRed, 0) >= 20, "the CAT lamp is lit red");
        const int accept = findBox(f, x0b, TB2 + 20, TB2 + 100, true);
        CHECKV(accept > 0, "Accept CAT connections stays ticked");
        const int reach = findBox(f, x0b, accept + 50, accept + 70, false);
        CHECKV(reach == accept + 2 * kPitch, "Reachable from other machines: an empty box (%d, want %d)", reach, accept + 2 * kPitch);
        const std::string tc = tones(f, reach + 20, B2 + kSecRow + yOf(ui->get_b2_body_h()) - 4, x0b, Rb);
        CHECKV(tc == "e", "no amber warning, the failure in red: '%s'", tc.c_str());
        fwd.clear();
        Ev g = hitFwd(x0b + 7, reach - 5 + 12);
        CHECKV(g.name == "cat-reachable" && g.args == "1", "Reachable from other machines forwards to ExtendEvents: '%s' '%s'", g.name.c_str(), g.args.c_str());
        park();
    }

    // =========================================================================================================
    // 4. The store page
    // =========================================================================================================
    // the top bar of any state: the strip ground and its rule, the search field (220 x 24), the tab group at 12 + 220 + 12, the catalogue line, the keys under it
    const auto topBar = [&](const Frame& f, int px, int py, int pw, int bar, bool browseLit, const char* what) {
        CHECKV(bar == 100, "%s: the top bar is 8 + 32 + 8 + a line and 6 + 24 + 8 = 100 high (%d)", what, bar);
        CHECKV(f.near(px + pw / 2, py + 4, strip, 0) && f.near(px + pw / 2, py + bar - 1, line, 0) && f.near(px + pw / 2, py + bar + 2, panel, 0) && f.near(px + pw / 2, py + bar - 2, strip, 0),
               "%s: the strip ground with its rule under it, the panel below", what);
        CHECKV(f.near(px + 12 + 100, py + 12, lineStrong, 0) && f.near(px + 12 + 100, py + 12 + 23, lineStrong, 0) && f.near(px + 12, py + 24, lineStrong, 0) && f.near(px + 12 + 219, py + 24, lineStrong, 0)
                   && f.near(px + 12 + 100, py + 11, strip, 0) && f.near(px + 12 + 100, py + 36, strip, 0) && f.near(px + 12 + 220, py + 24, strip, 0) && f.near(px + 12 + 100, py + 18, field, 0),
               "%s: the search field is 220 x 24 on the field ground", what);
        CHECKV(f.countInk(px + 12 + 6, py + 16, px + 12 + 24, py + 32, field) >= 12, "%s: the loupe", what);
        const int gx = px + 12 + 220 + 12;
        CHECKV(f.near(gx + 40, py + 8, lineStrong, 0) && f.near(gx + 40, py + 8 + 31, lineStrong, 0) && f.near(gx + 40, py + 7, strip, 0), "%s: the tab group is 32 high (SegGroup)", what);
        const int left = f.countNear(gx + 3, py + 11, gx + 80, py + 37, accent, 0), right = f.countNear(gx + 100, py + 11, gx + 180, py + 37, accent, 0);
        CHECKV(browseLit ? (left >= 1000 && right == 0) : (left == 0 && right >= 1000), "%s: %s is the latched tab (%d / %d)", what, browseLit ? "BROWSE" : "UPDATES (2)", left, right);
        CHECKV(f.countInk(px + 12, py + 48, px + 400, py + 62, strip) >= 100, "%s: the catalogue line", what);
        const int ky = py + bar - 8 - 24;
        CHECKV(f.near(px + 12 + 12, ky, keyLine, 0) && f.near(px + 12 + 12, ky + 23, keyLine, 0) && f.near(px + 12, ky + 12, keyLine, 0) && f.near(px + 12 + 12, ky - 1, strip, 0) && f.near(px + 12 + 12, ky + 24, strip, 0),
               "%s: CHECK AGAIN is a 24 px key on the strip, 8 px from its foot", what);
    };

    // ---------- Browse: 1000 x 700 ----------
    const int bx = kStX, by = kBrowseY;
    const int barB = yOf(ui->get_browse_bar_h());
    std::vector<int> bTop, bBot;                                   // the first row's cards: left and right border columns
    int c0t = 0, c0b = 0, vorT = 0, aisT = 0, aisB = 0;
    {
        const Frame f = shot();
        crop(f, "crop-st-browse.png", bx, by, kStW, kStH, 1);
        topBar(f, bx, by, kStW, barB, true, "Browse");
        CHECKV(ui->get_browse_cols() == 3 && std::fabs(ui->get_browse_card_w() - (976.0 - 12.0) / 3.0) < 0.01, "a 1000 px page lays three cards to a row (%d, %.2f)", ui->get_browse_cols(), ui->get_browse_card_w());
        // the top lines: the install error in red, the catalogue warning in amber
        CHECKV(tones(f, by + barB + 4, by + barB + 44, bx + 12, bx + 700) == "ea", "the install error is red, the catalogue warning amber: '%s'", tones(f, by + barB + 4, by + barB + 44, bx + 12, bx + 700).c_str());
        // the cards down the first column: a top and a bottom border each, 6 px between cards of a category
        const std::vector<int> col = vruns(f, bx + 230, by + barB + 4, by + kStH, lineStrong);
        CHECKV(col.size() == 6, "three cards in the first column (aircraft: ADS-B and VOR, marine: AIS), two borders each: %s", list(col).c_str());
        if (col.size() == 6) {
            c0t = col[0]; c0b = col[1]; vorT = col[2]; aisT = col[4]; aisB = col[5];
            CHECKV(col[2] - col[1] - 1 == 6, "the next row of cards is 6 px under the first (%d)", col[2] - col[1] - 1);
            CHECKV(c0b - c0t + 1 >= 100 && c0b - c0t + 1 <= 130, "an ADS-B card with badges is about 115 high (%d)", c0b - c0t + 1);
            CHECKV(col[3] - col[2] + 1 >= 85 && col[3] - col[2] + 1 <= c0b - c0t + 1, "a card without a version note is shorter or equal (%d)", col[3] - col[2] + 1);
            CHECKV(col[4] - col[3] > 6 + 6, "a category heading and the grid's spacing sit between the categories (%d)", col[4] - col[3] - 1);
        }
        // three columns: six vertical borders along the middle of the first row, the cards 321 wide, 6 apart, the page's inner width 976 held to its last pixel
        bTop = hruns(f, c0t + 74, bx, bx + kStW, lineStrong);
        const std::vector<int> hr = hruns(f, c0t + 74, bx, bx + kStW, lineStrong);
        CHECKV(hr.size() == 6, "six card borders along the first row: %s", list(hr).c_str());
        if (hr.size() == 6) {
            CHECKV(hr[0] == bx + 12 && hr[5] == bx + 12 + 976 - 1, "the cards span the page's inner width exactly (%d..%d)", hr[0], hr[5]);
            CHECKV(hr[1] - hr[0] >= 320 && hr[1] - hr[0] <= 322 && hr[3] - hr[2] >= 320 && hr[3] - hr[2] <= 322 && hr[5] - hr[4] >= 320 && hr[5] - hr[4] <= 322, "each card is 321 wide: %d %d %d", hr[1] - hr[0], hr[3] - hr[2], hr[5] - hr[4]);
            CHECKV(hr[2] - hr[1] - 1 >= 5 && hr[2] - hr[1] - 1 <= 7 && hr[4] - hr[3] - 1 >= 5 && hr[4] - hr[3] - 1 <= 7, "6 px between them: %d %d", hr[2] - hr[1] - 1, hr[4] - hr[3] - 1);
            // the three cards of the first row are one height: the tallest sets it
            CHECKV(f.near(hr[2] + 150, c0t, lineStrong, 0) && f.near(hr[2] + 150, c0b, lineStrong, 0) && f.near(hr[4] + 150, c0t, lineStrong, 0) && f.near(hr[4] + 150, c0b, lineStrong, 0),
                   "the cards of a row share their top and bottom edges");
            // the VOR card in the next row holds the column width; there is no card beside it
            const std::vector<int> hv = hruns(f, vorT + 40, bx, bx + kStW, lineStrong);
            CHECKV(hv.size() == 2 && hv[0] == hr[0] && hv[1] - hv[0] >= 320 && hv[1] - hv[0] <= 322, "the partly filled row keeps the column width (VOR): %s", list(hv).c_str());
            // the glyph tile: 30 x 30, 8 px in; the glyph in the soft accent
            CHECKV(f.near(hr[0] + 8 + 15, c0t + 8, lineStrong, 0) && f.near(hr[0] + 8 + 15, c0t + 8 + 29, lineStrong, 0) && f.near(hr[0] + 8, c0t + 8 + 15, lineStrong, 0) && f.near(hr[0] + 8 + 29, c0t + 8 + 15, lineStrong, 0)
                       && f.near(hr[0] + 8 + 4, c0t + 8 + 4, keyBg, 0),
                   "the glyph tile is a 30 px square 8 px in");
            CHECKV(f.countNear(hr[0] + 12, c0t + 12, hr[0] + 34, c0t + 34, accentSoft, 40) >= 25, "the glyph in the soft accent");
            // the name (semibold, fg), the summary (dim), the badges: a warn chip and a neutral one
            CHECKV(f.countNear(hr[0] + 46, c0t + 6, hr[0] + 250, c0t + 22, fg, 60) >= 60, "the card's name");
            CHECKV(f.countNear(hr[0] + 46, c0t + 24, hr[0] + 310, c0t + 38, dim, 24) >= 60, "the card's summary is dim");
            CHECKV(f.countNear(hr[0] + 46, c0t + 42, hr[0] + 130, c0t + 62, warnBg, 0) >= 150 && f.countNear(hr[0] + 130, c0t + 42, hr[0] + 220, c0t + 62, chipBg, 0) >= 150, "the NETWORK chip is amber, 1090 MHz the neutral one");
            // the UPDATE key: lit, 24 high, 8 px from the card's corner; the amber version note beside it
            const int kt = c0b + 1 - 8 - 24, kr = hr[1] - 8;
            CHECKV(f.countNear(kr - 70, kt + 1, kr, kt + 23, accent, 0) >= 800, "UPDATE is the lit key");
            CHECKV(f.near(kr, kt + 12, accent, 0) && f.near(kr + 1, kt + 12, card, 40) && f.near(kr - 20, kt - 1, card, 40), "UPDATE: a 24 px key with its right edge 8 px inside the card");
            CHECKV(f.countNear(hr[0] + 46, kt + 4, hr[0] + 140, kt + 20, warnFg, 40) >= 20, "1.9.1 > 1.9.2: the amber version note (%d)", f.countNear(hr[0] + 46, kt + 4, hr[0] + 140, kt + 20, warnFg, 40));
            // INSTALLED (card 2): greyed, the card ground in a half-alpha border
            CHECKV(f.near(hr[3] - 8 - 30, kt, dis, 8) && f.near(hr[3] - 8 - 30, kt + 3, card, 12), "INSTALLED: a greyed key");
            // FITTING... (card 3): the bar 60 % full beside a greyed key, the progress line under
            const int fitBot = c0b;
            int fill = 0, track = 0, barY = -1;
            for (int y = c0t + 40; y < fitBot && barY < 0; ++y) {
                if (f.near(hr[4] + 100, y, sliderFill, 0)) { barY = y; }
            }
            if (barY > 0) {
                fill = f.countNear(hr[4] + 46, barY, hr[4] + 321 - 8 - 70, barY + 1, sliderFill, 0);
                track = f.countNear(hr[4] + 46, barY, hr[4] + 321 - 8 - 70, barY + 1, sliderTrack, 0);
            }
            CHECKV(barY > 0 && fill > 0 && track > 0 && std::fabs(double(fill) / double(fill + track) - 0.6) < 0.06, "FITTING...: the bar is 60 %% full (%d of %d, row %d)", fill, fill + track, barY);
            CHECKV(barY > 0 && f.near(hr[4] + 100, barY + 5, sliderFill, 0) && !f.near(hr[4] + 100, barY + 6, sliderFill, 0) && !f.near(hr[4] + 100, barY - 1, sliderFill, 0), "the bar is 6 px thick");
            CHECKV(f.countNear(hr[4] + 46, c0b - 22, hr[4] + 200, c0b - 7, dim, 24) >= 30, "Fitting 2 of 5: the progress line, dim, under the key row");
            // the marine card: its own column-width card, a greyed GET
            const std::vector<int> ha = hruns(f, aisT + 40, bx, bx + kStW, lineStrong);
            CHECKV(ha.size() == 2 && ha[0] == hr[0] && ha[1] - ha[0] >= 320 && ha[1] - ha[0] <= 322, "the AIS card is one column wide, in the first column: %s", list(ha).c_str());
            CHECKV(f.near(hr[0] + 46, aisB - 4, card, 40) && f.near(hr[0] + 300, aisB - 28, dis, 8) == false, "the greyed GET is drawn in the AIS card");
        }
        // the foot: GET EVERYTHING (greyed, its reason beside it), the bulk tick and its sentence, the progress note
        int footKey = -1;
        for (int y = aisB + 2; y < by + kStH; ++y) { if (f.near(bx + 12 + 20, y, dis, 8) && f.near(bx + 12 + 20, y - 1, panel, 0)) { footKey = y; break; } }
        CHECKV(footKey == aisB + 1 + 12, "GET EVERYTHING sits 12 px under the last card, greyed (%d, want %d)", footKey, aisB + 13);
        CHECKV(f.countNear(bx + 12 + 125, footKey, bx + 12 + 400, footKey + 24, dim, 24) >= 40, "its reason beside it, dim");
        const int tickY = findBox(f, bx + 12, footKey + 26, footKey + 60, false);
        CHECKV(tickY == footKey + 24 + 8 + 1, "the notice tick is 8 px under the key, empty (%d)", tickY);
        CHECKV(f.countNear(bx + 12 + 22, tickY - 1, bx + 12 + 380, tickY + 15, fg, 60) >= 100, "I have read the notices ...: the sentence");
        CHECKV(f.countNear(bx + 12, tickY + 22, bx + 12 + 400, tickY + 40, dim, 24) >= 60, "the progress note is dim");
        // ---- the gestures ----
        Ev e;
        if (hr.size() == 6) {
            const int kt = c0b + 1 - 8 - 24, kr = hr[1] - 8;
            e = hit(kr - 30, kt + 12);
            CHECKV(e.name == "store-card-key" && e.args == "adsb", "the UPDATE key asks for its plugin: '%s' '%s'", e.name.c_str(), e.args.c_str());
            e = hit(hr[0] + 100, c0t + 14);
            CHECKV(e.name == "store-card-open" && e.args == "adsb", "a click on the card opens its page: '%s' '%s'", e.name.c_str(), e.args.c_str());
            e = hit(hr[2] + 100, c0t + 14);
            CHECKV(e.name == "store-card-open" && e.args == "acars", "the second column's card: '%s' '%s'", e.name.c_str(), e.args.c_str());
            e = hit(hr[3] - 8 - 30, kt + 12);
            CHECKV(e.name != "store-card-key", "INSTALLED is an indicator: it asks for nothing ('%s')", e.name.c_str());
            e = hit(hr[5] - 8 - 30, kt + 12);
            CHECKV(e.name != "store-card-key", "FITTING... is an indicator: it asks for nothing ('%s')", e.name.c_str());
            const int vorB = col[3], vorKey = vorB + 1 - 8 - 24;
            e = hit(hr[1] - 8 - 15, vorKey + 12);
            CHECKV(e.name == "store-card-key" && e.args == "vor", "the GET key of the next row: '%s' '%s'", e.name.c_str(), e.args.c_str());
            const int aisKey = aisB + 1 - 8 - 24;
            e = hit(hr[1] - 8 - 15, aisKey + 12);
            CHECKV(e.name != "store-card-key", "a GET that cannot be taken is greyed and asks for nothing ('%s')", e.name.c_str());
        }
        e = hit(bx + 12 + 125 - 60, footKey + 12);
        CHECKV(e.name.empty(), "a greyed GET EVERYTHING does not answer: '%s'", e.name.c_str());
        e = hit(bx + 12 + 7, tickY + 7);
        CHECKV(e.name == "store-bulk-tick" && e.args == "1", "the notice tick asks to tick: '%s' '%s'", e.name.c_str(), e.args.c_str());
        e = hit(bx + 12 + 200, tickY + 7);
        CHECKV(e.name == "store-bulk-tick" && e.args == "1", "the sentence is a click target as well: '%s' '%s'", e.name.c_str(), e.args.c_str());
        // the top bar
        e = hit(bx + 12 + 220 + 12 + 150, by + 24);
        CHECKV(e.name == "store-tab" && e.args == "1", "UPDATES (2): '%s' '%s'", e.name.c_str(), e.args.c_str());
        e = hit(bx + 12 + 220 + 12 + 40, by + 24);
        CHECKV(e.name == "store-tab" && e.args == "0", "BROWSE: '%s' '%s'", e.name.c_str(), e.args.c_str());
        e = hit(bx + 12 + 40, by + barB - 8 - 12);
        CHECKV(e.name == "store-check", "CHECK AGAIN: '%s'", e.name.c_str());
        const std::vector<int> kr = hruns(f, by + barB - 8 - 12, bx, bx + 400, keyLine);
        e = hit(kr.size() >= 4 ? (kr[2] + kr[3]) / 2 : bx + 160, by + barB - 8 - 12);
        CHECKV(e.name == "store-sites-toggle", "SITES: '%s' (keys at %s)", e.name.c_str(), list(kr).c_str());
        typeInto(bx + 12 + 100, by + 24, "adsb", false);
        CHECKV(evs.size() == 4 && evs.back().name == "store-search" && evs.back().args == "adsb" && evs[0].args == "a", "the search asks as it is typed: '%s'", names(evs).c_str());
        park();
    }

    // ---------- Updates: 1000 x 700, with the clean-up confirm open ----------
    const int ux = kStX, uy = kUpdatesY;
    const int barU = yOf(ui->get_updates_bar_h());
    {
        const Frame f = shot();
        crop(f, "crop-st-updates.png", ux, uy, kStW, kStH, 1);
        topBar(f, ux, uy, kStW, barU, false, "Updates");
        const int hy = uy + barU + 12;
        CHECKV(f.countNear(ux + 12, hy + 4, ux + 200, hy + 20, fg, 60) >= 60, "2 updates available: the heading");
        CHECKV(f.countNear(ux + 988 - 110, hy + 1, ux + 988, hy + 23, accent, 0) >= 900 && f.near(ux + 987, hy + 12, accent, 0) && f.near(ux + 988, hy + 12, panel, 0), "UPDATE ALL: a lit 24 px key flush right");
        // the rows: a bordered panel each, 44 high, 8 px apart
        const std::vector<int> rr = vruns(f, ux + 12 + 600, hy + 24, uy + 380, lineStrong);
        CHECKV(rr.size() == 4, "two update rows, two borders each: %s", list(rr).c_str());
        if (rr.size() == 4) {
            const int r1 = rr[0], r2 = rr[2];
            CHECKV(rr[1] - rr[0] + 1 >= 40 && rr[1] - rr[0] + 1 <= 46 && rr[3] - rr[2] + 1 >= 40 && rr[3] - rr[2] + 1 <= 46, "the rows are 44 high: %d %d", rr[1] - rr[0] + 1, rr[3] - rr[2] + 1);
            CHECKV(rr[2] - rr[1] - 1 == 8, "8 px between the rows (%d)", rr[2] - rr[1] - 1);
            CHECKV(r1 == hy + 24 + 8, "the first row is 8 px under the heading line (%d, want %d)", r1, hy + 32);
            CHECKV(f.near(ux + 12 + 8 + 12, r1 + 6, lineStrong, 0) && f.near(ux + 12 + 8 + 12, r1 + 6 + 23, lineStrong, 0) && f.near(ux + 12 + 8, r1 + 6 + 12, lineStrong, 0), "the glyph tile is 24 px, 8 px in");
            CHECKV(f.countNear(ux + 12 + 40, r1 + 6, ux + 12 + 170, r1 + 22, fg, 60) >= 60, "the plugin's name");
            CHECKV(f.countNear(ux + 12 + 175, r1 + 6, ux + 12 + 240, r1 + 22, warnFg, 40) >= 25, "1.9.1 > 1.9.2: the amber version");
            CHECKV(f.countNear(ux + 12 + 240, r1 + 3, ux + 12 + 290, r1 + 21, warnBg, 0) >= 120, "NEW: the amber badge");
            CHECKV(f.countNear(ux + 12 + 40, r1 + 24, ux + 12 + 200, r1 + 38, fgSoft, 40) >= 25, "faster decode: the what's-new line");
            // the row keys: xs (20 high), the first plain, the second greyed
            const int kr = ux + 988 - 8 - 1;
            CHECKV(f.near(kr, r1 + 6 + 10, keyLine, 0) && f.near(kr - 20, r1 + 6, keyLine, 0) && f.near(kr - 20, r1 + 6 + 19, keyLine, 0) && !f.near(kr - 20, r1 + 6 + 20, keyLine, 0), "UPDATE: an xs key, 8 px inside the row");
            CHECKV(f.near(kr - 20, r2 + 6, dis, 8), "the second UPDATE is greyed");
            // the footnotes, then CLEAN UP OLD VERSIONS (2) as an xs key with its lines under it
            const int fnY = rr[3] + 1 + 8;
            CHECKV(f.countNear(ux + 12, fnY, ux + 12 + 300, fnY + 30, dim, 24) >= 100, "the footnotes are dim");
            int cleanT = -1;
            for (int y = fnY + 20; y < fnY + 80; ++y) { if (f.near(ux + 12 + 12, y, keyLine, 0) && f.near(ux + 12 + 12, y - 1, panel, 0)) { cleanT = y; break; } }
            CHECKV(cleanT > 0 && f.near(ux + 12 + 12, cleanT + 19, keyLine, 0) && f.near(ux + 12, cleanT + 10, keyLine, 0), "CLEAN UP OLD VERSIONS (2): an xs key (top %d)", cleanT);
            CHECKV(f.countNear(ux + 12, cleanT + 24, ux + 12 + 400, cleanT + 60, dim, 24) >= 100, "its old copies are listed under it, dim");
            // gestures
            Ev e = hit(ux + 988 - 50, hy + 12);
            CHECKV(e.name == "store-update-all", "UPDATE ALL: '%s'", e.name.c_str());
            e = hit(kr - 10, r1 + 6 + 10);
            CHECKV(e.name == "store-update" && e.args == "adsb", "the row's UPDATE: '%s' '%s'", e.name.c_str(), e.args.c_str());
            e = hit(kr - 10, r2 + 6 + 10);
            CHECKV(e.name.empty(), "a greyed UPDATE asks for nothing: '%s'", e.name.c_str());
            e = hit(ux + 12 + 80, r1 + 14);
            CHECKV(e.name == "store-card-open" && e.args == "adsb", "the name opens the plugin's page: '%s' '%s'", e.name.c_str(), e.args.c_str());
            e = hit(ux + 12 + 70, cleanT + 10);
            CHECKV(e.name == "store-cleanup", "CLEAN UP OLD VERSIONS: '%s'", e.name.c_str());
            // the confirm: 290 wide, 8 px from the page's right and bottom edges, clear of everything above it
            const std::vector<int> mt = vruns(f, ux + 850, uy + barU + 4, uy + kStH, knobRing);
            CHECKV(mt.size() == 2, "the confirm's two borders down its middle: %s", list(mt).c_str());
            if (mt.size() == 2) {
                CHECKV(mt[1] == uy + kStH - 8 - 1, "the confirm ends 8 px above the page's foot (%d)", mt[1]);
                CHECKV(f.near(ux + kStW - 8 - 1, (mt[0] + mt[1]) / 2, knobRing, 0) && f.near(ux + kStW - 8 - 290, (mt[0] + mt[1]) / 2, knobRing, 0) && !f.near(ux + kStW - 8 - 291, (mt[0] + mt[1]) / 2, knobRing, 0), "the confirm is 290 wide, 8 px from the right edge");
                CHECKV(f.near(ux + 850, mt[0] + 4, menu, 0), "the confirm is the menu ground");
                CHECKV(mt[0] > uy + barU + static_cast<int>(ui->get_updates_content_h()), "it does not hide any control (top %d, the content ends at %d)", mt[0], uy + barU + static_cast<int>(ui->get_updates_content_h()));
                CHECKV(f.countNear(ux + 712, mt[0] + 8, ux + 960, mt[0] + 26, fg, 60) >= 60, "Remove old plugin versions: the title");
                const int kt = mt[1] - 10 - 24 + 1;
                CHECKV(f.countNear(ux + 712, kt + 1, ux + 820, kt + 23, accent, 0) >= 1200, "Remove 2 files: the lit key");
                int rx = ux + 712;
                while (rx < ux + 900 && !f.near(rx, kt + 3, accent, 0)) { ++rx; }
                while (rx < ux + 900 && f.near(rx, kt + 3, accent, 0)) { ++rx; }
                e = hit(ux + 712 + 20, kt + 12);
                CHECKV(e.name == "store-cleanup-confirm" && e.args == "1", "Remove 2 files: '%s' '%s'", e.name.c_str(), e.args.c_str());
                e = hit(rx + 6 + 25, kt + 12);
                CHECKV(e.name == "store-cleanup-confirm" && e.args == "0", "Keep them: '%s' '%s' (key at %d)", e.name.c_str(), e.args.c_str(), rx);
                e = hit(ux + 800, mt[0] + 40);
                CHECKV(e.name.empty(), "a click on the confirm's own words does nothing: '%s'", e.name.c_str());
            }
        }
        park();
    }

    // ---------- the plugin page: 1000 x 700 ----------
    const int pxp = kStX, pyp = kPluginY;
    const int barP = yOf(ui->get_plugin_bar_h());
    {
        const Frame f = shot();
        crop(f, "crop-st-plugin.png", pxp, pyp, kStW, kStH, 1);
        topBar(f, pxp, pyp, kStW, barP, true, "Plugin page");
        const int ty = pyp + barP + 12;
        CHECKV(f.near(pxp + 12 + 12, ty, keyLine, 0) && f.near(pxp + 12 + 12, ty + 23, keyLine, 0) && f.near(pxp + 12, ty + 12, keyLine, 0) && f.near(pxp + 12 + 12, ty - 1, panel, 0), "< BROWSE: a 24 px key at the left");
        CHECKV(f.countNear(pxp + 12 + 8, ty + 5, pxp + 12 + 80, ty + 19, keyFg, 60) >= 40, "< BROWSE: its words");
        CHECKV(f.countNear(pxp + 988 - 80, ty + 1, pxp + 988, ty + 23, accent, 0) >= 700 && f.near(pxp + 987, ty + 12, accent, 0), "the page key (UPDATE) is lit and flush right");
        // the header: the glyph tile, the name, the version and origin line, the badges, the install report
        const int hy = ty + 24 + 8;
        CHECKV(f.near(pxp + 12 + 15, hy, lineStrong, 0) && f.near(pxp + 12 + 15, hy + 29, lineStrong, 0) && f.near(pxp + 12, hy + 15, lineStrong, 0), "the glyph tile is 30 px");
        CHECKV(f.countNear(pxp + 12 + 38, hy, pxp + 12 + 260, hy + 16, fg, 60) >= 100, "the plugin's name, 13 px semibold");
        CHECKV(f.countNear(pxp + 12 + 38, hy + 18, pxp + 12 + 500, hy + 32, dim, 24) >= 100, "1.9.2 - origin: the dim line");
        const int by2 = hy + 32 + 8;
        CHECKV(f.countNear(pxp + 12, by2, pxp + 12 + 70, by2 + 20, warnBg, 0) >= 150 && f.countNear(pxp + 12 + 70, by2, pxp + 12 + 150, by2 + 20, chipBg, 0) >= 150, "the badges: NETWORK amber, 1090 MHz neutral");
        CHECKV(f.countNear(pxp + 12, by2 + 28, pxp + 12 + 300, by2 + 44, goodFg, 40) >= 60, "the install report is green");
        // the screenshots: a heading, a framed strip 132 high with an arrow each side, the note under it
        int stripT = -1;
        for (int y = by2 + 40; y < pyp + kStH && stripT < 0; ++y) { if (f.near(pxp + 12 + 26 + 300, y, lineStrong, 0) && f.near(pxp + 12 + 26 + 300, y + 3, well, 0)) { stripT = y; } }
        CHECKV(stripT > 0, "the strip's frame (top %d)", stripT);
        if (stripT > 0) {
            CHECKV(f.countNear(pxp + 12, stripT - 18, pxp + 12 + 120, stripT - 6, muted, 30) >= 30, "SCREENSHOTS: the cap heading");
            CHECKV(f.near(pxp + 12 + 26 + 300, stripT + 131, lineStrong, 0) && f.near(pxp + 12 + 26, stripT + 60, lineStrong, 0) && f.near(pxp + 12 + 26 + 923, stripT + 60, lineStrong, 0), "the strip is 132 high, 26 px in from each side");
            CHECKV(f.countInk(pxp + 12 + 26 + 100, stripT + 10, pxp + 12 + 26 + 800, stripT + 122, well) >= 400, "the picture is drawn in the strip");
            const int ay = stripT + 56;
            CHECKV(f.near(pxp + 12 + 10, ay, dis, 8) && f.near(pxp + 12 + 10, ay + 19, dis, 8), "the left arrow is greyed on the first picture");
            CHECKV(f.near(pxp + 988 - 10, ay, keyLine, 0) && f.near(pxp + 988 - 10, ay + 19, keyLine, 0), "the right arrow answers");
            CHECKV(f.countNear(pxp + 12, stripT + 136, pxp + 12 + 200, stripT + 150, dim, 24) >= 30, "Map with trails: the caption under the strip");
            Ev e = hit(pxp + 988 - 10, ay + 10);
            CHECKV(e.name == "store-picture" && e.args == "1", "the right arrow asks for picture 1: '%s' '%s'", e.name.c_str(), e.args.c_str());
            e = hit(pxp + 12 + 10, ay + 10);
            CHECKV(e.name.empty(), "the left arrow on the first picture asks for nothing: '%s'", e.name.c_str());
            // the notice box (BEFORE YOU FIT IT): the amber ground, the maker's words, the ticked tick
            int nbT = -1;
            for (int y = stripT + 150; y < pyp + kStH && nbT < 0; ++y) { if (f.near(pxp + 12 + 500, y, cardWarnLine, 0)) { nbT = y; } }
            CHECKV(nbT > 0 && f.near(pxp + 12 + 500, nbT + 4, cardWarn, 0), "the notice box on the card-warn ground (top %d)", nbT);
            if (nbT > 0) {
                CHECKV(f.countNear(pxp + 12 + 8, nbT + 8, pxp + 12 + 200, nbT + 22, warnFg, 40) >= 40, "BEFORE YOU FIT IT: the cap heading in amber");
                CHECKV(f.countNear(pxp + 12 + 8, nbT + 24, pxp + 12 + 500, nbT + 40, warnFg, 40) >= 100, "the maker's notice in amber");
                const int tb = findBoxOn(f, pxp + 12 + 8, nbT + 36, nbT + 80, true, cardWarn);
                CHECKV(tb > 0, "the notice tick is ticked (top %d)", tb);
                e = hit(pxp + 12 + 8 + 100, tb + 7);
                CHECKV(e.name == "store-plugin-notice" && e.args == "0", "the sentence unticks the notice: '%s' '%s'", e.name.c_str(), e.args.c_str());
                // DETAILS: the cap heading, HIDE DETAILS as an xs key at the right, the facts in the mono face
                int dkT = -1;
                for (int y = nbT + 40; y < pyp + kStH && dkT < 0; ++y) { if (f.near(pxp + 988 - 20, y, keyLine, 0) && f.near(pxp + 988 - 20, y - 1, panel, 0)) { dkT = y; } }
                CHECKV(dkT > 0 && f.near(pxp + 987, dkT + 10, keyLine, 0) && f.near(pxp + 988 - 20, dkT + 19, keyLine, 0), "HIDE DETAILS: an xs key flush right (top %d)", dkT);
                CHECKV(f.countNear(pxp + 12, dkT + 2, pxp + 12 + 80, dkT + 18, muted, 30) >= 30, "DETAILS: the cap heading");
                CHECKV(f.countNear(pxp + 12, dkT + 26, pxp + 12 + 400, dkT + 58, fgSoft, 40) >= 80, "the facts are drawn in the mono face");
                CHECKV(f.countNear(pxp + 12, dkT + 60, pxp + 12 + 600, dkT + 80, dim, 24) >= 100, "the reach sentence under the facts is dim");
                e = hit(pxp + 988 - 30, dkT + 10);
                CHECKV(e.name == "store-plugin-details" && e.args == "0", "HIDE DETAILS asks to hide them: '%s' '%s'", e.name.c_str(), e.args.c_str());
            }
        }
        Ev e = hit(pxp + 12 + 40, ty + 12);
        CHECKV(e.name == "store-plugin-back", "< BROWSE: '%s'", e.name.c_str());
        e = hit(pxp + 988 - 30, ty + 12);
        CHECKV(e.name == "store-plugin-key", "the page key: '%s'", e.name.c_str());
        park();
    }

    // ---------- the narrow page: 500 x 700, SITES open, two cards to a row, default-forwarding to ExtendEvents ----------
    const int nx = kNarrowX, ny = kNarrowY;
    const int barN = yOf(ui->get_narrow_bar_h());
    {
        const Frame f = shot();
        crop(f, "crop-st-narrow.png", nx, ny, kNarrowW, kNarrowH, 2);
        topBar(f, nx, ny, kNarrowW, barN, true, "Narrow");
        CHECKV(ui->get_narrow_cols() == 2 && std::fabs(ui->get_narrow_card_w() - 235.0) < 0.01, "a 500 px page lays two cards to a row, 235 wide (%d, %.2f)", ui->get_narrow_cols(), ui->get_narrow_card_w());
        CHECKV(f.countNear(nx + 12, ny + 48, nx + 480, ny + 62, warnFg, 40) >= 100, "the out-of-date catalogue line is amber");
        // the keys of the bar: CHECK AGAIN, SITES (latched: lit) and CANCEL (the danger key)
        const int kyN = ny + barN - 8 - 24;
        CHECKV(f.countNear(nx + 12, kyN, nx + 480, kyN + 24, accent, 0) >= 700, "SITES is lit while the panel is open");
        CHECKV(f.countNear(nx + 12, kyN, nx + 480, kyN + 24, dangerBg, 0) >= 600, "CANCEL is the danger key");
        // the SITES panel: the card ground in a strong border, 8 px in
        const int P = ny + barN + 12;
        CHECKV(f.near(nx + 12 + 200, P, lineStrong, 0) && f.near(nx + 12 + 200, P + 4, card, 0) && f.near(nx + 12, P + 60, lineStrong, 0) && f.near(nx + 12 + 475, P + 60, lineStrong, 0), "the SITES panel spans the page's inner width");
        CHECKV(f.countNear(nx + 12 + 8, P + 8, nx + 12 + 60, P + 32, warnFg, 40) >= 30, "SITES: the amber tag");
        CHECKV(f.countNear(nx + 12 + 8, P + 36, nx + 12 + 300, P + 60, fg, 60) >= 100, "the site's name");
        const int rmT = P + 36 + 2;
        CHECKV(f.near(nx + 12 + 8 + 459, rmT + 10, keyLine, 0) && f.near(nx + 12 + 8 + 440, rmT, keyLine, 0) && f.near(nx + 12 + 8 + 440, rmT + 19, keyLine, 0) && !f.near(nx + 12 + 8 + 440, rmT + 20, keyLine, 0), "REMOVE: an xs key flush right");
        CHECKV(f.countNear(nx + 12 + 8, P + 62, nx + 12 + 400, P + 78, warnFg, 40) >= 100, "the site's status line is amber");
        const int adT = P + 80;
        CHECKV(f.near(nx + 12 + 8 + 459 - 20, adT, dis, 8) && f.near(nx + 12 + 8 + 459, adT + 12, dis, 8), "ADD is greyed while the reason stands");
        CHECKV(f.near(nx + 12 + 8 + 200, adT, lineStrong, 0) && f.near(nx + 12 + 8 + 200, adT + 4, field, 0), "the address field takes the width between its label and ADD");
        CHECKV(f.countNear(nx + 12 + 8 + 70, adT + 4, nx + 12 + 8 + 160, adT + 20, fg, 60) >= 25, "https:// is the field's text");
        CHECKV(f.countNear(nx + 12 + 8, adT + 28, nx + 12 + 8 + 460, adT + 42, dim, 24) >= 100, "ADD's reason is dim");
        CHECKV(f.countNear(nx + 12 + 8, adT + 46, nx + 12 + 8 + 460, adT + 60, warnFg, 40) >= 80, "the refusal of the last ADD is amber");
        const int cbT = findBoxOn(f, nx + 12 + 8, adT + 62, adT + 90, false, card);
        CHECKV(cbT > 0, "the consent tick is empty (top %d)", cbT);
        // the grid: borders down the left edge are the panel, the first row of cards, the second row (VOR alone)
        const std::vector<int> rl = vruns(f, nx + 12, ny + barN + 4, ny + kNarrowH, lineStrong);
        CHECKV(rl.size() == 3, "the panel and two rows of cards down the left edge: %s", list(rl).c_str());
        if (rl.size() == 3) {
            const int y1 = rl[1] + 20, y2 = rl[2] + 20, l0 = nx + 12;
            CHECKV(f.near(l0, y1, lineStrong, 0) && f.near(l0 + 234, y1, lineStrong, 0) && f.near(l0 + 241, y1, lineStrong, 0) && f.near(l0 + 475, y1, lineStrong, 0)
                       && !f.near(l0 - 1, y1, lineStrong, 0) && !f.near(l0 + 235, y1, lineStrong, 0) && !f.near(l0 + 240, y1, lineStrong, 0) && !f.near(l0 + 476, y1, lineStrong, 0) && !f.near(l0 + 237, y1, lineStrong, 0),
                   "two cards of 235 side by side, 6 apart, to the page's inner edge");
            CHECKV(f.near(l0, y2, lineStrong, 0) && f.near(l0 + 234, y2, lineStrong, 0) && !f.near(l0 + 241, y2, lineStrong, 0) && !f.near(l0 + 475, y2, lineStrong, 0),
                   "the third card is alone in the next row and keeps the column width");
            CHECKV(f.countNear(nx + 12 + 8, rl[1] - 28, nx + 12 + 100, rl[1] - 12, muted, 30) >= 30, "AIRCRAFT: the category heading above the grid");
        }
        // the gestures: all forwarded to ExtendEvents
        Ev e = hitFwd(nx + 12 + 8 + 459 - 10, rmT + 10);
        CHECKV(e.name == "store-site-remove" && e.args == "0", "REMOVE: '%s' '%s'", e.name.c_str(), e.args.c_str());
        e = hitFwd(nx + 12 + 8 + 459 - 10, adT + 12);
        CHECKV(e.name.empty(), "a greyed ADD asks for nothing: '%s'", e.name.c_str());
        typeInto(nx + 12 + 8 + 200, adT + 12, "x", false);
        CHECKV(!fwd.empty() && fwd.back().name == "store-site-draft" && fwd.back().args == "x", "typing in the address field asks as it goes: '%s'", names(fwd).c_str());
        e = hitFwd(nx + 12 + 8 + 100, cbT + 7);
        CHECKV(e.name == "store-consent" && e.args == "1", "the consent sentence asks to tick: '%s' '%s'", e.name.c_str(), e.args.c_str());
        e = hitFwd(nx + 12 + 8 + 7, cbT + 7);
        CHECKV(e.name == "store-consent" && e.args == "1", "the consent box too: '%s' '%s'", e.name.c_str(), e.args.c_str());
        {
            auto m = ui->get_m_store_narrow();
            m.add_reason = "";
            ui->set_m_store_narrow(m);
            const Frame g = shot();
            CHECKV(g.near(nx + 12 + 8 + 459 - 20, adT, keyLine, 0), "with no reason standing ADD is a plain key");
            e = hitFwd(nx + 12 + 8 + 459 - 10, adT + 12);
            CHECKV(e.name == "store-site-add", "ADD: '%s'", e.name.c_str());
            m.add_reason = "ADD is greyed until the box is ticked and the address starts with https://";
            ui->set_m_store_narrow(m);
            shot();
        }
        // DEAD KEYS ARE GREYED (docs 6.61): with a transfer running (check-enabled and remove-enabled false) CHECK AGAIN and REMOVE are drawn disabled and ask nothing
        {
            auto m = ui->get_m_store_narrow();
            const Frame before = shot();
            m.check_enabled = false;
            auto sites = std::make_shared<slint::VectorModel<StoreSite>>();
            for (std::size_t i = 0; i < m.sites->row_count(); ++i) { StoreSite s = *m.sites->row_data(i); s.remove_enabled = false; sites->push_back(s); }
            m.sites = sites;
            ui->set_m_store_narrow(m);
            const Frame g = shot();
            int changed = 0;   // the keys' ground changes to the disabled one: pixels differ in the top bar and the site row
            for (int x = nx + 4; x < nx + 120; ++x) { const auto a = g.at(x, kyN + 12), b = before.at(x, kyN + 12); if (a.r != b.r || a.g != b.g || a.b != b.b) { ++changed; } }
            CHECKV(changed > 20, "a greyed CHECK AGAIN is drawn differently (%d pixels changed)", changed);
            Ev ec = hitFwd(nx + 20, kyN + 12);
            CHECKV(ec.name.empty(), "a greyed CHECK AGAIN asks for nothing: '%s'", ec.name.c_str());
            Ev er = hitFwd(nx + 12 + 8 + 459 - 10, rmT + 10);
            CHECKV(er.name.empty(), "a greyed REMOVE asks for nothing: '%s'", er.name.c_str());
            m = ui->get_m_store_narrow();
            m.check_enabled = true;
            auto sites2 = std::make_shared<slint::VectorModel<StoreSite>>();
            for (std::size_t i = 0; i < m.sites->row_count(); ++i) { StoreSite s = *m.sites->row_data(i); s.remove_enabled = true; sites2->push_back(s); }
            m.sites = sites2;
            ui->set_m_store_narrow(m);
            shot();
        }
        std::vector<int> kk;
        {
            bool in = false;
            for (int x = nx + 4; x < nx + 400; ++x) {
                const bool inked = !f.near(x, kyN + 1, strip, 6);
                if (inked && !in) { kk.push_back(x); }
                if (!inked && in) { kk.push_back(x - 1); }
                in = inked;
            }
        }
        CHECKV(kk.size() >= 6, "the three keys of the bar: %s", list(kk).c_str());
        if (kk.size() >= 6) {
            e = hitFwd((kk[0] + kk[1]) / 2, kyN + 12);
            CHECKV(e.name == "store-check", "CHECK AGAIN: '%s'", e.name.c_str());
            e = hitFwd((kk[2] + kk[3]) / 2, kyN + 12);
            CHECKV(e.name == "store-sites-toggle", "SITES: '%s'", e.name.c_str());
            e = hitFwd((kk[4] + kk[5]) / 2, kyN + 12);
            CHECKV(e.name == "store-cancel", "CANCEL: '%s'", e.name.c_str());
        }
        e = hitFwd(nx + 12 + 40, rl.size() == 3 ? rl[1] + 14 : ny + 300);
        CHECKV(e.name == "store-card-open" && e.args == "adsb", "a card opens its page (forwarded): '%s' '%s'", e.name.c_str(), e.args.c_str());
        park();
    }

    // ---------- a page too short for its cards: the body scrolls under the top bar ----------
    const int sx = 622, sy = 730;
    {
        const auto wheel = [&](int x, int y, float dy) {
            mv(x, y);
            scroll(win, static_cast<float>(x), static_cast<float>(y), 0.0f, dy);
            const auto start = std::chrono::steady_clock::now();
            double prev = -1e9;
            int still = 0;
            while (std::chrono::steady_clock::now() - start < std::chrono::seconds(4)) {
                settle(milliseconds(40));
                shot();
                const double cur = ui->get_short_scrolled();
                still = std::abs(cur - prev) < 0.01 ? still + 1 : 0;
                prev = cur;
                if (still >= 3 && std::chrono::steady_clock::now() - start >= milliseconds(120)) { break; }
            }
        };
        const Frame f = shot();
        crop(f, "crop-st-short.png", sx, sy, 500, 300, 2);
        const int barS = yOf(ui->get_short_bar_h());
        CHECKV(ui->get_short_content_h() > 300 - barS + 40, "the cards outgrow the body (%.0f in %d)", ui->get_short_content_h(), 300 - barS);
        CHECKV(ui->get_short_scrolled() == 0, "the body starts at the top");
        CHECKV(f.countNear(sx + 500 - 9, sy + barS + 1, sx + 500 - 3, sy + 300, knobRing, 0) >= 100, "the scroll thumb is drawn at the right edge");
        const std::vector<int> before = vruns(f, sx + 12, sy + barS + 1, sy + 300, lineStrong);
        wheel(sx + 250, sy + 200, -120.0f);
        const Frame g = shot();
        const std::vector<int> after = vruns(g, sx + 12, sy + barS + 1, sy + 300, lineStrong);
        CHECKV(ui->get_short_scrolled() > 20, "a wheel notch scrolls the body (%.1f)", ui->get_short_scrolled());
        CHECKV(g.near(sx + 250, sy + barS - 1, line, 0) && g.near(sx + 250, sy + 4, strip, 0), "the top bar stays where it is while the cards move");
        CHECKV(before != after, "the cards moved under the bar: %s -> %s", list(before).c_str(), list(after).c_str());
        wheel(sx + 250, sy + 200, -5000.0f);
        CHECKV(std::fabs(ui->get_short_scrolled() - (ui->get_short_content_h() - (300 - barS))) < 1.5, "the end of the body: scrolled %.1f of %.1f", ui->get_short_scrolled(), ui->get_short_content_h() - (300 - barS));
        wheel(sx + 250, sy + 200, 5000.0f);
        CHECKV(std::fabs(ui->get_short_scrolled()) < 0.5, "and back to the top (%.1f)", ui->get_short_scrolled());
        park();
    }

    return testSummary("test_slint_extend");
}
