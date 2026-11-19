// The map page's layout (MAP FOCUS, docs/multi-radio-records/map-focus.md): the body of every plugin page of kind 5 (ADS-B map, AIS, APRS, radiosondes, satellites ...) in the new window.
// What was wrong: in a 1600 x 1600 window the map sat in the bottom quarter under ~700 px of empty space (the toolbar took a share of the page's spare height), and in 1600 x 900 it was cut off
// by the window's bottom. What is held here, on the real PageBody laid out at the page sizes of 1600 x 1600, 1600 x 900 and 1024 x 640 windows:
//   A. the map pane is the page's focal point: its height is at least the page's height less 200 px at every size (and at least 60 % of the page at 1600 x 900, 55 % at 1024 x 640), its
//      bottom is the page's bottom, and no gap larger than 16 px lies between the toolbar row and the Layers row or between the Layers row and the map;
//   B. the fold: "Map options" is there and CLOSED by default (no Coverage tick, no GPS key); opened, the option rows and the GPS key appear, the map still ends at the page's bottom and
//      is shorter by about the rows' height; closed again, the map is back;
//   D. the same page TORN OFF (a TornWindow, the "+ Window" tab's result) at 700 x 500 and 1000 x 700: the map fills the window's content, the window's right and bottom edges resize it
//      (the offset the host adds), the map follows at once and its pane size reaches the kit (KitEvents.page-map-resized);
//   C. the divider between the target list and the map: at 260 px by default, follows the pointer from the press (many small moves land where one big one would; moving out and back
//      returns to the start), moves the map's left edge by the same amount, stops at 0 and at half the page, and a double-click puts it back to 260 px.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "render_harness.hpp"
#include "test_check.hpp"

#include "map_page_specimen.h"

#ifndef SLINT_FEATURE_EXPERIMENTAL
#define SLINT_FEATURE_EXPERIMENTAL 1
#endif
#include <slint-testing.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <optional>
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
            std::printf(__VA_ARGS__);              \
            std::printf("\n");                    \
        }                                         \
    } while (0)

slint::SharedString ss(const std::string& s) { return slint::SharedString(s); }
template <class T>
std::shared_ptr<slint::VectorModel<T>> modelOf(std::vector<T> v) { return std::make_shared<slint::VectorModel<T>>(std::move(v)); }

struct Box {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    float w() const { return x1 - x0; }
    float h() const { return y1 - y0; }
};

PageTab makePage() {
    PageTab t{};
    t.id = ss("adsb-map");
    t.kind = 5;
    t.title = ss("ADS-B map");
    t.has_presets = true;
    PresetKeyModel p1{};
    p1.label = ss("ADS-B 1090 MHz");
    p1.lit = true;
    t.presets = modelOf(std::vector<PresetKeyModel>{p1});
    t.has_settings = true;
    t.settings_open = false;
    SettingField f{};
    f.id = 1;
    f.label = ss("Receiver position");
    f.value = ss("51.5 -0.1");
    t.fields = modelOf(std::vector<SettingField>{f});
    t.map.targets = modelOf(std::vector<MapTarget>{});
    t.map.legend = modelOf(std::vector<LegendRow>{});
    t.map.list_columns = modelOf(std::vector<slint::SharedString>{ss("CALLSIGN"), ss("ID"), ss("NOTE")});
    TableRow r{};
    r.cells = modelOf(std::vector<slint::SharedString>{ss("FOXSDR1"), ss("4D2025"), ss("")});
    t.map.list_rows = modelOf(std::vector<TableRow>{r});
    t.map.list_widths = modelOf(std::vector<float>{100.f, 90.f, 60.f});
    t.map.count_text = ss("1 target");
    t.map.rx_text = ss("no receiver position: Set RX here, then click the map");
    t.map.sort_label = ss("Callsign");
    t.map.sort_ascending = true;
    t.map.coverage_status = ss("set the RX position to measure coverage");
    t.map.coverage_empty = true;
    t.map.trails_on = true;
    t.map.j.units = ss("Units: Automatic");
    t.map.j.cache_text = ss("No map tiles are kept on disk.");
    t.map.j.layers = modelOf(std::vector<LayerRow>{});
    t.map.j.grid_labels = modelOf(std::vector<GridLabel>{});
    return t;
}

}  // namespace

