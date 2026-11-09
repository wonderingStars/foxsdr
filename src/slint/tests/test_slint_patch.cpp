// The PATCH page (ui/patch_page.slint): PatchPage (the band, the canvas with its nodes, wires and faces, the inspector) and PatchPresetsPage (PageTab kind 10) -
// rendered headlessly from ui/specimens/patch_specimen.slint and probed against the PatchScope artboard (artboards/Workbench-PatchScope.dc.html) and the contract
// (slint-patch-contract.md, Components).
//
// What is asserted to the pixel: the band's rows (the transport keys 30 px, the run lamp, the seven part keys 24 px wrapping in their FlowRow at a narrow width, the
// 20 px decoder keys after their caption, the blueprints DropKey and its tall list), the information pane's chip tones and branches, the canvas (the grid, the plates
// with their outlines - selected ivory 2 px, blocked red 2 px - the header, the close key's hot state, the grip, the well with its two readings, the port dots by tone,
// the wires by tone and their selected stroke, the ghost, the refusal bubble, the three canvas keys), the faces per kind, the inspector's blocks and the Size keys, and
// the presets page (rows, the four keys, the name row, the note, the two in-page questions, rule 11 at 254 px). What is asserted as ink in a region: every word (the
// typefaces are the embedded Inter and JetBrains Mono, never glyph positions). Every callback is driven by a real pointer or key event and read back through the
// specimen's `Rec` global (the components with an override) or through PatchEvents (the plain ones).
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "render_harness.hpp"
#include "test_check.hpp"

#include "patch_specimen.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
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
Rgb blendOver(slint::Color c, Rgb under) { return over(tok(c), c.alpha() / 255.0, under); }

struct Ev {
    std::string name, args;
    std::string str() const { return args.empty() ? name : name + " " + args; }
};

// the part keys and the decoder keys are placed from ESTIMATED widths (characters * 7 + padding: 28 for a sm key, 20 for an xs key) by the window's keyWraps rule
int charCount(const std::string& s) {
    int n = 0;
    for (const unsigned char c : s) { if ((c & 0xC0) != 0x80) { ++n; } }
    return n;
}
struct Placed {
    int x = 0, row = 0, w = 0;
};
// widths in order; the first key goes at `first` (with `lead` true a key that does not fit after the caption wraps to x = left)
std::vector<Placed> flow(const std::vector<int>& widths, int left, int first, bool lead, int right, int gap) {
    std::vector<Placed> out;
    int cx = first - gap;   // the right edge of the last key placed
    int row = 0;
    for (const int w : widths) {
        const bool wrap = (lead || !out.empty()) && cx + gap + w > right;
        Placed p;
        p.w = w;
        if (wrap) { p.x = left; ++row; } else { p.x = cx + gap; }
        p.row = row;
        cx = p.x + w;
        out.push_back(p);
    }
    return out;
}

#define CHECK_GT(cnt, n)                                                  \
    do {                                                                  \
        const int c_ = static_cast<int>(cnt);                             \
        CHECKV(c_ > (n), "count %d, wanted more than %d", c_, static_cast<int>(n)); \
    } while (0)

std::string fmtD(double v) {
    char b[32];
    std::snprintf(b, sizeof(b), "%g", v);
    return b;
}

constexpr int kW = 1260, kH = 1130;
constexpr int PX = 10, PY = 10, PW = 1100, PH = 640;          // pg
constexpr int NX = 10, NY = 660, NW = 520, NH = 420;          // pgn
constexpr int RX = 540, RY = 660, RW = 440, RH = 460;         // pr
constexpr int QX = 990, QY = 660, QW = 254, QH = 460;         // prn

}  // namespace

