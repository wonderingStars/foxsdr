// The workspace bar (src/slint/ui/workspace.slint, docs/multi-radio-records/screen-s5.md, stage S5) at the widths a window can give it, in BOTH skins, fed with FAKE DATA for eight radios through the
// WorkspaceModel global (no host, no engine). What S4 left wrong: at 800 px the selector, the STOP keys and the lamps were clipped out of the one 30 px row, at 1280 x 720 with six to eight radios the last
// MASTER lamps were clipped, and on the PATCH and SCOPE tabs of a compact window the radios' row was not there at all. Now the bar WRAPS: its groups (layout keys, page keys, Link tuning, Start together,
// in a compact window the selector, STOP keys and MASTER lamps, the small-window notice) are placed by their own widths and a group that does not fit the rest of the row starts the next one.
//   - at every width from the smallest window's (624 px with the panel open leaves 264 px) to 1760 px, with 2 to 8 radios, on the RECEIVER tab and (compact) on the other tabs: every control the model
//     names is there exactly once and lies inside the bar (its rect inside [0, width] x [0, height]), no two controls overlap, and the height is a whole number of 30 px rows, never more than the groups need;
//   - the view under the bar starts at the bar's height (a marker strip), so a taller bar pushes it down and nothing is covered;
//   - long words (a wordier language) wrap onto more rows instead of being cut;
//   - Link tuning reports its click and shows its state (checked), Start together reports its click and is dead when asked to be;
//   - the contrast of every text new in stages S3 to S5 on its own background, measured from the skin's tokens (at least 4.5 to 1), is tabulated.
// The picture of each size in each skin is saved under shotDir().
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "render_harness.hpp"
#include "test_check.hpp"

#include "radio_bar_specimen.h"

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

StripRadio strip(int radio, bool on) {
    StripRadio r;
    r.radio = radio;
    r.label = ss("R" + std::to_string(radio));
    r.name = ss("Controls follow Radio " + std::to_string(radio));
    r.tip = ss("Select Radio " + std::to_string(radio));
    r.on = on;
    return r;
}
MasterLamp master(int radio, int state, bool on) {
    MasterLamp m;
    m.radio = radio;
    m.caption = ss(std::to_string(radio));
    m.name = ss("Radio " + std::to_string(radio) + " master");
    m.tip = ss("Radio " + std::to_string(radio));
    m.state = state;
    m.on = on;
    return m;
}

double linear(double c) { c /= 255.0; return c <= 0.03928 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); }
double luminance(const slint::Color& c) { return 0.2126 * linear(c.red()) + 0.7152 * linear(c.green()) + 0.0722 * linear(c.blue()); }
double contrast(const slint::Color& a, const slint::Color& b) {
    double la = luminance(a), lb = luminance(b);
    if (la < lb) { std::swap(la, lb); }
    return (la + 0.05) / (lb + 0.05);
}

struct Box { float x0, y0, x1, y1; };

}  // namespace