int main() {
    installHeadlessPlatform();
    auto ui = MapPageSpecimen::create();
    auto& win = ui->window();
    ui->set_page(makePage());
    GpsRow gps{};
    gps.port_index = -1;
    gps.baud_index = -1;
    gps.key_label = ss("Read position from GPS");
    gps.key_enabled = true;   // enabled, so no "name the port first" note sits under the key
    ui->global<MapExtras>().set_gps_shown(true);
    ui->global<MapExtras>().set_gps_hint(ss("Name the port the GPS receiver is on first."));
    ui->global<MapExtras>().set_gps(gps);

    const auto show = [&](float pw, float ph) {
        ui->set_view_w(pw);
        ui->set_view_h(ph);
        render(ui, 1600, 1600);
        settle(milliseconds(30));
        render(ui, 1600, 1600);
    };
    using Role = slint::language::AccessibleRole;
    const auto find = [&](const std::string& label, std::optional<Role> role = std::nullopt) {
        std::vector<slint::testing::ElementHandle> out;
        for (const auto& e : slint::testing::ElementHandle::find_by_accessible_label(ui, label)) {
            const auto er = e.accessible_role();
            if (role ? (er == *role) : (er != Role::Text)) { out.push_back(e); }
        }
        return out;
    };
    const auto box = [&](const std::string& label, std::optional<Role> role = std::nullopt) -> std::optional<Box> {
        const auto v = find(label, role);
        if (v.size() != 1) { return std::nullopt; }
        const auto p = v[0].absolute_position();
        const auto s = v[0].size();
        return Box{p.x, p.y, p.x + s.width, p.y + s.height};
    };
    const auto mapBox = [&]() { return box("Map", Role::Image); };

    struct Size { const char* name; float pw, ph, minFrac; };
    const Size sizes[] = {{"1600x1600", 1240.f, 1373.f, 0.0f}, {"1600x900", 1240.f, 673.f, 0.60f}, {"1024x640", 664.f, 413.f, 0.55f}};

    // ---------------------------------------------------------------- A and B: the page at each size, options folded, then opened
    float closedMapH[3] = {0, 0, 0};
    int si = 0;
    for (const Size& s : sizes) {
        show(s.pw, s.ph);
        const auto m = mapBox();
        const auto fit = box("Fit");
        const auto layers = box("Layers");
        const auto opts = box("Map options");
        CHECKV(m && fit && layers, "%s: found map %d fit %d layers %d", s.name, m ? 1 : 0, fit ? 1 : 0, layers ? 1 : 0);
        CHECKV(opts.has_value(), "%s: the Map options key is not there", s.name);
        if (!(m && fit && layers)) { ++si; continue; }
        std::printf("  %s: page %.0f x %.0f, map at (%.0f, %.0f) %.0f x %.0f, Fit y %.0f..%.0f, Layers y %.0f..%.0f\n", s.name, s.pw, s.ph, m->x0, m->y0, m->w(), m->h(), fit->y0, fit->y1, layers->y0, layers->y1);
        closedMapH[si] = m->h();
        CHECKV(m->h() >= s.ph - 200.f, "%s: the map is %.0f high, wanted at least %.0f", s.name, m->h(), s.ph - 200.f);
        CHECKV(m->h() >= s.minFrac * s.ph, "%s: the map is %.0f of %.0f high, wanted %.0f %%", s.name, m->h(), s.ph, s.minFrac * 100.f);
        CHECKV(std::fabs(m->y1 - s.ph) <= 1.f, "%s: the map ends at y %.0f, the page at %.0f", s.name, m->y1, s.ph);
        CHECKV(layers->y0 - fit->y1 <= 16.f, "%s: %.0f px between the toolbar row and the Layers row", s.name, layers->y0 - fit->y1);
        CHECKV(m->y0 - layers->y1 <= 16.f, "%s: %.0f px between the Layers row and the map", s.name, m->y0 - layers->y1);
        CHECKV(m->x1 >= s.pw - 1.f && m->w() >= s.pw - 330.f, "%s: the map is %.0f wide on a %.0f page", s.name, m->w(), s.pw);
        // B: closed by default
        CHECKV(find("Coverage").empty(), "%s: the Coverage tick shows with the options folded", s.name);
        CHECKV(find("Read position from GPS").empty(), "%s: the GPS key shows with the options folded", s.name);
        if (!opts) { ++si; continue; }
        // open the fold
        click(win, (opts->x0 + opts->x1) / 2, (opts->y0 + opts->y1) / 2);
        settle(milliseconds(30));
        render(ui, 1600, 1600);
        const auto mo = mapBox();
        const auto gpsKey = box("Read position from GPS");
        const auto lay2 = box("Layers");
        CHECKV(!find("Coverage").empty() && !find("Reset coverage").empty(), "%s: the option rows did not appear", s.name);
        CHECKV(gpsKey.has_value(), "%s: the GPS key did not appear", s.name);
        if (mo && gpsKey && lay2) {
            CHECKV(std::fabs(mo->y1 - s.ph) <= 1.f, "%s: open, the map ends at y %.0f of %.0f", s.name, mo->y1, s.ph);
            CHECKV(mo->h() < m->h() - 30.f && mo->h() > 100.f, "%s: open, the map is %.0f high (closed %.0f)", s.name, mo->h(), m->h());
            CHECKV(lay2->y0 - gpsKey->y1 <= 16.f, "%s: open, %.0f px between the last option row and the Layers row", s.name, lay2->y0 - gpsKey->y1);
        } else {
            CHECKV(false, "%s: open, an element is missing", s.name);
        }
        // close it again
        const auto opts2 = box("Map options");
        if (opts2) { click(win, (opts2->x0 + opts2->x1) / 2, (opts2->y0 + opts2->y1) / 2); }
        settle(milliseconds(30));
        render(ui, 1600, 1600);
        const auto mc = mapBox();
        CHECKV(mc && std::fabs(mc->h() - closedMapH[si]) <= 1.f && find("Coverage").empty(), "%s: closed again, the map is %.0f high (was %.0f)", s.name, mc ? mc->h() : -1.f, closedMapH[si]);
        ++si;
    }

    // ---------------------------------------------------------------- C: the divider
    show(1240.f, 673.f);
    const auto div = [&]() { return box("Resize target list", Role::Slider); };
    const auto d0 = div();
    CHECKV(d0.has_value(), "the divider between the list and the map is not there");
    if (d0) {
        const auto m0 = mapBox();
        CHECKV(std::fabs(d0->x0 - 260.f) <= 1.f, "the list's default width: divider at x %.0f", d0->x0);
        CHECKV(m0 && std::fabs(m0->x0 - d0->x1) <= 1.f, "the map starts at the divider's right edge (%.0f vs %.0f)", m0 ? m0->x0 : -1.f, d0->x1);
        const float cx = (d0->x0 + d0->x1) / 2, cy = (d0->y0 + d0->y1) / 2;
        // one drag of 100 px to the right, in ten small moves
        press(win, cx, cy);
        for (int i = 1; i <= 10; ++i) { moveTo(win, cx + 10.f * static_cast<float>(i), cy); render(ui, 1600, 1600); }
        release(win, cx + 100.f, cy);
        render(ui, 1600, 1600);
        auto d1 = div();
        auto m1 = mapBox();
        CHECKV(d1 && std::fabs(d1->x0 - (d0->x0 + 100.f)) <= 1.f, "after +100 the divider is at %.1f (wanted %.1f): it must follow the pointer, not compound", d1 ? d1->x0 : -1.f, d0->x0 + 100.f);
        CHECKV(m0 && m1 && std::fabs((m1->x0 - m0->x0) - 100.f) <= 1.f && std::fabs((m0->w() - m1->w()) - 100.f) <= 1.f, "the map's left edge and width change by the divider's 100 px");
        // out and back to the press point: the list is where it began
        const float c1 = d1 ? (d1->x0 + d1->x1) / 2 : cx;
        press(win, c1, cy);
        for (int i = 1; i <= 8; ++i) { moveTo(win, c1 + 25.f * static_cast<float>(i), cy); render(ui, 1600, 1600); }
        for (int i = 7; i >= 0; --i) { moveTo(win, c1 + 25.f * static_cast<float>(i), cy); render(ui, 1600, 1600); }
        release(win, c1, cy);
        render(ui, 1600, 1600);
        d1 = div();
        CHECKV(d1 && std::fabs(d1->x0 - (d0->x0 + 100.f)) <= 1.f, "out 200 and back: the divider is at %.1f (wanted %.1f)", d1 ? d1->x0 : -1.f, d0->x0 + 100.f);
        // the limits: far right stops at half the page, far left at 0
        const float c2 = d1 ? (d1->x0 + d1->x1) / 2 : c1;
        press(win, c2, cy);
        moveTo(win, 1500.f, cy);
        render(ui, 1600, 1600);
        release(win, 1500.f, cy);
        render(ui, 1600, 1600);
        auto d2 = div();
        CHECKV(d2 && std::fabs(d2->x0 - 620.f) <= 1.f, "dragged far right the list stops at half the page (620): divider at %.1f", d2 ? d2->x0 : -1.f);
        const float c3 = d2 ? (d2->x0 + d2->x1) / 2 : c2;
        press(win, c3, cy);
        moveTo(win, -300.f, cy);
        render(ui, 1600, 1600);
        release(win, -300.f, cy);
        render(ui, 1600, 1600);
        auto d3 = div();
        auto m3 = mapBox();
        CHECKV(d3 && std::fabs(d3->x0) <= 1.f, "dragged far left the list is hidden (0): divider at %.1f", d3 ? d3->x0 : -1.f);
        CHECKV(m3 && m3->x0 <= 8.f && m3->w() >= 1240.f - 8.f, "with the list hidden the map takes the page (%.0f wide from %.0f)", m3 ? m3->w() : -1.f, m3 ? m3->x0 : -1.f);
        // a double-click puts it back to 260
        const float c4 = d3 ? (d3->x0 + d3->x1) / 2 : 3.f;
        click(win, c4, cy);
        click(win, c4, cy);
        settle(milliseconds(30));
        render(ui, 1600, 1600);
        auto d4 = div();
        CHECKV(d4 && std::fabs(d4->x0 - 260.f) <= 1.f, "a double-click resets the list to 260: divider at %.1f", d4 ? d4->x0 : -1.f);
    }

    // ---------------------------------------------------------------- D: torn off
    float evW = -1, evH = -1;
    int evN = 0;
    ui->global<KitEvents>().on_page_map_resized([&](int, float w, float h) { evW = w; evH = h; ++evN; });
    ui->set_torn_mode(true);
    const float tx = 40.f, ty = 40.f;
    for (const float tw : {700.f, 1000.f}) {
        const float th = tw == 700.f ? 500.f : 700.f;
        ui->set_torn_w(tw);
        ui->set_torn_h(th);
        render(ui, 1600, 1600);
        settle(milliseconds(30));
        render(ui, 1600, 1600);
        const auto m = mapBox();
        CHECKV(m.has_value(), "torn %.0f: the map is not there", tw);
        if (!m) { continue; }
        std::printf("  torn %.0f x %.0f: map at (%.0f, %.0f) %.0f x %.0f\n", tw, th, m->x0, m->y0, m->w(), m->h());
        CHECKV(std::fabs(m->x1 - (tx + tw - 1.f)) <= 2.f && std::fabs(m->y1 - (ty + th - 1.f)) <= 2.f, "torn %.0f: the map ends at (%.0f, %.0f), the window's content at (%.0f, %.0f)", tw, m->x1, m->y1, tx + tw - 1.f, ty + th - 1.f);
        CHECKV(m->h() >= 0.6f * th, "torn %.0f: the map is %.0f of %.0f high", tw, m->h(), th);
        // the right edge, 3 px inside it, pulled 100 px; the bottom edge pulled 80 px
        const float ey = ty + th / 2.f;
        press(win, tx + tw - 3.f, ey);
        moveTo(win, tx + tw - 3.f + 100.f, ey);
        render(ui, 1600, 1600);
        release(win, tx + tw - 3.f + 100.f, ey);
        render(ui, 1600, 1600);
        CHECKV(std::fabs(ui->get_torn_w() - (tw + 100.f)) <= 1.f && std::fabs(ui->get_torn_h() - th) <= 1.f, "torn %.0f: the right edge gave %.0f x %.0f, wanted %.0f x %.0f", tw, ui->get_torn_w(), ui->get_torn_h(), tw + 100.f, th);
        const float nw = ui->get_torn_w();
        press(win, tx + nw / 2.f, ty + th - 3.f);
        moveTo(win, tx + nw / 2.f, ty + th - 3.f + 80.f);
        render(ui, 1600, 1600);
        release(win, tx + nw / 2.f, ty + th - 3.f + 80.f);
        render(ui, 1600, 1600);
        settle(milliseconds(30));
        render(ui, 1600, 1600);
        CHECKV(std::fabs(ui->get_torn_h() - (th + 80.f)) <= 1.f && std::fabs(ui->get_torn_w() - nw) <= 1.f, "torn %.0f: the bottom edge gave %.0f x %.0f, wanted %.0f x %.0f", tw, ui->get_torn_w(), ui->get_torn_h(), nw, th + 80.f);
        const auto m2 = mapBox();
        CHECKV(m2 && std::fabs(m2->x1 - (tx + ui->get_torn_w() - 1.f)) <= 2.f && std::fabs(m2->y1 - (ty + ui->get_torn_h() - 1.f)) <= 2.f, "torn %.0f: after the edges the map ends at (%.0f, %.0f)", tw, m2 ? m2->x1 : -1.f, m2 ? m2->y1 : -1.f);
        CHECKV(m2 && evN > 0 && std::fabs(evW - m2->w()) <= 1.f && std::fabs(evH - m2->h()) <= 1.f, "torn %.0f: the kit was told the pane is %.0f x %.0f (it is %.0f x %.0f)", tw, evW, evH, m2 ? m2->w() : -1.f, m2 ? m2->h() : -1.f);
    }
    return testSummary("test_slint_map_page");
}