int main() {
    installHeadlessPlatform();
    auto ui = PatchSpecimen::create();
    slint::Window& win = ui->window();

    const Skin wb = ui->global<Workbench>().get_value();
    const Rgb bg = tok(wb.bg), panel = tok(wb.panel), panelAlt = tok(wb.panel_alt), strip = tok(wb.strip), line = tok(wb.line), lineStrong = tok(wb.line_strong);
    const Rgb well = tok(wb.well), wellDeep = tok(wb.well_deep), field = tok(wb.field), menu = tok(wb.menu), card = tok(wb.card);
    const Rgb fg = tok(wb.fg), fgSoft = tok(wb.fg_soft), dim = tok(wb.dim), muted = tok(wb.muted);
    const Rgb accent = tok(wb.accent), accentInk = tok(wb.accent_ink), accentSoft = tok(wb.accent_soft), focus = tok(wb.focus);
    const Rgb errFg = tok(wb.chip_err_fg), warnFg = tok(wb.chip_warn_fg), goodFg = tok(wb.chip_good_fg);
    const Rgb keyBg = tok(wb.key_bg), keyLine = tok(wb.key_line), keyDisBg = tok(wb.key_disabled_bg), dangerBg = tok(wb.danger_bg);
    const Rgb lampGreen = tok(wb.lamp_green), lampAmber = tok(wb.lamp_amber), lampRed = tok(wb.lamp_red), lampOff = tok(wb.lamp_off);
    const Rgb bookmark = tok(wb.bookmark), cardErr = tok(wb.card_err), sliderTrack = tok(wb.slider_track), levelBar = tok(wb.level_bar);
    const Rgb tableSel = tok(wb.table_selected);
    const Rgb lineFaint = blendOver(wb.line_faint, wellDeep);
    CHECK(nearColour(panel, rgb(0x1d2026), 0) && nearColour(wellDeep, rgb(0x06070a), 0) && nearColour(accent, rgb(0xf5a524), 0));
    CHECK(nearColour(errFg, rgb(0xff9b9b), 0) && nearColour(warnFg, rgb(0xf5c66b), 0) && nearColour(goodFg, rgb(0x7fe6ad), 0));

    std::vector<Ev> evs;    // what the overriding page (pg) and the overriding presets page (pr) report
    std::vector<Ev> fwd;    // what the default-forwarding pages (pgn, prn) send to PatchEvents
    ui->global<Rec>().on_fired([&](slint::SharedString n, slint::SharedString a) { evs.push_back({std::string(n), std::string(a)}); });
    auto& pe = ui->global<PatchEvents>();
    const auto F = [&](const char* name) { return [&fwd, name]() { fwd.push_back({name, ""}); }; };
    pe.on_run_clicked(F("run-clicked"));
    pe.on_all_off_clicked(F("all-off-clicked"));
    pe.on_presets_clicked(F("presets-clicked"));
    pe.on_part_pressed([&](int k) { fwd.push_back({"part-pressed", std::to_string(k)}); });
    pe.on_decoder_pressed([&](int k) { fwd.push_back({"decoder-pressed", std::to_string(k)}); });
    pe.on_blueprints_opened(F("blueprints-opened"));
    pe.on_blueprint_picked([&](int k) { fwd.push_back({"blueprint-picked", std::to_string(k)}); });
    pe.on_canvas_size([&](float w, float h) { fwd.push_back({"canvas-size", fmtD(w) + " " + fmtD(h)}); });
    pe.on_zoom_step([&](bool in) { fwd.push_back({"zoom-step", in ? "1" : "0"}); });
    pe.on_canvas_pressed([&](float x, float y, int b) { fwd.push_back({"canvas-pressed", fmtD(x) + " " + fmtD(y) + " " + std::to_string(b)}); });
    pe.on_canvas_wheel([&](float x, float y, float nx, float ny, bool c, bool s) { fwd.push_back({"canvas-wheel", fmtD(x) + " " + fmtD(y) + " " + fmtD(nx) + " " + fmtD(ny) + " " + (c ? "1" : "0") + " " + (s ? "1" : "0")}); });
    pe.on_face_freq([&](int id, slint::SharedString t) { fwd.push_back({"face-freq", std::to_string(id) + " " + std::string(t)}); });
    pe.on_inspector_fold(F("inspector-fold"));
    pe.on_inspector_size([&](int s) { fwd.push_back({"inspector-size", std::to_string(s)}); });
    pe.on_inspector_remove(F("inspector-remove"));
    pe.on_fit(F("fit"));
    pe.on_canvas_delete(F("canvas-delete"));
    pe.on_presets_row([&](int k) { fwd.push_back({"presets-row", std::to_string(k)}); });
    pe.on_presets_row_activated([&](int k) { fwd.push_back({"presets-row-activated", std::to_string(k)}); });
    pe.on_presets_load(F("presets-load"));
    pe.on_presets_save_as(F("presets-save-as"));
    pe.on_presets_rename(F("presets-rename"));
    pe.on_presets_delete(F("presets-delete"));
    pe.on_presets_name([&](slint::SharedString t) { fwd.push_back({"presets-name", std::string(t)}); });
    pe.on_presets_go(F("presets-go"));
    pe.on_presets_cancel(F("presets-cancel"));
    pe.on_presets_replace([&](bool y) { fwd.push_back({"presets-replace", y ? "1" : "0"}); });
    pe.on_presets_delete_confirm([&](bool y) { fwd.push_back({"presets-delete-confirm", y ? "1" : "0"}); });

    const auto mv = [&](int x, int y) { moveTo(win, static_cast<float>(x), static_cast<float>(y)); };
    const auto clk = [&](int x, int y, slint::PointerEventButton b = slint::PointerEventButton::Left) { click(win, static_cast<float>(x), static_cast<float>(y), b); };
    const auto tap = [&](std::u8string_view k) { keyTap(win, k); };
    const auto shot = [&] { return render(ui, kW, kH); };
    const auto park = [&] { mv(1255, 1125); settle(milliseconds(30)); };
    const auto typeText = [&](const std::string& text) { for (const char c : text) { const std::u8string u(1, static_cast<char8_t>(c)); tap(u); } };
    // the events a gesture reported, canvas-size and the pointer's own movement left out unless asked for
    const auto quiet = [](const std::vector<Ev>& v, bool moves = false) {
        std::vector<std::string> r;
        for (const Ev& e : v) {
            if (e.name == "canvas-size") { continue; }
            if (!moves && (e.name == "canvas-moved" || e.name == "canvas-left")) { continue; }
            r.push_back(e.str());
        }
        return r;
    };
    const auto join = [](const std::vector<std::string>& v) { std::string s; for (const std::string& e : v) { s += (s.empty() ? "" : " | ") + e; } return s; };
    const auto lastOf = [](const std::vector<Ev>& v, const char* name) {
        for (auto it = v.rbegin(); it != v.rend(); ++it) { if (it->name == name) { return *it; } }
        return Ev{};
    };
    const auto countOf = [](const std::vector<Ev>& v, const char* name) { int n = 0; for (const Ev& e : v) { if (e.name == name) { ++n; } } return n; };
    // click a spot and report what the overriding pages said / what the plain ones forwarded
    const auto hit = [&](int x, int y) { evs.clear(); fwd.clear(); clk(x, y); shot(); return quiet(evs); };
    const auto hitF = [&](int x, int y) { evs.clear(); fwd.clear(); clk(x, y); shot(); return quiet(fwd); };
    const auto isOnly = [&](const std::vector<std::string>& v, const std::string& want) { return v.size() == 1 && v[0] == want; };
    // the list a dropdown opens under a 24 px key at (keyX, keyY): row i's middle
    const auto popupRow = [&](int keyX, int keyY, int i) { return std::pair<int, int>{keyX + 30, keyY + 24 + 2 + 5 + i * 26 + 13}; };
    // type into the field at (x, y): End, erase, type, optionally Enter
    const auto typeInto = [&](int x, int y, const std::string& text, bool enter, int erase = 24) {
        evs.clear();
        fwd.clear();
        clk(x, y);
        tap(slint::platform::key_codes::End);
        for (int i = 0; i < erase; ++i) { tap(slint::platform::key_codes::Backspace); }
        typeText(text);
        if (enter) { tap(slint::platform::key_codes::Return); }
        shot();
    };
    const auto editNode = [&](int i, const std::function<void(CanvasNode&)>& f) {
        auto c = ui->get_m_canvas();
        auto n = *c.nodes->row_data(static_cast<std::size_t>(i));
        f(n);
        c.nodes->set_row_data(static_cast<std::size_t>(i), n);
    };
    const auto resetEv = [&] { evs.clear(); fwd.clear(); };

    Frame f = shot();
    f.savePng(shotDir() + "patch-specimen.png");

    // ======================================================== the page's geometry (1100 x 640 at (10, 10)) ========================================================
    const int bandH = static_cast<int>(std::lround(ui->get_band_h()));
    const int canvasOy = static_cast<int>(std::lround(ui->get_canvas_oy()));
    const int infoW = static_cast<int>(std::lround(ui->get_info_w()));
    const int cw = static_cast<int>(std::lround(ui->get_canvas_w()));
    const int ch = static_cast<int>(std::lround(ui->get_canvas_h()));
    if (std::getenv("FOXSDR_SLINT_DUMP") != nullptr) { std::printf("band %d, canvas oy %d, canvas %d x %d\n", bandH, canvasOy, cw, ch); }
    CHECKV(infoW == 462, "info width %d (clamp(1100 * 0.42, 300, 560), at most half)", infoW);
    CHECKV(static_cast<int>(std::lround(ui->get_left_w())) == 630, "left column %d", static_cast<int>(std::lround(ui->get_left_w())));
    CHECKV(bandH >= 96 && bandH <= 200, "band height %d", bandH);
    CHECKV(canvasOy == bandH + 1, "canvas top %d vs band %d + 1 rule", canvasOy, bandH);
    CHECKV(cw == PW - 8 - 190, "canvas width %d (the page, the 8 px gap, the 190 px inspector)", cw);
    CHECKV(ch == PH - canvasOy, "canvas height %d", ch);
    CHECKV(static_cast<int>(std::lround(ui->get_inspector_x())) == PW - 190 && static_cast<int>(std::lround(ui->get_inspector_w())) == 190, "inspector at %d, %d wide", static_cast<int>(std::lround(ui->get_inspector_x())), static_cast<int>(std::lround(ui->get_inspector_w())));
    CHECK(static_cast<int>(std::lround(ui->get_transport_y())) == 8);
    CHECK(static_cast<int>(std::lround(ui->get_readout_y())) == 44);
    CHECK(static_cast<int>(std::lround(ui->get_parts_y())) == 63 && ui->get_part_rows() == 1);
    CHECK(static_cast<int>(std::lround(ui->get_decoders_y())) == 93 && ui->get_decoder_rows() == 1);
    CHECK(static_cast<int>(std::lround(ui->get_blueprints_y())) == 119);
    CHECK(static_cast<int>(std::lround(ui->get_blueprint_note_y())) == 149);
    // the band: the strip ground, its rule, the information pane's left hairline
    CHECK(f.near(PX + 5, PY + 5, strip, 0));
    CHECK(f.rowNear(PY + bandH, PX + 2, PX + 500, line, 2));
    CHECK(f.colNear(PX + PW - infoW, PY + 2, PY + bandH - 2, line, 2));
    CHECK(f.near(PX + PW - infoW - 4, PY + 5, strip, 0));

    // ---- the transport row: START / STOP, ALL OFF, PRESETS ----
    {
        const int ty = PY + 8;
        CHECK(f.near(PX + 14, ty + 15, dangerBg, 2));                       // running: the run key is `danger`
        CHECK(f.near(PX + 104, ty + 15, dangerBg, 2));                      // ALL OFF is `danger` always
        CHECK(f.near(PX + 260, ty + 15, keyBg, 2));                         // PRESETS closed
        CHECK(f.colNear(PX + 10, ty + 6, ty + 24, keyLine, 12) || f.colNear(PX + 10, ty + 6, ty + 24, tok(wb.danger_line), 12));
        CHECK(f.countInk(PX + 14, ty + 4, PX + 90, ty + 26, dangerBg, 40) > 20);   // STOP is drawn
        CHECK(f.countInk(PX + 104, ty + 4, PX + 246, ty + 26, dangerBg, 40) > 30);  // ALL OFF
        CHECK(f.countInk(PX + 260, ty + 4, PX + 336, ty + 26, keyBg, 40) > 30);     // PRESETS
        CHECK(isOnly(hit(PX + 50, ty + 15), "run-clicked"));
        CHECK(isOnly(hit(PX + 175, ty + 15), "all-off-clicked"));
        CHECK(isOnly(hit(PX + 296, ty + 15), "presets-clicked"));
        // the stopped look: START is `on` (accent); PRESETS is `on` while the page is open
        auto m = ui->get_m_patch();
        m.running = false;
        m.run_label = "START";
        m.presets_open = true;
        ui->set_m_patch(m);
        park();
        f = shot();
        CHECK(f.near(PX + 14, ty + 15, accent, 2));
        CHECK(f.near(PX + 260, ty + 15, accent, 2));
        CHECK(f.countNear(PX + 14, ty + 4, PX + 90, ty + 26, accentInk, 30) > 10);   // START in the ink of an `on` key
        m.running = true;
        m.run_label = "STOP";
        m.presets_open = false;
        ui->set_m_patch(m);
        park();
        f = shot();
        CHECK(f.near(PX + 14, ty + 15, dangerBg, 2) && f.near(PX + 260, ty + 15, keyBg, 2));
    }

    // ---- the run readout: a lamp and its caption ----
    {
        const int ly = PY + 44;
        CHECK(f.near(PX + 14, ly + 6, lampGreen, 12));                          // lit: the green dot
        CHECK(f.countNear(PX + 24, ly, PX + 260, ly + 14, tok(wb.lamp_green_cap), 40) > 20);   // the phosphor-green caption
        auto m = ui->get_m_patch();
        m.run_line_lit = false;
        m.run_line = "STOPPED - 0 of 1 radio switched on";
        ui->set_m_patch(m);
        park();
        f = shot();
        CHECK(!f.near(PX + 14, ly + 6, lampGreen, 30));
        CHECK(f.countNear(PX + 24, ly, PX + 260, ly + 14, tok(wb.lamp_green_cap), 40) == 0);
        CHECK(f.countNear(PX + 24, ly, PX + 260, ly + 14, muted, 40) > 20);
        m.run_line_lit = true;
        m.run_line = "RUNNING - 1 of 1 radio open";
        ui->set_m_patch(m);
        park();
        f = shot();
    }

    // ---- the seven part keys, one row at 610 px ----
    const std::vector<std::string> partLabels = {"Radio", "Channel", "Demod", "Spectrum", "Speaker", "Text out", "Map"};
    {
        std::vector<int> w;
        for (const auto& l : partLabels) { w.push_back(7 * charCount(l) + 28); }
        const auto at = flow(w, PX + 10, PX + 10, false, PX + 10 + 610, 4);
        const int py = PY + 63;
        CHECK(at.size() == 7);
        for (std::size_t i = 0; i < at.size(); ++i) {
            CHECKV(at[i].row == 0, "part key %zu wrapped at 610 px", i);
            CHECK(f.near(at[i].x + 4, py + 12, keyBg, 2));                                   // the key's ground
            CHECK(!f.near(at[i].x + at[i].w, py + 12, keyBg, 2) || i + 1 < at.size());     // each key is its own box
            CHECK(f.countInk(at[i].x + 6, py + 4, at[i].x + at[i].w - 6, py + 20, keyBg, 40) > 8);   // its word
            CHECK(f.near(at[i].x + 20, py - 1, strip, 2) && f.near(at[i].x + 20, py + 24, strip, 2));   // 24 px high
            const auto r = hit(at[i].x + at[i].w / 2, py + 12);
            CHECKV(isOnly(r, "part-pressed " + std::to_string(i)), "part key %zu: %s", i, join(r).c_str());
        }
        CHECK(f.near(at[0].x + at[0].w + 2, py + 12, strip, 2));                               // 4 px between keys
    }

    // ---- the decoders row: the caption, then 20 px keys ----
    {
        const int dy = PY + 93;
        const int x0 = PX + static_cast<int>(std::lround(ui->get_decoders_x0()));
        CHECKV(x0 > PX + 10 + 30 && x0 < PX + 10 + 110, "decoders start %d", x0);
        CHECK(f.countInk(PX + 10, dy, x0 - 6, dy + 20, strip, 40) > 20);               // the caption "DECODERS"
        const auto m = ui->get_m_patch();
        std::vector<int> w;
        std::vector<std::string> labels;
        for (std::size_t i = 0; i < m.decoders->row_count(); ++i) { labels.push_back(std::string(m.decoders->row_data(i)->label)); w.push_back(7 * charCount(labels.back()) + 20); }
        const auto at = flow(w, PX + 10, x0, true, PX + 10 + 610, 4);
        for (std::size_t i = 0; i < at.size(); ++i) {
            CHECK(at[i].row == 0);
            CHECK(f.near(at[i].x + 3, dy + 10, keyBg, 2));
            CHECK(f.near(at[i].x + 20, dy - 1, strip, 2) && f.near(at[i].x + 20, dy + 20, strip, 2));    // xs: 20 px
            const auto r = hit(at[i].x + at[i].w / 2, dy + 10);
            CHECKV(isOnly(r, "decoder-pressed " + std::to_string(i)), "decoder key %zu (%s): %s", i, labels[i].c_str(), join(r).c_str());
        }
        // empty: the host's sentence after the caption, no keys
        auto m2 = ui->get_m_patch();
        auto keep = m2.decoders;
        m2.decoders = std::make_shared<slint::VectorModel<DecoderKey>>();
        m2.no_decoders = "- none installed. Add decoder plugins from the plugin store in DECODE.";
        ui->set_m_patch(m2);
        park();
        f = shot();
        CHECK(f.countInk(x0, dy, PX + 10 + 600, dy + 40, strip, 40) > 40);
        CHECK(!f.near(x0 + 3, dy + 10, keyBg, 2));
        resetEv();
        clk(x0 + 20, dy + 10);
        CHECK(quiet(evs).empty());
        m2.decoders = keep;
        m2.no_decoders = "";
        ui->set_m_patch(m2);
        park();
        f = shot();
        CHECK(f.near(at[0].x + 3, dy + 10, keyBg, 2));
    }

    // ---- the blueprints row: a DropKey with its fixed placeholder, a tall list ----
    {
        const int by = PY + 119;
        CHECK(f.countInk(PX + 10, by, PX + 80, by + 24, strip, 40) > 20);                // the caption "BLUEPRINTS"
        // the drop key: find its left edge on the key row (its border), width 340 px at this page width
        int kx = -1;
        for (int x = PX + 20; x < PX + 200 && kx < 0; ++x) { if (f.near(x, by + 12, keyLine, 4) && f.near(x, by + 6, keyLine, 4)) { kx = x; } }
        CHECKV(kx > 0, "no blueprints key found");
        if (kx > 0) {
            CHECK(f.near(kx + 340 - 1, by + 12, keyLine, 4) && !f.near(kx + 340 + 2, by + 12, keyLine, 4));       // clamp(120, room, 340) = 340
            CHECK(f.countInk(kx + 6, by + 3, kx + 300, by + 21, keyBg, 40) > 60);        // the placeholder, always
            evs.clear();
            clk(kx + 100, by + 12);
            shot();
            CHECK(quiet(evs).size() == 1 && quiet(evs)[0] == "blueprints-opened");
            const auto [rx, ry] = popupRow(kx, by, 1);
            f = shot();
            CHECK(f.countInk(kx + 4, by + 28, kx + 330, by + 28 + 78, menu, 30) > 100);   // the list: three rows of words
            CHECK(f.countNear(kx + 4, by + 28, kx + 330, by + 28 + 78, menu, 4) > 3000);
            evs.clear();
            clk(rx, ry);
            shot();
            CHECK(isOnly(quiet(evs), "blueprint-picked 1"));
            // the placeholder does not change with the pick (the window's combo preview never changes)
            f = shot();
            CHECK(f.countInk(kx + 6, by + 3, kx + 300, by + 21, keyBg, 40) > 60);
            // a long list: up to 20 rows tall (ImGuiComboFlags_HeightLarge)
            auto m = ui->get_m_patch();
            auto keep = m.blueprints;
            auto big = std::make_shared<slint::VectorModel<SelectItem>>();
            for (int i = 0; i < 25; ++i) { SelectItem it; it.label = slint::SharedString("Blueprint number " + std::to_string(i)); it.enabled = true; big->push_back(it); }
            m.blueprints = big;
            ui->set_m_patch(m);
            park();
            shot();
            evs.clear();
            clk(kx + 100, by + 12);
            f = shot();
            const int listTop = by + 24 + 2;
            CHECK(f.near(kx + 200, listTop + 3, menu, 8));
            CHECK(f.near(kx + 200, listTop + 525, menu, 8));                               // 20 rows of 26 px and the frame
            CHECK(!f.near(kx + 200, listTop + 540, menu, 8));
            const auto [r2x, r2y] = popupRow(kx, by, 19);
            evs.clear();
            clk(r2x, r2y);
            shot();
            CHECK(isOnly(quiet(evs), "blueprint-picked 19"));
            m.blueprints = keep;
            ui->set_m_patch(m);
            park();
            f = shot();
        }
        // the note under it, dim; and the row's absence while blueprints-shown is false
        const int ny = PY + 149;
        CHECK(f.countNear(PX + 10, ny, PX + 600, ny + 16, dim, 40) > 30);
        auto m = ui->get_m_patch();
        m.blueprints_shown = false;
        m.blueprint_note = "";
        ui->set_m_patch(m);
        park();
        f = shot();
        CHECK(ui->get_blueprints_y() < 0);
        CHECKV(static_cast<int>(std::lround(ui->get_band_h())) == 93 + 20 + 8, "band without blueprints %d", static_cast<int>(std::lround(ui->get_band_h())));
        m.blueprints_shown = true;
        m.blueprint_note = "Placed \"ADS-B Aircraft Decoder\". Press START to run it.";
        ui->set_m_patch(m);
        park();
        f = shot();
        CHECK(static_cast<int>(std::lround(ui->get_band_h())) == bandH);
    }

    // ---- the information pane: the verdict chip, the lines, the branches ----
    {
        const int ix = PX + PW - infoW;
        CHECK(f.countNear(ix + 8, PY + 8, ix + 250, PY + 28, blendOver(wb.chip_good_bg, strip), 4) > 100);         // runnable: tone good
        CHECK(f.countNear(ix + 8, PY + 8, ix + 250, PY + 28, goodFg, 40) > 20);
        CHECK(f.countNear(ix + 8, PY + 8, ix + infoW - 10, PY + 28, blendOver(wb.chip_err_bg, strip), 4) == 0);
        CHECK(f.countNear(ix + 8, PY + 28, ix + infoW - 10, PY + 50, warnFg, 40) > 20);                                // level-line, amber
        CHECK(f.countNear(ix + 8, PY + 48, ix + infoW - 10, PY + 70, muted, 40) > 20);                                 // plan-line, muted
        CHECK(f.countNear(ix + 8, PY + 64, ix + infoW - 10, PY + 84, fg, 40) > 20);                                    // the selection line
        CHECK(f.countNear(ix + 8, PY + 80, ix + infoW - 10, PY + 130, errFg, 40) > 20);                                // a blocking problem: rust
        CHECK(f.countNear(ix + 8, PY + 80, ix + infoW - 10, PY + 130, dim, 40) > 20);                                  // an advisory one: dim
        // not runnable: tone err, the sentence the host sends; no level line, no plan line
        auto m = ui->get_m_patch();
        auto info = m.info;
        info.runnable = false;
        info.verdict = "This patch cannot run yet.";
        info.level_line = "";
        info.plan_line = "";
        m.info = info;
        ui->set_m_patch(m);
        park();
        f = shot();
        CHECK(f.countNear(ix + 8, PY + 8, ix + 250, PY + 28, blendOver(wb.chip_err_bg, strip), 4) > 100);
        CHECK(f.countNear(ix + 8, PY + 8, ix + 250, PY + 28, errFg, 40) > 20);
        CHECK(f.countNear(ix + 8, PY + 28, ix + infoW - 10, PY + 62, warnFg, 40) == 0);
        // none selected: the three help sentences; no selection line and no problems
        info.none_selected = true;
        info.selection = "";
        info.problems = std::make_shared<slint::VectorModel<ProblemLine>>();
        auto help = std::make_shared<slint::VectorModel<slint::SharedString>>();
        help->push_back("Nothing selected.");
        help->push_back("Click a node to set what it does. Drag from one port to another to wire them; drag a node by its title bar to move it. Delete removes whatever is selected.");
        help->push_back("Drag empty canvas or scroll to move round the patch. Ctrl+scroll or the + and - keys zoom; Fit shows every node.");
        info.help = help;
        m.info = info;
        ui->set_m_patch(m);
        park();
        f = shot();
        CHECK(f.countNear(ix + 8, PY + 30, ix + infoW - 10, PY + 160, dim, 40) > 200);                                  // wrapped, dim 11 px
        CHECK(f.countNear(ix + 8, PY + 30, ix + infoW - 10, PY + 160, fg, 40) == 0);
        // a selection while the inspector is folded: the folded note under it
        info.none_selected = false;
        info.selection = "CHANNEL  Channel";
        info.folded_note = "Its settings are folded away at the right of the canvas - press << to open them.";
        m.info = info;
        m.inspector_folded = true;
        ui->set_m_patch(m);
        park();
        f = shot();
        CHECK(f.countNear(ix + 8, PY + 30, ix + infoW - 10, PY + 50, fg, 40) > 20);
        CHECK(f.countNear(ix + 8, PY + 48, ix + infoW - 10, PY + 90, dim, 40) > 40);
        m.inspector_folded = false;
        info.folded_note = "";
        info.selection = "RADIO  Radio";
        info.runnable = true;
        info.verdict = "This patch can run.";
        info.level_line = "-61.4 dB on this channel";
        info.plan_line = "1 channel(s), decimate by 8";
        auto probs = std::make_shared<slint::VectorModel<ProblemLine>>();
        probs->push_back(ProblemLine{"no frequency set", false});
        probs->push_back(ProblemLine{"nothing is listening to this", true});
        info.problems = probs;
        m.info = info;
        ui->set_m_patch(m);
        park();
        f = shot();
    }

    // ======================================================== the canvas ========================================================
    const int CX = PX, CY = PY + canvasOy;
    f = shot();
    // a debugging aid: FOXSDR_SLINT_DUMP="x,y,w,h" prints the colours of a region of the first canvas frame, row by row
    if (const char* d = std::getenv("FOXSDR_SLINT_DUMP"); d != nullptr) {
        int dx = 0, dy = 0, dw = 0, dh = 0;
        if (std::sscanf(d, "%d,%d,%d,%d", &dx, &dy, &dw, &dh) == 4) {
            for (int y = dy; y < dy + dh; ++y) {
                std::printf("y%4d:", y);
                for (int x = dx; x < dx + dw; ++x) { std::printf(" %s", hex(f.at(x, y)).c_str()); }
                std::printf("\n");
            }
        }
    }
    // the grid: lines at x = 6 + 26k and y = 14 + 26k in line-faint over the deep well, none while grid-step is 0
    CHECK(f.near(CX + 600, CY + 340, wellDeep, 2));
    CHECK(f.near(CX + 6 + 26 * 23, CY + 340, lineFaint, 3));
    CHECK(f.near(CX + 600, CY + 14 + 26 * 12, lineFaint, 3));
    CHECK(f.near(CX + 6 + 26 * 23 - 1, CY + 340, wellDeep, 2));
    {
        auto c = ui->get_m_canvas();
        c.grid_step = 0;
        ui->set_m_canvas(c);
        park();
        f = shot();
        CHECK(f.near(CX + 6 + 26 * 23, CY + 340, wellDeep, 2) && f.near(CX + 600, CY + 14 + 26 * 12, wellDeep, 2));
        c.grid_step = 26;
        ui->set_m_canvas(c);
        park();
        f = shot();
        CHECK(f.near(CX + 6 + 26 * 23, CY + 340, lineFaint, 3));
    }

    // ---- the plates: ground, outline, header, close key, grip, well ----
    // node 2 (Channel): plain; node 1 (Radio): selected; node 3 (Demod): blocked
    CHECK(f.near(CX + 250, CY + 80, lineStrong, 3) && f.near(CX + 430 - 1, CY + 80, lineStrong, 3));                  // 1 px border
    CHECK(!f.near(CX + 249, CY + 80, lineStrong, 3) && f.near(CX + 252, CY + 112, panel, 3));                        // panel plate
    CHECK(f.near(CX + 20, CY + 100, fg, 8) && f.near(CX + 21, CY + 100, fg, 8) && !f.near(CX + 22, CY + 100, fg, 8));  // selected: 2 px ivory
    CHECK(f.near(CX + 460, CY + 60, lampRed, 8) && f.near(CX + 461, CY + 60, lampRed, 8) && !f.near(CX + 462, CY + 60, lampRed, 8));   // blocked: 2 px red
    CHECK(f.near(CX + 20 + 100, CY + 20, fg, 8) && f.near(CX + 20 + 100, CY + 20 + 150 - 1, fg, 8));                  // the selected outline is a full rectangle
    {
        // the header: 22 px of key-bg, the name left, the kind caption after it
        CHECK(f.near(CX + 405, CY + 26, keyBg, 3));
        CHECK(f.near(CX + 428, CY + 41, keyBg, 3) && f.near(CX + 428, CY + 43, panel, 3));                           // beside the well the header is 22 px                                                                  // below the header: the plate
        CHECK(f.countInk(CX + 256, CY + 24, CX + 306, CY + 38, keyBg, 40) > 20);                                     // "Channel"
        CHECK(f.countNear(CX + 256, CY + 24, CX + 306, CY + 38, fg, 40) > 10);                                       // in fg
        CHECK(f.countNear(CX + 312, CY + 24, CX + 380, CY + 38, muted, 40) > 10);                                    // the caption, muted
        // the close key: an X of two lines, muted, fg while hot
        CHECK_GT(f.countNear(CX + 203, CY + 25, CX + 215, CY + 37, muted, 40), 8);                                    // node 1: not hot, muted
        CHECK(f.countNear(CX + 203, CY + 25, CX + 215, CY + 37, fg, 30) == 0);
        CHECK_GT(f.countNear(CX + 413, CY + 25, CX + 425, CY + 37, fg, 30), 8);                                       // node 2: hot, ivory
        CHECK(f.countNear(CX + 413, CY + 25, CX + 425, CY + 37, muted, 30) == 0);
        // the grip: a filled right triangle at the bottom right; accent-soft, fg while hot (node 4)
        CHECK(f.near(CX + 655, CY + 155, accentSoft, 20));                                                            // node 3, not hot: its corner at (660, 160)
        CHECK(f.near(CX + 875, CY + 125, fg, 20));                                                                    // node 4, hot
        CHECK(!f.near(CX + 650, CY + 148, accentSoft, 20));                                                          // above the hypotenuse
        // the well: the face rectangle, (6, 20) to (w - 6, h - 16)
        CHECK(f.near(CX + 300, CY + 100, well, 2));
        CHECK(f.near(CX + 256, CY + 70, well, 2) && !f.near(CX + 255, CY + 70, well, 2));                           // left edge at x + 6
        CHECK(f.near(CX + 423, CY + 70, well, 2) && !f.near(CX + 424 + 1, CY + 70, well, 2));                       // right edge at x + w - 6
        CHECK(f.near(CX + 300, CY + 103, well, 2) && !f.near(CX + 300, CY + 105, well, 2));                           // bottom edge at y + h - 16 = 104
        // the readings: amber then phosphor
        CHECK(f.countNear(CX + 258, CY + 42, CX + 330, CY + 58, warnFg, 40) > 8);                                    // node 2: reading, amber
        CHECK(f.countNear(CX + 26, CY + 212, CX + 120, CY + 228, warnFg, 40) > 8);                                   // node 5: "3 lines"
        CHECK(f.countNear(CX + 26, CY + 230, CX + 214, CY + 246, goodFg, 40) > 8);                                   // reading2 phosphor
    }

    // ---- the zoom scales the header (22z), the close key and the grip; the host sends the node's pixels ----
    {
        auto c = ui->get_m_canvas();
        c.zoom = 2.0f;
        ui->set_m_canvas(c);
        park();
        const Frame g = shot();
        CHECKV(g.near(CX + 23, CY + 190 + 42, keyBg, 3) && g.near(CX + 23, CY + 190 + 46, panel, 3), "header at zoom 2: %s %s", hex(g.at(CX + 23, CY + 190 + 42)).c_str(), hex(g.at(CX + 23, CY + 190 + 46)).c_str());   // beside the well the header is 44 px
        CHECK_GT(g.countNear(CX + 250 + 180 - 8 - 28, CY + 20 + 8, CX + 250 + 180 - 8, CY + 20 + 36, fg, 30), 20);     // the close key: 28 px square, 8 px in, ivory (hot)
        CHECK(g.near(CX + 250 + 180 - 4 - 5, CY + 20 + 100 - 4 - 5, accentSoft, 20));                                  // the grip: legs of 24 px
        c.zoom = 1.0f;
        ui->set_m_canvas(c);
        park();
        f = shot();
    }
    // LATER IS ON TOP: the Display node, selected and moved over the Map node, is under it; swapped in the model, it is over it
    {
        editNode(6, [](CanvasNode& n) { n.x = 640; n.selected = true; });
        park();
        Frame g = shot();
        CHECK(!g.near(CX + 799, CY + 250, fg, 20));
        auto c = ui->get_m_canvas();
        const auto a = *c.nodes->row_data(6), b = *c.nodes->row_data(7);
        c.nodes->set_row_data(6, b);
        c.nodes->set_row_data(7, a);
        park();
        g = shot();
        CHECK(g.near(CX + 799, CY + 250, fg, 20));
        c.nodes->set_row_data(6, a);
        c.nodes->set_row_data(7, b);
        editNode(6, [](CanvasNode& n) { n.x = 540; n.selected = false; });
        park();
        f = shot();
    }

    // ---- the ports: a dot of radius 4 by tone, a ring of `line` ----
    CHECK(f.near(CX + 220, CY + 95, lampGreen, 8));                                  // Radio out, I/Q
    CHECK(f.near(CX + 250, CY + 70, lampGreen, 8));                                  // Channel in
    CHECK(f.near(CX + 660, CY + 90, lampAmber, 8));                                  // Demod out, audio
    CHECK(f.near(CX + 690, CY + 75, lampAmber, 8));                                  // Speaker in
    CHECK(f.near(CX + 250, CY + 245, fg, 8));                                        // Text sink in: text
    CHECK(f.near(CX + 730, CY + 240, bookmark, 8));                                  // Map in: track
    CHECK(f.near(CX + 220, CY + 96, lampGreen, 8) && !f.near(CX + 220, CY + 102, lampGreen, 8));                      // radius 4 (a 1 px ring beyond)
    {
        editNode(6, [](CanvasNode& n) {
            auto in = std::make_shared<slint::VectorModel<PortDot>>();
            in->push_back(PortDot{540, 240, 3});
            n.inputs = in;
        });
        park();
        f = shot();
        CHECK(f.near(CX + 540, CY + 240, focus, 8));                                  // control
        editNode(6, [](CanvasNode& n) {
            auto in = std::make_shared<slint::VectorModel<PortDot>>();
            in->push_back(PortDot{540, 240, 0});
            n.inputs = in;
        });
    }

    // ---- the wires: a Path per wire, stroke by tone, the selected one in fg and thicker ----
    {
        park();
        f = shot();
        const Rgb tones[5] = {lampGreen, lampAmber, fg, focus, bookmark};
        for (int k = 0; k < 5; ++k) {
            const int y = CY + 400 + 14 * k;
            CHECKV(f.countNear(CX + 60, y - 2, CX + 80, y + 3, tones[k], 40) >= 20, "wire tone %d", k);
            CHECK(f.countNear(CX + 60, y - 2, CX + 80, y + 3, tones[(k + 1) % 5], 12) == 0);
            // 1.8 px at zoom 1: the stroke is two rows deep at most
            int rows = 0;
            for (int yy = y - 4; yy <= y + 4; ++yy) { if (!f.near(CX + 70, yy, wellDeep, 40)) { ++rows; } }
            CHECKV(rows >= 2 && rows <= 3, "wire %d is %d rows deep", k, rows);
        }
        // selected: fg and 3 px
        int deep = 0;
        for (int yy = CY + 420; yy <= CY + 436; ++yy) { if (!f.near(CX + 230, yy, wellDeep, 40)) { ++deep; } }
        CHECKV(deep >= 4 && deep <= 5, "the selected wire is %d rows deep", deep);
        { int full = 0; for (int yy = CY + 424; yy <= CY + 431; ++yy) { if (f.near(CX + 230, yy, fg, 30)) { ++full; } } CHECKV(full >= 2, "ivory rows %d", full); }
        CHECK(f.countNear(CX + 200, CY + 424, CX + 260, CY + 433, lampAmber, 12) == 0);
        // the wires run under the nodes: the Channel -> Demod wire (selected) is fg between the two nodes
        CHECK(f.countNear(CX + 432, CY + 66, CX + 458, CY + 94, fg, 30) > 10);
        // the ghost wire (the one being dragged): dim, 1.6 px; none while empty
        CHECK(f.countNear(CX + 320, CY + 396, CX + 380, CY + 404, dim, 8) == 0);
        auto c = ui->get_m_canvas();
        c.ghost = "M 300 400 L 400 400";
        ui->set_m_canvas(c);
        park();
        f = shot();
        CHECK(f.countNear(CX + 320, CY + 396, CX + 380, CY + 404, dim, 30) > 20);
        c.ghost = "";
        ui->set_m_canvas(c);
        park();
        f = shot();
        CHECK(f.countNear(CX + 320, CY + 396, CX + 380, CY + 404, dim, 8) == 0);
    }

    // ---- the refusal bubble: rust on card-err with a red hairline, at the host's x, y ----
    {
        CHECK(f.near(CX + 345, CY + 390, cardErr, 3));
        CHECK(f.near(CX + 340, CY + 390, lampRed, 8) && !f.near(CX + 338, CY + 390, lampRed, 20));
        CHECK(f.countNear(CX + 346, CY + 382, CX + 520, CY + 398, errFg, 40) > 20);
        auto c = ui->get_m_canvas();
        auto r = c.refusal;
        r.shown = false;
        c.refusal = r;
        ui->set_m_canvas(c);
        park();
        f = shot();
        CHECK(!f.near(CX + 345, CY + 390, cardErr, 3));
        CHECK(f.countNear(CX + 346, CY + 382, CX + 520, CY + 398, errFg, 40) == 0);
        r.shown = true;
        c.refusal = r;
        ui->set_m_canvas(c);
        park();
        f = shot();
    }

    // ---- the canvas keys: "-", "+", "Fit", bottom right, 10 px in ----
    int kMinus = 0, kPlus = 0, kFit = 0, kY = 0;
    {
        const int right = CX + cw - 10;
        kY = CY + ch - 10 - 14;
        kFit = right - 3 - 22;                       // 44 px min
        kPlus = right - 3 - 44 - 2 - 14;
        kMinus = kPlus - 28 - 2;
        CHECK(f.near(right - 1, kY, lineStrong, 6) && f.near(right - 2, kY, panelAlt, 3));                      // the group's frame, then its ground
        CHECK(f.near(kFit - 18, kY - 12, panelAlt, 3) && f.near(kFit - 18, kY - 14, lineStrong, 6));         // 28 px high: a small SegGroup
        CHECK(f.countInk(kMinus - 10, kY - 8, kMinus + 10, kY + 8, panelAlt, 40) > 3);
        CHECK(f.countInk(kPlus - 10, kY - 8, kPlus + 10, kY + 8, panelAlt, 40) > 6);
        CHECK(f.countInk(kFit - 14, kY - 8, kFit + 14, kY + 8, panelAlt, 40) > 8);
        evs.clear();
        clk(kMinus, kY);
        CHECKV(isOnly(quiet(evs), "zoom-step 0"), "%s", join(quiet(evs)).c_str());
        evs.clear();
        clk(kPlus, kY);
        CHECK(isOnly(quiet(evs), "zoom-step 1"));
        evs.clear();
        clk(kFit, kY);
        CHECK(isOnly(quiet(evs), "fit"));
        // hidden while keys-shown is false: no group, no clicks
        auto c = ui->get_m_canvas();
        c.keys_shown = false;
        ui->set_m_canvas(c);
        park();
        f = shot();
        CHECK(f.countNear(kMinus - 20, kY - 14, right, kY + 14, panelAlt, 3) == 0 && f.countNear(kMinus - 20, kY - 14, right, kY + 14, lineStrong, 3) == 0);
        evs.clear();
        clk(kFit, kY);
        clk(kPlus, kY);
        CHECK(countOf(evs, "zoom-step") == 0 && countOf(evs, "fit") == 0);
        c.keys_shown = true;
        ui->set_m_canvas(c);
        park();
        f = shot();
    }

    // ---- the pointer is one stream: press / move / release in canvas-local pixels, button 0 left, 1 right, 2 middle ----
    {
        park();
        evs.clear();
        mv(CX + 700, CY + 400);
        press(win, static_cast<float>(CX + 700), static_cast<float>(CY + 400));
        mv(CX + 705, CY + 405);
        mv(CX + 710, CY + 410);
        release(win, static_cast<float>(CX + 710), static_cast<float>(CY + 410));
        shot();
        CHECK(countOf(evs, "canvas-pressed") == 1 && lastOf(evs, "canvas-pressed").args == "700 400 0");
        CHECK(countOf(evs, "canvas-released") == 1 && lastOf(evs, "canvas-released").args == "710 410 0");
        CHECK(countOf(evs, "canvas-moved") >= 2 && lastOf(evs, "canvas-moved").args == "710 410");
        {
            // the order: pressed, then the moves, then released
            int pi = -1, ri = -1, mi = -1;
            for (std::size_t i = 0; i < evs.size(); ++i) {
                if (evs[i].name == "canvas-pressed") { pi = static_cast<int>(i); }
                if (evs[i].name == "canvas-released") { ri = static_cast<int>(i); }
                if (evs[i].name == "canvas-moved" && evs[i].args == "705 405") { mi = static_cast<int>(i); }
            }
            CHECK(pi >= 0 && mi > pi && ri > mi);
        }
        evs.clear();
        press(win, static_cast<float>(CX + 700), static_cast<float>(CY + 400), slint::PointerEventButton::Right);
        release(win, static_cast<float>(CX + 700), static_cast<float>(CY + 400), slint::PointerEventButton::Right);
        press(win, static_cast<float>(CX + 701), static_cast<float>(CY + 401), slint::PointerEventButton::Middle);
        release(win, static_cast<float>(CX + 703), static_cast<float>(CY + 402), slint::PointerEventButton::Middle);
        shot();
        CHECK(countOf(evs, "canvas-pressed") == 2);
        std::vector<std::string> pr, rl;
        for (const Ev& e : evs) { if (e.name == "canvas-pressed") { pr.push_back(e.args); } if (e.name == "canvas-released") { rl.push_back(e.args); } }
        CHECKV(pr == std::vector<std::string>({"700 400 1", "701 401 2"}), "presses: %s", join(pr).c_str());
        CHECKV(rl == std::vector<std::string>({"700 400 1", "703 402 2"}), "releases: %s", join(rl).c_str());
        // the pointer leaving the canvas
        evs.clear();
        mv(CX + 700, CY + 300);
        mv(PX + 300, PY + 20);
        shot();
        CHECK(countOf(evs, "canvas-left") >= 1);
        // a node's face widget takes its own press: nothing reaches canvas-pressed (the Radio's switch)
        evs.clear();
        clk(CX + 31 + 20, CY + 44 + 12);
        shot();
        CHECK(countOf(evs, "canvas-pressed") == 0 && countOf(evs, "face-switch") == 1);
    }

    // ---- the wheel: notches (120 px = one), ctrl, shift ----
    {
        evs.clear();
        scroll(win, static_cast<float>(CX + 700), static_cast<float>(CY + 400), 0.f, -120.f);
        CHECKV(lastOf(evs, "canvas-wheel").args == "700 400 0 -1 0 0", "wheel: %s", lastOf(evs, "canvas-wheel").args.c_str());
        evs.clear();
        scroll(win, static_cast<float>(CX + 700), static_cast<float>(CY + 400), 0.f, 60.f);
        CHECKV(lastOf(evs, "canvas-wheel").args == "700 400 0 0.5 0 0", "a touchpad's half notch: %s", lastOf(evs, "canvas-wheel").args.c_str());
        evs.clear();
        keyDown(win, slint::platform::key_codes::Control);
        scroll(win, static_cast<float>(CX + 650), static_cast<float>(CY + 380), 0.f, 120.f);
        keyUp(win, slint::platform::key_codes::Control);
        CHECKV(lastOf(evs, "canvas-wheel").args == "650 380 0 1 1 0", "ctrl wheel: %s", lastOf(evs, "canvas-wheel").args.c_str());
        evs.clear();
        keyDown(win, slint::platform::key_codes::Shift);
        scroll(win, static_cast<float>(CX + 650), static_cast<float>(CY + 380), 0.f, -240.f);
        keyUp(win, slint::platform::key_codes::Shift);
        CHECKV(lastOf(evs, "canvas-wheel").args == "650 380 0 -2 0 1", "shift wheel: %s", lastOf(evs, "canvas-wheel").args.c_str());
        evs.clear();
        scroll(win, static_cast<float>(CX + 650), static_cast<float>(CY + 380), 120.f, 0.f);
        CHECKV(lastOf(evs, "canvas-wheel").args == "650 380 1 0 0 0", "sideways: %s", lastOf(evs, "canvas-wheel").args.c_str());
    }

    // ---- the Delete key, through the FocusScope the canvas takes on a press (not Backspace) ----
    {
        clk(CX + 700, CY + 400);
        evs.clear();
        tap(slint::platform::key_codes::Delete);
        shot();
        CHECKV(isOnly(quiet(evs), "canvas-delete"), "%s", join(quiet(evs)).c_str());
        evs.clear();
        tap(slint::platform::key_codes::Backspace);
        shot();
        CHECK(quiet(evs).empty());
        // a text box that holds the focus keeps its own Delete
        clk(CX + 31 + 60, CY + 71 + 12);
        evs.clear();
        tap(slint::platform::key_codes::Delete);
        shot();
        CHECK(countOf(evs, "canvas-delete") == 0);
        clk(CX + 700, CY + 400);
    }

    // ======================================================== the faces ========================================================
    // zoom 1, a face starts 5 px in from the well's left and 4 px down (a Channel's at 22), 24 px controls, 3 px apart
    {
        const int fx = CX + 20 + 11, fy = CY + 20 + 24;                      // node 1: well at (+6, +20); the face (+5, +4)
        // RADIO: the switch Key at the left, tinted by its state; the device label beside it
        CHECK(f.near(fx + 3, fy + 3, over(lampGreen, 0.45, well), 14));            // open: lamp-green at 45 %
        CHECK(f.countInk(fx + 6, fy + 4, fx + 36, fy + 20, over(lampGreen, 0.45, well), 40) > 8);                        // "ON"
        CHECK(f.countNear(fx + 46, fy, fx + 178, fy + 30, fgSoft, 40) > 20);                                           // "RTL-SDR #0"
        CHECKV(isOnly(hit(fx + 20, fy + 12), "face-switch 1"), "switch");
        // waiting: lamp-amber at 35 %; off: the plain key ground
        editNode(0, [](CanvasNode& n) { auto fm = n.face; fm.radio_switch = 1; fm.radio_switch_label = "ON"; n.face = fm; });
        park();
        f = shot();
        CHECK(f.near(fx + 3, fy + 3, over(lampAmber, 0.35, well), 14));
        editNode(0, [](CanvasNode& n) { auto fm = n.face; fm.radio_switch = 0; fm.radio_switch_label = "OFF"; n.face = fm; });
        park();
        f = shot();
        CHECK(f.near(fx + 3, fy + 3, keyBg, 6));
        editNode(0, [](CanvasNode& n) { auto fm = n.face; fm.radio_switch = 2; fm.radio_switch_label = "ON"; n.face = fm; });
        park();
        f = shot();
        // the centre box: field ground, mono, right-aligned, " MHz" after the figure; Enter or the focus leaving commits
        const int bx = fx, by = fy + 24 + 3;
        CHECK(f.near(bx + 4, by + 12, field, 3));
        CHECK(f.countNear(bx + 100, by + 3, bx + 177, by + 21, fg, 40) > 30);                                           // the figure and its suffix
        typeInto(bx + 60, by + 12, "145.5", true);
        CHECKV(isOnly(quiet(evs), "face-centre 1 145.5"), "%s", join(quiet(evs)).c_str());
        typeInto(bx + 60, by + 12, "88.8", false);
        CHECK(quiet(evs).empty());
        clk(CX + 700, CY + 400);                                            // the focus leaves: the typed text is committed
        shot();
        CHECKV(countOf(evs, "face-centre") == 1 && lastOf(evs, "face-centre").args == "1 88.8", "%s", join(quiet(evs)).c_str());
        // the note under it, amber; the lines under that
        CHECK(f.countNear(bx, by + 24, bx + 178, by + 42, warnFg, 40) > 30);
        CHECK(f.countNear(bx, by + 40, bx + 178, by + 70, warnFg, 40) > 20);
    }
    {
        // CHANNEL: the frequency box at 22 px under the well's top, the reading line above it
        const int fx = CX + 250 + 11, fy = CY + 20 + 20 + 22;
        CHECK(f.near(fx + 4, fy + 12, field, 3));
        CHECK(f.countNear(fx + 60, fy + 3, fx + 157, fy + 21, fg, 40) > 30);
        typeInto(fx + 60, fy + 12, "99.5", true);
        CHECKV(isOnly(quiet(evs), "face-freq 2 99.5"), "%s", join(quiet(evs)).c_str());
    }
    {
        // DEMOD: the mode DropKey of kModeNames, the Squelch tick, the slider and the gate
        const int fx = CX + 460 + 11, fy = CY + 20 + 24;
        CHECK(f.countInk(fx + 4, fy + 3, fx + 170, fy + 21, keyBg, 40) > 10);                                           // "WFM"
        evs.clear();
        clk(fx + 30, fy + 12);
        shot();
        CHECK(quiet(evs).empty());
        f = shot();
        CHECK(f.countInk(fx + 4, fy + 28, fx + 100, fy + 28 + 8 * 26, menu, 30) > 150);                                  // the eight names
        const auto [mx, my] = popupRow(fx, fy, 4);
        evs.clear();
        clk(mx, my);
        shot();
        CHECKV(isOnly(quiet(evs), "face-mode 3 4"), "%s", join(quiet(evs)).c_str());
        // the tick
        const int ty = fy + 24 + 3;
        park();
        f = shot();
        CHECK(f.near(fx + 2, ty + 4, accent, 6));                                                                       // squelch-on: the box is lit
        CHECKV(isOnly(hit(fx + 6, ty + 8), "face-squelch-on 3 0"), "tick");
        park();
        f = shot();
        CHECK(f.near(fx + 2, ty + 4, accent, 6));                                                                       // the host owns the value: still on
        // the slider (while on): 25 % of the track
        const int sy = ty + 16 + 3;
        const int sx = fx + 8, sw = 178 - 8 - 8 - 48 - 4;
        CHECK(f.countNear(sx, sy, sx + sw, sy + 16, tok(wb.slider_fill), 6) > 100);
        evs.clear();
        clk(sx + sw / 4, sy + 8);
        shot();
        {
            const Ev e = lastOf(evs, "face-squelch");
            CHECKV(e.name == "face-squelch" && e.args.rfind("3 ", 0) == 0 && std::fabs(std::atof(e.args.c_str() + 2) - 0.25) < 0.02, "face-squelch: %s", e.args.c_str());
        }
        CHECK(f.countNear(fx + 178 - 48, sy, fx + 178, sy + 16, fgSoft, 60) + f.countNear(fx + 178 - 48, sy, fx + 178, sy + 16, dim, 60) > 15);   // "-60 dB"
        // the gate: amber figure, phosphor word while lit
        const int gy = sy + 16 + 3;
        CHECK_GT(f.countNear(fx, gy, fx + 56, gy + 16, warnFg, 70), 6);
        CHECK_GT(f.countNear(fx + 62, gy, fx + 178, gy + 16, goodFg, 70), 6);
        editNode(2, [](CanvasNode& n) { auto fm = n.face; fm.gate_lit = false; fm.gate_word = "closed"; n.face = fm; });
        park();
        f = shot();
        CHECK(f.countNear(fx + 62, gy, fx + 178, gy + 16, goodFg, 40) == 0);
        CHECK_GT(f.countNear(fx + 62, gy, fx + 178, gy + 16, dim, 70), 6);
        // squelch off: no slider, no gate-less change
        editNode(2, [](CanvasNode& n) { auto fm = n.face; fm.squelch_on = false; fm.gate_shown = false; n.face = fm; });
        park();
        f = shot();
        CHECK(f.countNear(sx, sy, sx + sw, sy + 16, tok(wb.slider_fill), 6) == 0 && f.countNear(sx, sy, sx + sw, sy + 16, sliderTrack, 4) == 0);
        editNode(2, [](CanvasNode& n) { auto fm = n.face; fm.squelch_on = true; fm.gate_shown = true; fm.gate_lit = true; fm.gate_word = "open"; n.face = fm; });
        park();
        f = shot();
    }
    {
        // SINK (audio): the channel's name (phosphor), where it goes, the level bar 5 px high, the errors in rust
        const int fx = CX + 690 + 11, fy = CY + 20 + 24;
        CHECK(f.countNear(fx, fy, fx + 168, fy + 16, goodFg, 40) > 15);
        CHECK(f.countNear(fx, fy + 14, fx + 168, fy + 32, dim, 40) > 15);
        CHECK(f.countNear(fx, fy, fx + 168, fy + 80, levelBar, 4) > 5 * 90 && f.countNear(fx, fy, fx + 168, fy + 80, levelBar, 4) < 5 * 112);   // 0.6 of 168 px, 5 rows
        CHECK(f.countNear(fx, fy, fx + 168, fy + 80, sliderTrack, 4) > 5 * 55);                                           // the rest of the track
        CHECK(f.countNear(fx, fy + 40, fx + 168, fy + 80, errFg, 40) > 15);
        editNode(3, [](CanvasNode& n) { auto fm = n.face; fm.level_shown = false; n.face = fm; });
        park();
        f = shot();
        { const int a = f.countNear(fx, fy + 28, fx + 168, fy + 44, levelBar, 4), b = f.countNear(fx, fy + 28, fx + 168, fy + 44, sliderTrack, 4); CHECKV(a == 0 && b == 0, "bar colours %d, %d", a, b); }
        editNode(3, [](CanvasNode& n) { auto fm = n.face; fm.level_shown = true; n.face = fm; });
        park();
        f = shot();
        // SINK (text): the lines, no wrap, cut at the face edge
        const int tx = CX + 250 + 11, tyy = CY + 190 + 24;
        CHECK(f.countNear(tx, tyy, tx + 238, tyy + 16, fgSoft, 40) > 20 && f.countNear(tx, tyy + 28, tx + 238, tyy + 46, fgSoft, 40) > 20);
        editNode(5, [](CanvasNode& n) {
            auto fm = n.face;
            auto ls = std::make_shared<slint::VectorModel<FaceLine>>();
            ls->push_back(FaceLine{"APRS: a line that runs on far beyond the edge of the face without ever wrapping onto a second line at all", 0});
            fm.lines = ls;
            n.face = fm;
        });
        park();
        f = shot();
        CHECK(f.countInk(tx, tyy, tx + 238, tyy + 16, well, 24) > 100);
        CHECK(f.countInk(tx + 238, tyy, tx + 243, tyy + 16, well, 24) == 0);                    // nothing past the face's edge (the well ends 5 px later)
        CHECK(f.countInk(tx, tyy + 16, tx + 238, tyy + 40, well, 24) == 0);                     // no second line
        // DECODER: the picture, fitted and centred under the reading line
        CHECK(f.countInk(CX + 30, CY + 250, CX + 210, CY + 330, well, 24) > 300);
        editNode(4, [](CanvasNode& n) { auto fm = n.face; fm.have_picture = false; n.face = fm; });
        park();
        f = shot();
        CHECK(f.countInk(CX + 30, CY + 250, CX + 210, CY + 330, well, 24) == 0);
        editNode(4, [](CanvasNode& n) { auto fm = n.face; fm.have_picture = true; n.face = fm; });
        // DISPLAY and MAP: their lines only
        CHECK(f.countNear(CX + 546, CY + 214, CX + 690, CY + 270, dim, 40) > 30);
        CHECK(f.countNear(CX + 736, CY + 214, CX + 860, CY + 240, warnFg, 40) > 15);
    }
    {
        // faces are hidden while faces-live is false, and while the face is under 40 px wide
        auto c = ui->get_m_canvas();
        c.faces_live = false;
        ui->set_m_canvas(c);
        park();
        f = shot();
        const int fx = CX + 20 + 11, fy = CY + 20 + 24;
        CHECK(f.near(fx + 3, fy + 3, well, 3) && f.near(fx + 60, fy + 24 + 3 + 12, well, 3));
        evs.clear();
        clk(fx + 20, fy + 12);
        CHECK(countOf(evs, "face-switch") == 0);
        CHECK(f.countInk(CX + 256, CY + 40, CX + 424, CY + 100, well, 24) <= f.countInk(CX + 256, CY + 40, CX + 424, CY + 100, well, 24));
        c.faces_live = true;
        ui->set_m_canvas(c);
        editNode(1, [](CanvasNode& n) { n.w = 60; });
        park();
        f = shot();
        CHECK(f.near(CX + 250 + 11 + 4, CY + 62 + 12, well, 3));                          // w - 22 = 38 < 40: no face
        editNode(1, [](CanvasNode& n) { n.w = 180; });
        park();
        f = shot();
        CHECK(f.near(CX + 250 + 11 + 4, CY + 62 + 12, field, 3));
        // below zoom 0.5 the close key goes; below 11 * z < 5 px the header words go; the readings below 13 * z < 6 px
        c.zoom = 0.4f;
        ui->set_m_canvas(c);
        park();
        f = shot();
        CHECK(f.countInk(CX + 256, CY + 24, CX + 306, CY + 30, keyBg, 40) == 0);
        CHECK(f.countNear(CX + 415, CY + 27, CX + 425, CY + 37, fg, 30) == 0);
        CHECK(f.countNear(CX + 258, CY + 42, CX + 330, CY + 58, warnFg, 40) == 0);
        c.zoom = 1.0f;
        ui->set_m_canvas(c);
        park();
        f = shot();
        CHECK(f.countNear(CX + 258, CY + 42, CX + 330, CY + 58, warnFg, 40) > 8);
    }

    // ======================================================== the inspector (Radio selected) ========================================================
    const int IX = PX + PW - 190;
    const int blockY = static_cast<int>(std::lround(ui->get_inspector_block_y()));
    const int nameY = static_cast<int>(std::lround(ui->get_inspector_name_y()));
    const int sizeY = static_cast<int>(std::lround(ui->get_inspector_size_y()));
    const int defaultY = static_cast<int>(std::lround(ui->get_inspector_default_y()));
    const int removeY = static_cast<int>(std::lround(ui->get_inspector_remove_y()));
    const int IY = CY;           // the inspector's top
    f = shot();
    CHECK(f.near(IX + 100, IY + ch - 6, panelAlt, 2));                                            // ground
    CHECK(f.colNear(IX, IY + 2, IY + ch - 2, line, 2));                                            // left hairline
    CHECK(f.near(IX - 4, IY + 100, bg, 2));                                                        // the 8 px gap shows the page
    CHECKV(nameY > 0 && blockY > nameY && sizeY > blockY && defaultY > sizeY && removeY > defaultY, "anchors %d %d %d %d %d", nameY, blockY, sizeY, defaultY, removeY);
    {
        CHECK(f.countNear(IX + 8, IY + 6, IX + 100, IY + 30, fg, 40) > 15);                         // "RADIO", Caption in fg
        CHECK(isOnly(hit(IX + 175, IY + 8 + 10), "inspector-fold"));
        // the name: every keystroke is reported
        typeInto(IX + 8 + 60, IY + nameY + 12, "Rx2", false, 12);
        const auto names = quiet(evs);
        CHECKV(!names.empty() && names.back() == "inspector-name Rx2" && countOf(evs, "inspector-name") >= 3, "name events: %s", join(names).c_str());
        // the device: a DropKey; opening it asks for the list, a greyed row cannot be picked
        const int dy = IY + blockY + 20;
        CHECK(f.countInk(IX + 12, dy + 3, IX + 160, dy + 21, keyBg, 40) > 30);
        evs.clear();
        clk(IX + 8 + 40, dy + 12);
        f = shot();
        CHECK(isOnly(quiet(evs), "inspector-device-opened"));
        CHECK(f.countInk(IX + 10, dy + 28, IX + 180, dy + 28 + 78, menu, 30) > 150);
        evs.clear();
        const auto [r1x, r1y] = popupRow(IX + 8, dy, 1);
        clk(r1x, r1y);
        shot();
        CHECK(quiet(evs).empty());                                                                    // a held device is greyed
        const auto [r2x, r2y] = popupRow(IX + 8, dy, 2);
        clk(r2x, r2y);
        shot();
        CHECK(isOnly(quiet(evs), "inspector-device 2"));
        // Look for radios, and "looking..." while it does
        const int ky = dy + 24 + 6;
        CHECK(f.near(IX + 8 + 4, ky + 12, keyBg, 3));
        CHECK(isOnly(hit(IX + 8 + 40, ky + 12), "inspector-look"));
        {
            auto m = ui->get_m_inspector();
            auto r = m.radio;
            r.looking = true;
            r.look_enabled = false;
            m.radio = r;
            ui->set_m_inspector(m);
            park();
            f = shot();
            CHECK(f.near(IX + 8 + 4, ky + 12, keyDisBg, 3));
            CHECK(f.countNear(IX + 8 + 110, ky, IX + 182, ky + 24, dim, 40) > 8);
            CHECK(hit(IX + 8 + 40, ky + 12).empty());
            r.looking = false;
            r.look_enabled = true;
            m.radio = r;
            ui->set_m_inspector(m);
            park();
            f = shot();
        }
        // the centre: a mono box; Enter commits; the note under it, amber
        const int cy2 = ky + 24 + 6 + 20;
        CHECK(f.countNear(IX + 8, cy2 - 20, IX + 182, cy2, muted, 40) > 20);                       // the label "Centre (MHz)"
        typeInto(IX + 8 + 60, cy2 + 12, "7.1", true);
        CHECKV(isOnly(quiet(evs), "inspector-centre 7.1"), "%s", join(quiet(evs)).c_str());
        CHECK(f.countNear(IX + 8, cy2 + 24, IX + 182, cy2 + 46, warnFg, 40) > 15);
        // the rate: a DropKey; with the note gone it follows at a pitch of 6; a recording's fixed text instead
        {
            auto m = ui->get_m_inspector();
            auto r = m.radio;
            r.centre_note = "";
            m.radio = r;
            ui->set_m_inspector(m);
            park();
            f = shot();
            const int rateY = cy2 + 24 + 6 + 20;
            CHECK(f.countInk(IX + 12, rateY + 3, IX + 160, rateY + 21, keyBg, 40) > 30);
            evs.clear();
            clk(IX + 8 + 40, rateY + 12);
            f = shot();
            CHECK(quiet(evs).empty());
            const auto [r3x, r3y] = popupRow(IX + 8, rateY, 1);
            clk(r3x, r3y);
            shot();
            CHECK(isOnly(quiet(evs), "inspector-rate 1"));
            park();
            f = shot();
            // the lines: amber figures, a dim sentence
            CHECK_GT(f.countNear(IX + 8, rateY + 28, IX + 182, rateY + 80, warnFg, 70), 20);
            CHECK(f.countNear(IX + 8, rateY + 70, IX + 182, rateY + 130, dim, 40) > 15);
            r.rate_fixed = true;
            r.rate_fixed_text = "2.400 MS/s";
            m.radio = r;
            ui->set_m_inspector(m);
            park();
            f = shot();
            CHECK(f.countInk(IX + 12, rateY + 3, IX + 160, rateY + 21, panelAlt, 30) > 20 && !f.near(IX + 8 + 2, rateY + 12, keyBg, 3));
            CHECK(f.countNear(IX + 8, rateY, IX + 182, rateY + 20, warnFg, 40) > 15);               // the fixed text, amber
            CHECK(f.countNear(IX + 8, rateY + 18, IX + 182, rateY + 40, dim, 40) > 15);             // "The recording sets the rate."
            evs.clear();
            clk(IX + 8 + 40, rateY + 12);
            CHECK(quiet(evs).empty());
            r.rate_fixed = false;
            m.radio = r;
            ui->set_m_inspector(m);
            park();
            f = shot();
        }
    }
    {
        // the folded strip: 24 px, the fold key ("<<") centred, nothing else; the canvas takes the rest
        auto m = ui->get_m_patch();
        m.inspector_folded = true;
        evs.clear();
        ui->set_m_patch(m);
        park();
        f = shot();
        CHECKV(lastOf(evs, "canvas-size").args == fmtD(PW - 8 - 24) + " " + fmtD(ch), "canvas-size: %s", lastOf(evs, "canvas-size").args.c_str());
        CHECKV(static_cast<int>(std::lround(ui->get_inspector_w())) == 24 && static_cast<int>(std::lround(ui->get_canvas_w())) == PW - 8 - 24, "folded: %d, canvas %d", static_cast<int>(std::lround(ui->get_inspector_w())), static_cast<int>(std::lround(ui->get_canvas_w())));
        const int fx = PX + PW - 24;
        CHECK(f.near(fx + 12, IY + 8 + 2, keyBg, 6) && f.near(fx + 23, IY + 8 + 10, panelAlt, 3) && f.near(fx + 12, IY + 8 + 24, panelAlt, 3));   // a 20 px key, the strip round it
        CHECK(f.countInk(fx + 2, IY + 40, fx + 24, IY + ch - 2, panelAlt, 12) <= 40);
        CHECK(isOnly(hit(fx + 12, IY + 8 + 10), "inspector-fold"));
        m.inspector_folded = false;
        evs.clear();
        ui->set_m_patch(m);
        park();
        f = shot();
        CHECK(static_cast<int>(std::lround(ui->get_inspector_w())) == 190);
        CHECKV(lastOf(evs, "canvas-size").args == fmtD(cw) + " " + fmtD(ch), "canvas-size: %s", lastOf(evs, "canvas-size").args.c_str());
    }
    // the other kinds, one after another, on the same model
    const auto setKind = [&](int kind, const std::function<void(InspectorModel&)>& f2) {
        auto m = ui->get_m_inspector();
        m.kind = kind;
        f2(m);
        ui->set_m_inspector(m);
        park();
        return shot();
    };
    {
        // CHANNEL: "Frequency (MHz)" and a mono box
        f = setKind(1, [](InspectorModel& m) { m.kind_caption = "CHANNEL"; m.name = "Channel"; });
        const int by = IY + static_cast<int>(std::lround(ui->get_inspector_block_y()));
        CHECK(f.countNear(IX + 8, by, IX + 182, by + 16, muted, 40) > 20);
        CHECK(f.near(IX + 12, by + 20 + 12, field, 3));
        typeInto(IX + 8 + 60, by + 20 + 12, "99.5", true);
        CHECKV(isOnly(quiet(evs), "inspector-freq 99.5"), "%s", join(quiet(evs)).c_str());
        // the Size keys, for every kind: Smaller / Larger half width, Default size full width, the grip sentence, Remove this node
        const int sy = IY + static_cast<int>(std::lround(ui->get_inspector_size_y()));
        CHECK(f.near(IX + 8 + 4, sy + 12, keyBg, 3) && f.near(IX + 8 + 84 + 6 + 4, sy + 12, keyBg, 3) && f.near(IX + 8 + 84 + 3, sy + 12, panelAlt, 3));
        CHECK(isOnly(hit(IX + 8 + 40, sy + 12), "inspector-size 0"));
        CHECK(isOnly(hit(IX + 8 + 84 + 6 + 40, sy + 12), "inspector-size 1"));
        const int dfy = IY + static_cast<int>(std::lround(ui->get_inspector_default_y()));
        CHECK(f.near(IX + 8 + 4, dfy + 12, keyBg, 3) && f.near(IX + 8 + 170, dfy + 12, keyBg, 3));
        CHECK(isOnly(hit(IX + 8 + 90, dfy + 12), "inspector-size 2"));
        CHECK(f.countNear(IX + 8, dfy + 26, IX + 182, dfy + 60, dim, 40) > 30);                  // "Or drag the brass corner ..."
        const int rmy = IY + static_cast<int>(std::lround(ui->get_inspector_remove_y()));
        CHECK(f.near(IX + 8 + 4, rmy + 12, dangerBg, 3) && f.near(IX + 8 + 170, rmy + 12, dangerBg, 3));
        CHECK(isOnly(hit(IX + 8 + 90, rmy + 12), "inspector-remove"));
    }
    {
        // DEMOD: Mode, the squelch tick and slider, the gate readout
        f = setKind(2, [](InspectorModel& m) { m.kind_caption = "DEMOD"; m.name = "Demod"; });
        const int by = IY + static_cast<int>(std::lround(ui->get_inspector_block_y()));
        const int dy = by + 20;
        CHECK(f.countInk(IX + 12, dy + 3, IX + 160, dy + 21, keyBg, 40) > 10);
        evs.clear();
        clk(IX + 8 + 40, dy + 12);
        f = shot();
        const auto [mx, my] = popupRow(IX + 8, dy, 3);
        clk(mx, my);
        shot();
        CHECK(isOnly(quiet(evs), "inspector-mode 3"));
        const int ty = dy + 24 + 6;
        CHECKV(isOnly(hit(IX + 8 + 6, ty + 8), "inspector-squelch-on 0"), "tick");
        park();
        f = shot();
        const int sy2 = ty + 16 + 6;
        const int sx = IX + 8 + 8, sw = 174 - 8 - 8 - 48 - 4;
        CHECK_GT(f.countNear(sx, sy2, sx + sw, sy2 + 16, tok(wb.slider_fill), 6), 100);
        evs.clear();
        clk(sx + 3 * sw / 4, sy2 + 8);
        park();
        f = shot();
        {
            const Ev e = lastOf(evs, "inspector-squelch");
            CHECKV(e.name == "inspector-squelch" && std::fabs(std::atof(e.args.c_str()) - 0.75) < 0.02, "inspector-squelch: %s", e.args.c_str());
        }
        const int gy = sy2 + 16 + 6;
        CHECK_GT(f.countNear(IX + 8, gy, IX + 62, gy + 16, warnFg, 70), 6);
        CHECK_GT(f.countNear(IX + 70, gy, IX + 182, gy + 16, goodFg, 70), 6);
    }
    {
        // DECODER: Plugin, the notes, the fed lines, the settings form
        f = setKind(3, [](InspectorModel& m) { m.kind_caption = "DECODER"; m.name = "ADS-B"; });
        const int by = IY + static_cast<int>(std::lround(ui->get_inspector_block_y()));
        const int dy = by + 20;
        CHECK(f.countInk(IX + 12, dy + 3, IX + 160, dy + 21, keyBg, 40) > 10);
        CHECK(f.countNear(IX + 8, dy + 28, IX + 182, dy + 110, dim, 40) > 60);                       // the feed note
        CHECK(f.countNear(IX + 8, dy + 28, IX + 182, dy + 160, warnFg, 40) > 40);                    // the fed lines
        evs.clear();
        clk(IX + 8 + 40, dy + 12);
        f = shot();
        const auto [px, py2] = popupRow(IX + 8, dy, 1);
        clk(px, py2);
        shot();
        CHECK(isOnly(quiet(evs), "inspector-plugin 1"));
        // the settings form: the kit's, open; a committed field is inspector-setting(id, text)
        f = setKind(3, [](InspectorModel& m) {
            auto d = m.decoder;
            d.feed_note = "";
            d.fed = std::make_shared<slint::VectorModel<slint::SharedString>>();
            d.shown_note = "";
            m.decoder = d;
        });
        const int formY = by + 20 + 24 + 6;
        CHECK(f.countInk(IX + 8, formY, IX + 182, formY + 40, panelAlt, 12) > 30);               // the form's fold line / the hairline
        int fy = -1;
        for (int y = formY + 20; y < formY + 120 && fy < 0; ++y) { if (f.near(IX + 8 + 30, y, field, 3) && f.near(IX + 8 + 30, y + 6, field, 3)) { fy = y; } }
        CHECKV(fy > 0, "no settings field found");
        if (fy > 0) {
            typeInto(IX + 8 + 30, fy + 6, "13", true, 6);
            CHECKV(isOnly(quiet(evs), "inspector-setting 7 13"), "%s", join(quiet(evs)).c_str());
        }
        // without settings, no form
        f = setKind(3, [](InspectorModel& m) { auto d = m.decoder; d.has_settings = false; m.decoder = d; });
        CHECK(f.countNear(IX + 8, formY, IX + 182, formY + 120, field, 3) < 20);
        f = setKind(3, [](InspectorModel& m) { auto d = m.decoder; d.has_settings = true; m.decoder = d; });
    }
    {
        // MAP: wired count, a row per source (amber count, then the name), the help
        f = setKind(6, [](InspectorModel& m) { m.kind_caption = "MAP"; m.name = "Map"; });
        const int by = IY + static_cast<int>(std::lround(ui->get_inspector_block_y()));
        CHECK(f.countInk(IX + 8, by, IX + 182, by + 16, panelAlt, 40) > 20);
        CHECK(f.countNear(IX + 8, by + 14, IX + 182, by + 60, warnFg, 40) > 15);
        CHECK(f.countNear(IX + 8, by + 14, IX + 182, by + 60, fg, 40) + f.countNear(IX + 8, by + 14, IX + 182, by + 60, fgSoft, 40) > 20);
        CHECK(f.countNear(IX + 8, by + 50, IX + 182, by + 130, dim, 40) > 60);
        // SINK (audio): "Sound goes to", a DropKey with a greyed MP3, lines, the files note
        f = setKind(5, [](InspectorModel& m) { m.kind_caption = "SINK"; m.name = "Speaker"; });
        const int dy = by + 20;
        CHECK(f.countInk(IX + 12, dy + 3, IX + 160, dy + 21, keyBg, 40) > 10);
        evs.clear();
        clk(IX + 8 + 40, dy + 12);
        f = shot();
        const auto [r1x, r1y] = popupRow(IX + 8, dy, 1);
        clk(r1x, r1y);
        shot();
        CHECK(quiet(evs).empty());
        const auto [r0x, r0y] = popupRow(IX + 8, dy, 0);
        clk(r0x, r0y);
        shot();
        CHECK(isOnly(quiet(evs), "inspector-sink 0"));
        park();
        f = shot();
        CHECK_GT(f.countNear(IX + 8, dy + 28, IX + 182, dy + 70, warnFg, 70), 15);
        CHECK(f.countNear(IX + 8, dy + 60, IX + 182, dy + 130, dim, 40) > 30);
        // SINK (text), DISPLAY: the help sentence, dim, wrapped
        f = setKind(5, [](InspectorModel& m) { m.audio_sink = false; m.help = "Shows the lines from every decoder wired to it, and sends them to the Decoder output window."; });
        CHECK(f.countNear(IX + 8, by, IX + 182, by + 60, dim, 40) > 60);
        f = setKind(4, [](InspectorModel& m) { m.kind_caption = "DISPLAY"; m.help = "This node has nothing to set yet."; });
        CHECK(f.countNear(IX + 8, by, IX + 182, by + 40, dim, 40) > 30);
        f = setKind(0, [](InspectorModel& m) { m.kind_caption = "RADIO"; m.name = "Radio"; m.audio_sink = true; });
    }

    // ======================================================== the narrow page: the FlowRows wrap, the Radio key greyed ========================================================
    f = shot();
    {
        const int nbH = static_cast<int>(std::lround(ui->get_n_band_h()));
        CHECKV(static_cast<int>(std::lround(ui->get_n_info_w())) == 260, "narrow info width %d", static_cast<int>(std::lround(ui->get_n_info_w())));
        CHECK(static_cast<int>(std::lround(ui->get_n_canvas_w())) == NW);                                      // no inspector shown
        CHECK(static_cast<int>(std::lround(ui->get_n_canvas_oy())) == nbH + 1);
        // the transport wraps: ALL OFF shrinks to the room, PRESETS drops under
        CHECK(static_cast<int>(std::lround(ui->get_n_transport_y())) == 8);
        CHECK(static_cast<int>(std::lround(ui->get_n_readout_y())) == 8 + 30 + 6 + 30 + 6);
        const int ty = NY + 8;
        CHECK(f.near(NX + 14, ty + 15, accent, 2));                                            // stopped: START is `on`
        CHECK(f.near(NX + 100 + 139, ty + 15, dangerBg, 2) && !f.near(NX + 100 + 143, ty + 15, dangerBg, 2));       // ALL OFF is 142 px: 232 - 84 - 6
        CHECK(f.near(NX + 14, ty + 36 + 15, accent, 2));                                       // PRESETS (open: `on`) on the second line
        CHECK(isOnly(hitF(NX + 50, ty + 15), "run-clicked"));
        CHECK(isOnly(hitF(NX + 150, ty + 15), "all-off-clicked"));
        CHECK(isOnly(hitF(NX + 50, ty + 36 + 15), "presets-clicked"));
        // the part keys: Radio, Channel, Demod / Spectrum, Speaker, Text out / Map
        std::vector<int> w;
        for (const auto& l : partLabels) { w.push_back(7 * charCount(l) + 28); }
        const auto at = flow(w, NX + 10, NX + 10, false, NX + 10 + 232, 4);
        CHECK(ui->get_n_part_rows() == 3 && at.back().row == 2);
        const int pY = NY + static_cast<int>(std::lround(ui->get_n_parts_y()));
        for (std::size_t i = 0; i < at.size(); ++i) {
            const int y = pY + at[i].row * 28;
            CHECKV(f.near(at[i].x + 4, y + 12, i == 0 ? keyDisBg : keyBg, 2), "narrow part key %zu", i);
            const auto r = hitF(at[i].x + at[i].w / 2, y + 12);
            if (i == 0) { CHECKV(r.empty(), "the greyed Radio key fired %s", join(r).c_str()); }
            else { CHECKV(isOnly(r, "part-pressed " + std::to_string(i)), "narrow part key %zu: %s", i, join(r).c_str()); }
        }
        // the greyed key still shows its tooltip (the window's AllowWhenDisabled)
        park();
        const Frame before = shot();
        mv(at[0].x + 20, pY + 12);
        settle(milliseconds(900));
        const Frame after = shot();
        CHECK(after.countInk(at[0].x - 4, pY + 24, at[0].x + 250, pY + 60, strip, 6) > before.countInk(at[0].x - 4, pY + 24, at[0].x + 250, pY + 60, strip, 6) + 100);
        park();
        // the decoders wrap after their caption
        {
            const auto m = ui->get_m_narrow();
            std::vector<int> dw;
            for (std::size_t i = 0; i < m.decoders->row_count(); ++i) { dw.push_back(7 * charCount(std::string(m.decoders->row_data(i)->label)) + 20); }
            const int x0 = NX + static_cast<int>(std::lround(ui->get_n_decoders_x0()));
            const auto dat = flow(dw, NX + 10, x0, true, NX + 10 + 232, 4);
            const int dY = NY + static_cast<int>(std::lround(ui->get_n_decoders_y()));
            // THE ROWS ARE CAPPED (docs 6.52): the narrow band wraps these decoders into three rows or more, and the band shows two of them with a
            // fold key on the third row ("All N decoders"); the keys of the folded rows are not there to press. The fold key unfolds them all (one
            // row more, the key's own, under them) and folds them away again.
            const int allRows = dat.back().row + 1;
            CHECK(allRows >= 3);
            CHECK(ui->get_n_decoder_rows() == 3);
            // the two rows that show fire their keys; a folded key's place is empty (its row is below the fold key's, so the test needs four rows for that)
            for (std::size_t i = 0; i < dat.size(); ++i) {
                if (dat[i].row >= 2 && dat[i].row < 3) { continue; }   // the fold key's own row: a press there unfolds, tested below on purpose
                const int y = dY + dat[i].row * 24;
                const auto r = hitF(dat[i].x + dat[i].w / 2, y + 10);
                if (dat[i].row < 2) { CHECKV(isOnly(r, "decoder-pressed " + std::to_string(i)), "narrow decoder key %zu: %s", i, join(r).c_str()); }
                else { CHECKV(r.empty(), "folded decoder key %zu fired %s", i, join(r).c_str()); }
            }
            CHECK(ui->get_n_decoder_rows() == 3);
            // the fold key sits at the band's own left edge (root.x0, 10 px in), under the two rows; it fires no event of the page: it unfolds
            CHECK(hitF(NX + 10 + 8, dY + 2 * 24 + 10).empty());
            CHECKV(ui->get_n_decoder_rows() == allRows + 1, "rows after the fold key: %d, expected %d", ui->get_n_decoder_rows(), allRows + 1);
            for (std::size_t i = 0; i < dat.size(); ++i) {
                const int y = dY + dat[i].row * 24;
                const auto r = hitF(dat[i].x + dat[i].w / 2, y + 10);
                CHECKV(isOnly(r, "decoder-pressed " + std::to_string(i)), "unfolded decoder key %zu: %s", i, join(r).c_str());
            }
            CHECK(hitF(NX + 10 + 8, dY + allRows * 24 + 10).empty());   // "Fewer decoders", under them all
            CHECKV(ui->get_n_decoder_rows() == 3, "rows after Fewer decoders: %d", ui->get_n_decoder_rows());
        }
        // the information pane's help branch, wrapped in 260 px; the verdict in tone err
        CHECK(f.countNear(NX + NW - 260 + 8, NY + 8, NX + NW - 6, NY + 28, blendOver(wb.chip_err_bg, strip), 4) > 100);
        CHECK(f.countNear(NX + NW - 260 + 8, NY + 30, NX + NW - 6, NY + nbH, dim, 40) > 200);
        // nothing past the page: the band's contents are clipped to the column
        CHECK(f.countInk(NX + NW, NY, NX + NW + 8, NY + nbH, bg, 4) == 0);
    }

    // ======================================================== the presets page (440 x 460) ========================================================
    {
        const int ly = RY + static_cast<int>(std::lround(ui->get_pr_list_y()));
        const int lh = static_cast<int>(std::lround(ui->get_pr_list_h()));
        const int ky = RY + static_cast<int>(std::lround(ui->get_pr_keys_y()));
        const int ny = RY + static_cast<int>(std::lround(ui->get_pr_note_y()));
        CHECKV(ly == RY + 8 && lh > 100, "list at %d, %d high", ly - RY, lh);
        CHECK(ky == ly + lh + 6);
        CHECKV(ny > ky && ui->get_pr_name_y() < 0, "note at %d, name row %f (edit is 0)", ny - RY, static_cast<double>(ui->get_pr_name_y()));
        CHECK(f.near(RX + 8 + 40, ly + 6 * 25 + 40, card, 3) || f.near(RX + 60, ly + lh - 4, card, 3));   // the list's ground: card
        CHECK(f.colNear(RX + 8, ly + 2, ly + lh - 2, lineStrong, 4) && f.rowNear(ly, RX + 12, RX + RW - 12, lineStrong, 4));   // 1 px line-strong border
        // rows of 25 px: the previous patch muted, the selected one on table-selected in accent-soft
        const int r0 = ly + 1;
        CHECK(f.countNear(RX + 12, r0, RX + 200, r0 + 25, muted, 40) > 20);
        CHECK(f.near(RX + RW - 30, r0 + 2 * 25 + 12, tableSel, 3) && !f.near(RX + RW - 30, r0 + 25 + 12, tableSel, 3));
        CHECK(f.countNear(RX + 12, r0 + 50, RX + 200, r0 + 75, accentSoft, 40) > 20);
        CHECK(f.countNear(RX + 12, r0 + 25, RX + 200, r0 + 50, fgSoft, 40) + f.countNear(RX + 12, r0 + 25, RX + 200, r0 + 50, fg, 40) > 20);
        evs.clear();
        clk(RX + 100, r0 + 25 + 12);
        CHECKV(isOnly(quiet(evs), "presets-row 1"), "%s", join(quiet(evs)).c_str());
        settle(milliseconds(650));
        evs.clear();
        clk(RX + 100, r0 + 75 + 12);
        clk(RX + 100, r0 + 75 + 12);
        shot();
        CHECKV(join(quiet(evs)) == "presets-row 3 | presets-row 3 | presets-row-activated 3", "double-click: %s", join(quiet(evs)).c_str());
        settle(milliseconds(650));
        // the four keys: LOAD, SAVE AS..., RENAME, DELETE (xs, 20 px), widths estimated, 4 px apart
        const std::vector<std::string> keys = {"LOAD", "SAVE AS...", "RENAME", "DELETE"};
        std::vector<int> w;
        for (const auto& k : keys) { w.push_back(7 * charCount(k) + 20); }
        const auto at = flow(w, RX + 8, RX + 8, false, RX + 8 + RW - 16, 4);
        const char* want[4] = {"presets-load", "presets-save-as", "presets-rename", "presets-delete"};
        for (int i = 0; i < 4; ++i) {
            CHECK(at[static_cast<std::size_t>(i)].row == 0);
            const int cx = at[static_cast<std::size_t>(i)].x + at[static_cast<std::size_t>(i)].w / 2;
            CHECK(f.near(at[static_cast<std::size_t>(i)].x + 3, ky + 10, i == 3 ? keyDisBg : keyBg, 2));            // DELETE is greyed (delete-enabled false)
            const auto r = hit(cx, ky + 10);
            if (i == 3) { CHECKV(r.empty(), "the greyed DELETE fired %s", join(r).c_str()); }
            else { CHECKV(isOnly(r, want[i]), "key %d: %s", i, join(r).c_str()); }
        }
        {
            auto m = ui->get_m_presets();
            m.delete_enabled = true;
            m.load_enabled = false;
            ui->set_m_presets(m);
            park();
            f = shot();
            CHECK(isOnly(hit(at[3].x + at[3].w / 2, ky + 10), "presets-delete"));
            CHECK(f.near(at[0].x + 3, ky + 10, keyDisBg, 2) && hit(at[0].x + 20, ky + 10).empty());
            m.load_enabled = true;
            ui->set_m_presets(m);
            park();
            f = shot();
        }
        // the note: dim by default, the green of success while note-good (the window draws every note muted; the host says which)
        CHECK(f.countNear(RX + 8, ny, RX + RW - 8, ny + 16, goodFg, 40) > 20);
        {
            auto m = ui->get_m_presets();
            m.note_good = false;
            ui->set_m_presets(m);
            park();
            f = shot();
            CHECK(f.countNear(RX + 8, ny, RX + RW - 8, ny + 16, goodFg, 40) == 0 && f.countNear(RX + 8, ny, RX + RW - 8, ny + 16, dim, 40) > 20);
            m.note_good = true;
            ui->set_m_presets(m);
            park();
        }
        // the name row (edit 1: SAVE AS): "Name", a box of half the width, SAVE and CANCEL; the host's focus request puts the keyboard in the box
        {
            auto m = ui->get_m_presets();
            m.edit = 1;
            m.name_draft = "";
            ui->set_m_presets(m);
            park();
            f = shot();
            const int nY = RY + static_cast<int>(std::lround(ui->get_pr_name_y()));
            const int ny2 = RY + static_cast<int>(std::lround(ui->get_pr_note_y()));
            const int ky2 = RY + static_cast<int>(std::lround(ui->get_pr_keys_y()));
            CHECKV(nY > ky2 && nY < ny2 && ky2 < ky, "name row at %d, note at %d", nY - RY, ny2 - RY);
            CHECK(f.countNear(RX + 8, nY, RX + 44, nY + 24, muted, 40) > 10);                         // the caption "Name"
            CHECK(f.near(RX + 52, nY + 12, field, 3) && f.near(RX + 48 + 211, nY + 12, lineStrong, 4) && f.near(RX + 48 + 213 + 1, nY + 12, bg, 60) == false);
            CHECK(static_cast<int>(std::lround(ui->get_pr_list_h())) < lh);                          // the list gave the row its room
            evs.clear();
            tap(u8"x");                                                                                // the box took the focus when the row opened
            shot();
            CHECKV(lastOf(evs, "presets-name").args == "x", "presets-name: %s", lastOf(evs, "presets-name").args.c_str());
            typeInto(RX + 48 + 60, nY + 12, "Mix", false, 8);
            CHECKV(lastOf(evs, "presets-name").args == "Mix" && countOf(evs, "presets-name") >= 3, "typed name: %s", lastOf(evs, "presets-name").args.c_str());
            evs.clear();
            tap(slint::platform::key_codes::Return);
            shot();
            CHECKV(isOnly(quiet(evs), "presets-go"), "%s", join(quiet(evs)).c_str());
            // SAVE (edit 1) and CANCEL after the box
            const int sx = RX + 48 + 212 + 6;
            CHECK(isOnly(hit(sx + 20, nY + 12), "presets-go"));
            CHECK(isOnly(hit(sx + 48 + 4 + 30, nY + 12), "presets-cancel"));
            m.edit = 2;
            ui->set_m_presets(m);
            park();
            f = shot();
            CHECK(isOnly(hit(sx + 20, nY + 12), "presets-go"));
            CHECK(isOnly(hit(sx + 62 + 4 + 30, nY + 12), "presets-cancel"));
            m.edit = 0;
            ui->set_m_presets(m);
            park();
            f = shot();
            CHECK(ui->get_pr_name_y() < 0 && static_cast<int>(std::lround(ui->get_pr_list_h())) == lh);
        }
        // empty: the host's sentence in place of the rows
        {
            auto m = ui->get_m_presets();
            auto keepRows = m.rows;
            m.rows = std::make_shared<slint::VectorModel<PresetRow>>();
            m.empty_shown = true;
            ui->set_m_presets(m);
            park();
            f = shot();
            CHECK(!f.near(RX + RW - 30, r0 + 2 * 25 + 12, tableSel, 3));
            m.empty_shown = false;
            m.rows = keepRows;
            ui->set_m_presets(m);
            park();
            f = shot();
        }
    }

    // ---- the two in-page questions (over the page, nothing else takes input) and rule 11 at 254 px with long German strings ----
    {
        const int qlY = QY + static_cast<int>(std::lround(ui->get_prn_list_y()));
        const int mw = static_cast<int>(std::lround(ui->get_prn_modal_w()));
        const int mx = QX + static_cast<int>(std::lround(ui->get_prn_modal_x()));
        const int my = QY + static_cast<int>(std::lround(ui->get_prn_modal_y()));
        const int mh = static_cast<int>(std::lround(ui->get_prn_modal_h()));
        const int mky = QY + static_cast<int>(std::lround(ui->get_prn_modal_keys_y()));
        const int yesX = QX + static_cast<int>(std::lround(ui->get_prn_modal_yes_x()));
        const int noX = QX + static_cast<int>(std::lround(ui->get_prn_modal_no_x()));
        CHECKV(mw == 254 - 16 && mx == QX + 8, "modal %d wide at %d", mw, mx - QX);
        CHECK(mh > 80 && my > QY && my + mh < QY + QH);
        CHECK(f.near(mx + 4, my + 4, menu, 3) && f.near(mx + mw / 2, my + mh - 4, menu, 3));                     // the panel
        CHECK(f.countNear(mx + 10, my + 8, mx + mw - 10, my + 24, fg, 40) > 20);                                  // the title "Replace preset?"
        CHECK(f.countNear(mx + 10, my + 26, mx + mw - 10, my + mh - 40, fgSoft, 40) > 100);                       // the host's sentence, wrapped
        CHECK(f.countInk(mx + 10, mky, mx + mw - 10, mky + 24, menu, 40) > 40);                                   // REPLACE / CANCEL
        CHECK(f.near(yesX + 3, mky + 12, accent, 3));                                                              // REPLACE is `on`
        // the backdrop dims the list and takes the input: a click on a row, a key, the name row does nothing
        CHECK(!f.near(QX + QW - 30, qlY + 1 + 25 + 12, tableSel, 3));
        CHECK(hitF(QX + 100, qlY + 1 + 25 + 12).empty());
        CHECK(hitF(QX + 20, QY + static_cast<int>(std::lround(ui->get_prn_keys_y())) + 10).empty());
        CHECK(isOnly(hitF(yesX + 20, mky + 12), "presets-replace 1"));
        CHECK(isOnly(hitF(noX + 20, mky + 12), "presets-replace 0"));
        // the delete question: DELETE / CANCEL
        auto m = ui->get_m_presets_narrow();
        m.ask_replace = false;
        m.ask_delete = true;
        m.delete_text = "Das Preset \"Flugfunk-Ueberwachung mit allen Dekodern\" loeschen? Dies kann nicht rueckgaengig gemacht werden.";
        ui->set_m_presets_narrow(m);
        park();
        f = shot();
        const int mky2 = QY + static_cast<int>(std::lround(ui->get_prn_modal_keys_y()));
        const int yes2 = QX + static_cast<int>(std::lround(ui->get_prn_modal_yes_x()));
        const int no2 = QX + static_cast<int>(std::lround(ui->get_prn_modal_no_x()));
        CHECK(f.countNear(QX + 18, QY + static_cast<int>(std::lround(ui->get_prn_modal_y())) + 8, QX + QW - 18, QY + static_cast<int>(std::lround(ui->get_prn_modal_y())) + 24, fg, 40) > 20);
        CHECK(isOnly(hitF(yes2 + 20, mky2 + 12), "presets-delete-confirm 1"));
        CHECK(isOnly(hitF(no2 + 20, mky2 + 12), "presets-delete-confirm 0"));
        m.ask_delete = false;
        ui->set_m_presets_narrow(m);
        park();
        f = shot();
        // no question: the page takes its clicks again; the name row (edit 1) wraps its keys under the box at this width
        CHECK(isOnly(hitF(QX + 100, qlY + 1 + 25 + 12), "presets-row 1"));
        const int nY = QY + static_cast<int>(std::lround(ui->get_prn_name_y()));
        CHECK(nY > 0 && f.countNear(QX + 8, nY, QX + 44, nY + 24, muted, 40) > 10);
        CHECK(f.near(QX + 52, nY + 12, field, 3));
        CHECK(isOnly(hitF(QX + 8 + 20, nY + 24 + 4 + 10), "presets-go"));
        CHECK(isOnly(hitF(QX + 8 + 48 + 4 + 30, nY + 24 + 4 + 10), "presets-cancel"));
        // the long German note wraps inside the page and nothing is drawn past it
        const int pnY = QY + static_cast<int>(std::lround(ui->get_prn_note_y()));
        CHECK(pnY > nY && f.countNear(QX + 8, pnY, QX + QW - 8, pnY + 30, dim, 40) > 60);
        CHECK(f.countInk(QX + QW, QY, kW, QY + QH, bg, 4) == 0);
        // rule 11: the bare page with the longest words asks for no more than its 254 px cell
        CHECKV(static_cast<int>(std::lround(ui->get_p1_w())) == 254, "probe cell %d", static_cast<int>(std::lround(ui->get_p1_w())));
    }

    // ======================================================== the forwarding: pgn and prn reach PatchEvents, pg and pr do not ========================================================
    {
        // pgn gets a Channel node, its inspector and its canvas keys: the canvas, the face and the inspector gestures reach PatchEvents too
        auto nm = ui->get_m_narrow();
        const auto keepDecoders = nm.decoders;
        nm.inspector_shown = true;
        nm.decoders = std::make_shared<slint::VectorModel<DecoderKey>>();
        nm.blueprints_shown = false;
        nm.blueprint_note = "";
        ui->set_m_narrow(nm);
        auto nc = ui->get_m_canvas_narrow();
        auto nodes = std::make_shared<slint::VectorModel<CanvasNode>>();
        CanvasNode cn = *ui->get_m_canvas().nodes->row_data(1);
        cn.id = 7; cn.x = 20; cn.y = 20; cn.hot_close = false;
        nodes->push_back(cn);
        nc.nodes = nodes;
        ui->set_m_canvas_narrow(nc);
        auto ni = ui->get_m_inspector();
        ni.kind = 1; ni.kind_caption = "CHANNEL"; ni.name = "Channel";
        ui->set_m_inspector_narrow(ni);
        park();
        f = shot();
        const int ox = NX, oy = NY + static_cast<int>(std::lround(ui->get_n_canvas_oy()));
        const int ncw = static_cast<int>(std::lround(ui->get_n_canvas_w())), nch = static_cast<int>(std::lround(ui->get_n_canvas_h()));
        CHECKV(ncw == NW - 8 - 190 && nch > 100, "narrow canvas %d x %d", ncw, nch);
        // the pointer and the wheel
        CHECKV(isOnly(hitF(ox + 200, oy + nch - 60), "canvas-pressed 200 " + std::to_string(nch - 60) + " 0"), "press");
        fwd.clear();
        scroll(win, static_cast<float>(ox + 200), static_cast<float>(oy + nch - 60), 0.f, -120.f);
        CHECKV(countOf(fwd, "canvas-wheel") == 1 && lastOf(fwd, "canvas-wheel").args == "200 " + std::to_string(nch - 60) + " 0 -1 0 0", "wheel: %s", lastOf(fwd, "canvas-wheel").args.c_str());
        fwd.clear();
        tap(slint::platform::key_codes::Delete);
        CHECK(countOf(fwd, "canvas-delete") == 1);
        // the canvas keys
        const int right = ox + ncw - 10, ky = oy + nch - 10 - 14;
        CHECK(isOnly(hitF(right - 3 - 44 - 2 - 14, ky), "zoom-step 1"));
        CHECK(isOnly(hitF(right - 3 - 22, ky), "fit"));
        // the face: the Channel's frequency box (the well's 22 px mark under its top)
        fwd.clear();
        typeInto(ox + 20 + 11 + 40, oy + 20 + 20 + 22 + 12, "88.8", true);
        CHECKV(lastOf(fwd, "face-freq").args == "7 88.8", "face-freq: %s", lastOf(fwd, "face-freq").args.c_str());
        // the inspector: the fold key, and the Size keys when they are in view
        const int ix = NX + NW - 190;
        CHECK(isOnly(hitF(ix + 175, oy + 8 + 10), "inspector-fold"));
        // the inspector scrolls: the wheel over it brings Remove this node (the last row, 8 px above the foot) into view
        scroll(win, static_cast<float>(ix + 100), static_cast<float>(oy + 60), 0.f, -2000.f);
        park();
        f = shot();
        CHECK(f.near(ix + 8 + 4, oy + nch - 8 - 12, dangerBg, 3));
        CHECK(isOnly(hitF(ix + 8 + 90, oy + nch - 8 - 12), "inspector-remove"));
        // put the pointer back on the first page and take the narrow models back
        nm.inspector_shown = false;
        nm.decoders = keepDecoders;
        nm.blueprints_shown = true;
        nm.blueprint_note = "Placed \"ADS-B\". It has no preset frequency - type one in before pressing START.";
        ui->set_m_narrow(nm);
        nc.nodes = std::make_shared<slint::VectorModel<CanvasNode>>();
        ui->set_m_canvas_narrow(nc);
        park();
        f = shot();
    }
    {
        resetEv();
        clk(PX + 50, PY + 8 + 15);
        shot();
        CHECK(isOnly(quiet(evs), "run-clicked") && fwd.empty());
    }

    // ======================================================== the deliberate-break hook (see the report): this check names the property the break targets ========================================================
    f = shot();
    f.savePng(shotDir() + "patch-specimen-final.png");
    return testSummary("test_slint_patch");
}