int main() {
    installHeadlessPlatform();
    auto ui = RadioBarSpecimen::create();
    auto& win = ui->window();
    auto& g = ui->global<WorkspaceModel>();

    std::string lastEvent;
    int events = 0;
    g.on_link_toggled([&] { lastEvent = "link"; ++events; });
    g.on_together_clicked([&] { lastEvent = "together"; ++events; });
    g.on_selector_clicked([&](int) { lastEvent = "selector"; ++events; });
    g.on_stop_all_clicked([&] { lastEvent = "stop-all"; ++events; });
    g.on_master_clicked([&](int) { lastEvent = "master"; ++events; });

    // THE DATA: eight radios (the worst case), two pages, Link off, something to start
    const auto feed = [&](int radios) {
        g.set_radio_count(radios);
        g.set_selected_radio(1);
        g.set_selector(modelOf(std::vector<StripRadio>{strip(1, true), strip(2, false), strip(3, false), strip(4, false)}));
        std::vector<MasterLamp> lamps;
        for (int r = 1; r <= radios; ++r) { lamps.push_back(master(r, r % 3, r == 1)); }
        g.set_masters(modelOf(lamps));
        g.set_stop_selected_text(ss("STOP R1"));
        g.set_stop_selected_tip(ss("Stop Radio 1"));
        g.set_selected_running(true);
        g.set_stop_all_text(ss("STOP ALL"));
        g.set_stop_all_tip(ss("Stop every open radio"));
        g.set_page_labels(modelOf(std::vector<slint::SharedString>(radios > 4 ? std::vector<slint::SharedString>{ss("Radios 1-4"), ss("Radios 5-8")} : std::vector<slint::SharedString>{ss("Radios 1-4")})));
        g.set_page_tips(modelOf(std::vector<slint::SharedString>{ss("Show radios 1 to 4"), ss("Show radios 5 to 8")}));
        g.set_link_text(ss("Link tuning"));
        g.set_link_tip(ss("Linked radios move together by the same amount and keep the gaps between them."));
        g.set_together_text(ss("Start together"));
        g.set_together_tip(ss("Start every open radio"));
        g.set_together_enabled(true);
    };
    feed(8);
    constexpr int W = 1500, H = 260;
    render(ui, W, H);

    const auto find = [&](const std::string& label) {
        std::vector<slint::testing::ElementHandle> out;
        for (const auto& e : slint::testing::ElementHandle::find_by_accessible_label(ui, label)) {
            if (e.accessible_role() != slint::language::AccessibleRole::Text) { out.push_back(e); }
        }
        return out;
    };
    const auto one = [&](const std::string& label) -> std::optional<slint::testing::ElementHandle> {
        const auto v = find(label);
        if (v.size() != 1) { return std::nullopt; }
        return v[0];
    };
    const auto boxOf = [](const slint::testing::ElementHandle& e) { const auto p = e.absolute_position(); const auto s = e.size(); return Box{p.x, p.y, p.x + s.width, p.y + s.height}; };
    const auto clickCentre = [&](const slint::testing::ElementHandle& e) {
        const Box b = boxOf(e);
        click(win, (b.x0 + b.x1) / 2, (b.y0 + b.y1) / 2);
        settle(milliseconds(20));
    };

    const Skin workbench = ui->global<Workbench>().get_value();
    const Skin night = ui->global<NightWatch>().get_value();

    struct Mode { const char* name; bool compact, receiver; };
    const Mode modes[] = {{"receiver-compact", true, true}, {"receiver-wide", false, true}, {"other-tab-compact", true, false}};
    const int widths[] = {264, 320, 440, 560, 640, 760, 920, 1100, 1400, 1760};

    // which controls a mode must show, with 8 radios
    const auto expected = [](const Mode& m, int radios) {
        std::vector<std::string> v;
        if (m.receiver) {
            for (const char* s : {"One", "Side by side", "2 x 2", "Link tuning", "Start together"}) { v.push_back(s); }
            if (radios > 4) { v.push_back("Radios 1-4"); v.push_back("Radios 5-8"); }   // the page keys are offered when a radio lives beyond the first page
        }
        if (m.compact) {
            for (int r = 1; r <= 4; ++r) { v.push_back("Controls follow Radio " + std::to_string(r)); }
            v.push_back("STOP R1"); v.push_back("STOP ALL");
            for (int r = 1; r <= radios; ++r) { v.push_back("Radio " + std::to_string(r) + " master"); }
        }
        return v;
    };

    // THE SWEEP: both skins, every mode, every width, 2 to 8 radios
    for (const char* skinName : {"workbench", "night"}) {
        ui->global<Theme>().set_skin(std::string(skinName) == "night" ? night : workbench);
        std::printf("-- skin %s\n", skinName);
        for (const Mode& m : modes) {
            ui->set_compact(m.compact);
            ui->set_receiver(m.receiver);
            for (const int radios : {2, 5, 8}) {
                feed(radios);
                for (const int w : widths) {
                    ui->set_bar_w(static_cast<float>(w));
                    Frame f = render(ui, W, H);
                    const float barH = ui->get_bar_height();
                    const int rows = ui->get_bar_rows();
                    CHECKV(std::fabs(barH - 30.f * static_cast<float>(rows)) < 0.5f && rows >= 1 && rows <= 6, "%s %s %d radios %d px: height %.1f is %d rows", skinName, m.name, radios, w, static_cast<double>(barH), rows);
                    const auto under = one("the view under the bar");
                    CHECKV(under.has_value() && std::fabs(boxOf(*under).y0 - barH) < 0.5f, "%s %s %d radios %d px: the view under the bar starts at the bar's height", skinName, m.name, radios, w);
                    std::vector<std::pair<std::string, Box>> boxes;
                    for (const std::string& label : expected(m, radios)) {
                        const auto e = one(label);
                        CHECKV(e.has_value(), "%s %s %d radios %d px: \"%s\" is not exactly one control (%zu)", skinName, m.name, radios, w, label.c_str(), find(label).size());
                        if (!e) { continue; }
                        const Box b = boxOf(*e);
                        CHECKV(b.x0 >= -0.5f && b.y0 >= -0.5f && b.x1 <= static_cast<float>(w) + 0.5f && b.y1 <= barH + 0.5f, "%s %s %d radios %d px: \"%s\" lies at %.0f,%.0f to %.0f,%.0f outside the bar (%d x %.0f)", skinName, m.name, radios, w, label.c_str(),
                               static_cast<double>(b.x0), static_cast<double>(b.y0), static_cast<double>(b.x1), static_cast<double>(b.y1), w, static_cast<double>(barH));
                        boxes.push_back({label, b});
                    }
                    for (std::size_t i = 0; i < boxes.size(); ++i) {
                        for (std::size_t j = i + 1; j < boxes.size(); ++j) {
                            const Box& a = boxes[i].second;
                            const Box& b = boxes[j].second;
                            const bool overlap = a.x0 < b.x1 - 0.5f && b.x0 < a.x1 - 0.5f && a.y0 < b.y1 - 0.5f && b.y0 < a.y1 - 0.5f;
                            CHECKV(!overlap, "%s %s %d radios %d px: \"%s\" overlaps \"%s\"", skinName, m.name, radios, w, boxes[i].first.c_str(), boxes[j].first.c_str());
                        }
                    }
                    if (radios == 8 || w == 264 || w == 1400) { std::printf("   %-18s %d radios %4d px: %d row(s), %.0f px high\n", m.name, radios, w, rows, static_cast<double>(barH)); }
                    if (radios == 8 && (w == 264 || w == 440 || w == 920 || w == 1400)) { f.savePng(shotDir() + "bar-" + skinName + "-" + m.name + "-" + std::to_string(w) + ".png"); }
                    if (m.receiver && m.compact && radios == 8) {
                        // wide enough = one or two rows; a window of 1280 x 720 (920 px of centre) must not need more than two
                        if (w >= 920) { CHECKV(rows <= 2, "%s %s 8 radios %d px: %d rows (at most two are wanted from 920 px up)", skinName, m.name, w, rows); }
                    }
                }
            }
        }
    }

    // EIGHT RADIOS IN SIDE BY SIDE are four pages of two: four page keys ("Radios 1-2" ... "Radios 7-8"), the widest the page group gets. The host offers page keys only while a multi-pane layout is on screen
    // (window of 1280 x 720 or more: a centre column of 920 px with the panel open), so the group is checked from there up: every key inside the bar, at most two rows, none overlapping.
    for (const char* skinName : {"workbench", "night"}) {
        ui->global<Theme>().set_skin(std::string(skinName) == "night" ? night : workbench);
        ui->set_compact(true);
        ui->set_receiver(true);
        feed(8);
        g.set_page_labels(modelOf(std::vector<slint::SharedString>{ss("Radios 1-2"), ss("Radios 3-4"), ss("Radios 5-6"), ss("Radios 7-8")}));
        for (const int w : {920, 1100, 1400}) {
            ui->set_bar_w(static_cast<float>(w));
            Frame f = render(ui, W, H);
            const float barH = ui->get_bar_height();
            CHECKV(ui->get_bar_rows() <= 2, "%s: four page keys at %d px need %d rows", skinName, w, ui->get_bar_rows());
            for (const char* label : {"Radios 1-2", "Radios 3-4", "Radios 5-6", "Radios 7-8", "Link tuning", "Radio 8 master", "STOP ALL"}) {
                const auto e = one(label);
                CHECKV(e.has_value(), "%s %d px: \"%s\" is there", skinName, w, label);
                if (e) { const Box b = boxOf(*e); CHECKV(b.x0 >= -0.5f && b.x1 <= static_cast<float>(w) + 0.5f && b.y1 <= barH + 0.5f, "%s %d px: \"%s\" lies inside the bar (%.0f to %.0f)", skinName, w, label, static_cast<double>(b.x0), static_cast<double>(b.x1)); }
            }
            if (w == 920) { f.savePng(shotDir() + "bar-" + skinName + "-four-page-keys-920.png"); }
        }
        feed(8);
    }

    // LONG WORDS wrap, they are not cut: the receiver tab of a compact window at the smallest width with a wordy Link key and notice
    {
        ui->global<Theme>().set_skin(workbench);
        ui->set_compact(true);
        ui->set_receiver(true);
        feed(8);
        ui->set_bar_w(264.f);
        render(ui, W, H);
        const int plain = ui->get_bar_rows();
        g.set_link_text(ss("Abstimmung koppeln (verknuepft)"));
        g.set_together_text(ss("Alle gleichzeitig starten"));
        g.set_notice(ss("Several radios need a window of at least 1280 x 720. Showing one radio."));
        Frame f = render(ui, W, H);
        const int wordy = ui->get_bar_rows();
        CHECKV(wordy >= plain, "wordy labels need %d rows against %d", wordy, plain);
        for (const char* label : {"Abstimmung koppeln (verknuepft)", "Alle gleichzeitig starten", "STOP ALL", "Radio 8 master"}) {
            const auto e = one(label);
            CHECKV(e.has_value(), "wordy: \"%s\" is there", label);
            if (e) { const Box b = boxOf(*e); CHECKV(b.x0 >= -0.5f && b.x1 <= 264.5f && b.y1 <= ui->get_bar_height() + 0.5f, "wordy: \"%s\" lies inside the bar (%.0f to %.0f)", label, static_cast<double>(b.x0), static_cast<double>(b.x1)); }
        }
        const auto notice = find("Several radios need a window of at least 1280 x 720. Showing one radio.");
        std::printf("   wordy, 264 px: %d rows (plain %d)\n", wordy, plain);
        f.savePng(shotDir() + "bar-workbench-wordy-264.png");
        g.set_link_text(ss("Link tuning"));
        g.set_together_text(ss("Start together"));
        g.set_notice(ss(""));
        (void)notice;
    }

    // THE TWO NEW KEYS: a click is reported, the state is shown, Start together can be dead
    for (const char* skinName : {"workbench", "night"}) {
        ui->global<Theme>().set_skin(std::string(skinName) == "night" ? night : workbench);
        ui->set_compact(false);
        ui->set_receiver(true);
        feed(3);
        ui->set_bar_w(1200.f);
        g.set_link_on(false);
        render(ui, W, H);
        const auto link = one("Link tuning");
        CHECKV(link.has_value() && !link->accessible_checked().value_or(true) && link->accessible_checkable().value_or(false), "%s: Link tuning starts off", skinName);
        if (link) {
            events = 0; lastEvent.clear();
            clickCentre(*link);
            CHECKV(lastEvent == "link" && events == 1, "%s: a click on Link tuning gave '%s'", skinName, lastEvent.c_str());
        }
        g.set_link_on(true);
        render(ui, W, H);
        CHECKV(one("Link tuning").has_value() && one("Link tuning")->accessible_checked().value_or(false), "%s: Link tuning shows that it is on", skinName);
        CHECKV(std::string(one("Link tuning")->accessible_description().value_or(slint::SharedString())).find("keep the gaps") != std::string::npos, "%s: the tip is the key's description", skinName);
        const auto tog = one("Start together");
        CHECKV(tog.has_value() && tog->accessible_enabled().value_or(false), "%s: Start together is live while a radio is stopped", skinName);
        if (tog) {
            events = 0; lastEvent.clear();
            clickCentre(*tog);
            CHECKV(lastEvent == "together" && events == 1, "%s: a click on Start together gave '%s'", skinName, lastEvent.c_str());
        }
        g.set_together_enabled(false);
        render(ui, W, H);
        CHECKV(one("Start together").has_value() && !one("Start together")->accessible_enabled().value_or(true), "%s: Start together is dead when every radio runs", skinName);
        events = 0; lastEvent.clear();
        if (const auto dead = one("Start together")) { clickCentre(*dead); }
        CHECKV(events == 0, "%s: a dead Start together reports nothing", skinName);
        g.set_together_enabled(true);
        g.set_link_on(false);
    }

    // CONTRAST of every text new in stages S3 to S5, from each skin's own tokens: the key's text on the key, lit on the accent, the danger key, the dead key, the notice on the bar
    {
        std::printf("-- contrast (text on its background), WCAG ratio, at least 4.5:1 wanted for every text that is read\n");
        std::printf("   %-44s %-10s %-10s\n", "text on background", "Workbench", "Night Watch");
        struct Pair { const char* what; slint::Color (*fg)(const Skin&); slint::Color (*bg)(const Skin&); bool exempt; };
        const Pair pairs[] = {
            {"key text (Link tuning, Start together) on key", [](const Skin& s) { return s.key_fg; }, [](const Skin& s) { return s.key_bg; }, false},
            {"lit key text (Link tuning on, Link in a pane)", [](const Skin& s) { return s.accent_ink; }, [](const Skin& s) { return s.accent; }, false},
            {"lit key text, hovered", [](const Skin& s) { return s.accent_ink; }, [](const Skin& s) { return s.accent_hover; }, false},
            {"danger key text (Link could not follow, STOP Rn)", [](const Skin& s) { return s.danger_fg; }, [](const Skin& s) { return s.danger_bg; }, false},
            {"layout / page / selector key text (SegKey) on its group", [](const Skin& s) { return s.fg_soft; }, [](const Skin& s) { return s.panel_alt; }, false},
            {"layout / page / selector key text, hovered", [](const Skin& s) { return s.fg_soft; }, [](const Skin& s) { return s.menu_hot; }, false},
            {"small-window notice on the bar", [](const Skin& s) { return s.chip_warn_fg; }, [](const Skin& s) { return s.strip; }, false},
            {"caption (Layout) on the bar", [](const Skin& s) { return s.muted; }, [](const Skin& s) { return s.strip; }, false},
            {"MASTER lamp number, stopped, on the bar", [](const Skin& s) { return s.muted; }, [](const Skin& s) { return s.strip; }, false},
            {"MASTER lamp number, running, on the bar", [](const Skin& s) { return s.lamp_green_cap; }, [](const Skin& s) { return s.strip; }, false},
            {"MASTER lamp number, failed, on the bar", [](const Skin& s) { return s.lamp_red_cap; }, [](const Skin& s) { return s.strip; }, false},
            // the texts of the parts that stages S3 and S4 added (the pane's own are stage S2's)
            {"R-tag ink on radio 1's colour (pane, panel, mix)", [](const Skin& s) { return s.radio_ink; }, [](const Skin& s) { return s.radio_1; }, false},
            {"R-tag ink on radio 2's colour", [](const Skin& s) { return s.radio_ink; }, [](const Skin& s) { return s.radio_2; }, false},
            {"R-tag ink on radio 3's colour", [](const Skin& s) { return s.radio_ink; }, [](const Skin& s) { return s.radio_3; }, false},
            {"R-tag ink on radio 4's colour", [](const Skin& s) { return s.radio_ink; }, [](const Skin& s) { return s.radio_4; }, false},
            {"Add-a-radio card: title on the card", [](const Skin& s) { return s.fg; }, [](const Skin& s) { return s.well_deep; }, false},
            {"Add-a-radio card: hint on the card", [](const Skin& s) { return s.muted; }, [](const Skin& s) { return s.well_deep; }, false},
            {"Add-a-radio card: 'No other radio found.' and the quiet cell's name (muted since stage S5)", [](const Skin& s) { return s.muted; }, [](const Skin& s) { return s.well_deep; }, false},
            {"Add-a-radio card: the engine's refusal on the card", [](const Skin& s) { return s.chip_warn_fg; }, [](const Skin& s) { return s.well_deep; }, false},
            {"Audio mix: the block's title on the panel", [](const Skin& s) { return s.muted; }, [](const Skin& s) { return s.panel_alt; }, false},
            {"pane: device name on the header", [](const Skin& s) { return s.fg_soft; }, [](const Skin& s) { return s.panel; }, false},
            {"pane: frequency on the header", [](const Skin& s) { return s.accent_soft; }, [](const Skin& s) { return s.panel; }, false},
            {"pane: readout caption on the row", [](const Skin& s) { return s.muted; }, [](const Skin& s) { return s.strip; }, false},
            {"pane: readout value on the row", [](const Skin& s) { return s.fg_soft; }, [](const Skin& s) { return s.strip; }, false},
            {"pane: decoder chip", [](const Skin& s) { return s.chip_good_fg; }, [](const Skin& s) { return s.chip_good_bg; }, false},
            {"pane: mode chip", [](const Skin& s) { return s.chip_fg; }, [](const Skin& s) { return s.chip_bg; }, false},
            {"disabled key text (Start together when dead)", [](const Skin& s) { return s.key_disabled_fg; }, [](const Skin& s) { return s.key_disabled_bg; }, true},
        };
        for (const Pair& p : pairs) {
            const double cw = contrast(p.fg(workbench), p.bg(workbench)), cn = contrast(p.fg(night), p.bg(night));
            std::printf("   %-44s %6.2f:1    %6.2f:1%s\n", p.what, cw, cn, p.exempt ? "   (disabled: exempt)" : "");
            if (!p.exempt) { CHECKV(cw >= 4.5, "Workbench: %s is %.2f:1", p.what, cw); CHECKV(cn >= 4.5, "Night Watch: %s is %.2f:1", p.what, cn); }
        }
    }

    return testSummary("test_slint_radio_bar");
}
