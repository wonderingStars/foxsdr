// The multi-radio strip block, the Audio mix, the sections panel's R-tag and the Add-a-radio card (src/slint/ui/radio_strip.slint and workspace.slint, docs/multi-radio-records/screen-s4.md, stage S4),
// on one sheet in BOTH skins, fed with FAKE DATA through the WorkspaceModel global (no host, no engine): the selector keys read "Controls follow Radio n" and the lit one is the selected radio's; STOP Rn
// is live only while that radio runs; STOP ALL reads START ALL when told; the MASTER lamps are one per open radio, green / red / dark, each a key named "Radio n master"; the Audio mix has a route key,
// MUTE, SOLO and a volume slider for every radio, each named with its radio; the card lists the choices, shows the engine's note unchanged, and reports the pane's number with the choice.
// Every gesture is a real pointer event or the control's own default action, found by its accessible label through the element tree, and arrives with the right radio number.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "render_harness.hpp"
#include "test_check.hpp"

#include "radio_strip_specimen.h"

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
            std::printf(__VA_ARGS__);             \
            std::printf("\n");                    \
        }                                         \
    } while (0)

slint::SharedString ss(const std::string& s) { return slint::SharedString(s); }
Rgb rgbOf(const slint::Color& c) { return Rgb{c.red(), c.green(), c.blue()}; }

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
    m.tip = ss("Radio " + std::to_string(radio) + " master: state " + std::to_string(state));
    m.state = state;
    m.on = on;
    return m;
}
MixRow mixRow(int radio, int route, bool muted, bool solo, float volume) {
    static const char* words[4] = {"Both", "Left", "Right", "None"};
    const std::string n = std::to_string(radio);
    MixRow r;
    r.radio = radio;
    r.name = ss("Radio " + n);
    r.route = route;
    r.route_text = ss(words[route]);
    r.route_label = ss("Output of Radio " + n);
    r.route_tip = ss(std::string("Output of Radio ") + n + ": " + words[route]);
    r.muted = muted;
    r.mute_label = ss("Mute Radio " + n);
    r.solo = solo;
    r.solo_label = ss("Solo Radio " + n);
    r.solo_tip = ss("Hear only the radios marked SOLO");
    r.volume = volume;
    r.volume_label = ss("Radio " + n + ": VOLUME");
    r.volume_text = ss(std::to_string(static_cast<int>(volume * 100)) + "%");
    r.running = true;
    return r;
}
AddChoice choice(const char* caption) { AddChoice c; c.caption = ss(caption); c.accessible = ss(std::string("Add ") + caption); return c; }

}  // namespace

int main() {
    installHeadlessPlatform();
    auto ui = RadioStripSpecimen::create();
    auto& win = ui->window();
    auto& g = ui->global<WorkspaceModel>();

    std::string lastEvent;
    int lastRadio = -1, lastChoice = -1, events = 0;
    float lastVolume = -1;
    g.on_selector_clicked([&](int r) { lastEvent = "selector"; lastRadio = r; ++events; });
    g.on_master_clicked([&](int r) { lastEvent = "master"; lastRadio = r; ++events; });
    g.on_stop_selected_clicked([&] { lastEvent = "stop-selected"; ++events; });
    g.on_stop_all_clicked([&] { lastEvent = "stop-all"; ++events; });
    g.on_route_clicked([&](int r) { lastEvent = "route"; lastRadio = r; ++events; });
    g.on_mute_clicked([&](int r) { lastEvent = "mute"; lastRadio = r; ++events; });
    g.on_solo_clicked([&](int r) { lastEvent = "solo"; lastRadio = r; ++events; });
    g.on_volume_changed([&](int r, float v) { lastEvent = "volume"; lastRadio = r; lastVolume = v; ++events; });
    g.on_add_chosen([&](int slot, int c) { lastEvent = "add"; lastRadio = slot; lastChoice = c; ++events; });
    g.on_add_rescan_clicked([&] { lastEvent = "rescan"; ++events; });

    // THE DATA: four radios in the selector (radio 2 selected), three open radios (running, stopped, failed), the card with three devices
    g.set_radio_count(3);
    g.set_selected_radio(2);
    g.set_selector(modelOf(std::vector<StripRadio>{strip(1, false), strip(2, true), strip(3, false), strip(4, false)}));
    g.set_stop_selected_text(ss("STOP R2"));
    g.set_stop_selected_tip(ss("Stop Radio 2"));
    g.set_selected_running(true);
    g.set_stop_all_text(ss("STOP ALL"));
    g.set_stop_all_tip(ss("Stop every open radio"));
    g.set_masters(modelOf(std::vector<MasterLamp>{master(1, 1, false), master(2, 0, true), master(3, 2, false)}));
    g.set_mix(modelOf(std::vector<MixRow>{mixRow(1, 0, false, false, 0.5f), mixRow(2, 1, true, false, 0.25f), mixRow(3, 3, false, true, 0.9f)}));
    g.set_can_add(true);
    g.set_add_choices(modelOf(std::vector<AddChoice>{choice("Signal generator"), choice("RTL-SDR #0 (native)"), choice("Airspy Mini")}));
    g.set_add_title(ss("Add a radio"));
    g.set_add_hint(ss("Choose a radio to open in this pane."));
    g.set_add_none(ss("No other radio found."));
    g.set_add_rescan(ss("Look for radios again"));
    g.set_selected_name(ss("Radio 2"));
    constexpr int W = 1160, H = 520;
    render(ui, W, H);

    const auto find = [&](const std::string& label, bool anyRole = false) {
        std::vector<slint::testing::ElementHandle> out;
        for (const auto& e : slint::testing::ElementHandle::find_by_accessible_label(ui, label)) {
            if (anyRole || e.accessible_role() != slint::language::AccessibleRole::Text) { out.push_back(e); }
        }
        return out;
    };
    const auto one = [&](const std::string& label, bool anyRole = false) -> std::optional<slint::testing::ElementHandle> {
        const auto v = find(label, anyRole);
        if (v.size() != 1) { return std::nullopt; }
        return v[0];
    };
    const auto clickCentre = [&](const slint::testing::ElementHandle& e) {
        const auto p = e.absolute_position();
        const auto s = e.size();
        click(win, p.x + s.width / 2, p.y + s.height / 2);
        settle(milliseconds(20));
    };
    const auto press = [&](const std::string& label, const char* event, int radio) {
        const auto e = one(label);
        CHECKV(e.has_value(), "\"%s\" is not exactly one control", label.c_str());
        if (!e) { return; }
        events = 0; lastEvent.clear(); lastRadio = -1;
        clickCentre(*e);
        CHECKV(lastEvent == event && (radio < 0 || lastRadio == radio), "a click on \"%s\" gave '%s' for radio %d", label.c_str(), lastEvent.c_str(), lastRadio);
    };

    const Skin workbench = ui->global<Workbench>().get_value();
    const Skin night = ui->global<NightWatch>().get_value();
    const auto skinRun = [&](const char* skinName, const Skin& skin) {
        std::printf("-- skin %s\n", skinName);
        ui->global<Theme>().set_skin(skin);
        Frame f = render(ui, W, H);

        // ---- the selector: four keys named with their radio, the selected one lit ----
        for (int r = 1; r <= 4; ++r) {
            const auto k = one("Controls follow Radio " + std::to_string(r));
            CHECKV(k.has_value(), "%s: selector key %d", skinName, r);
            if (k) { CHECKV(k->accessible_checked().value_or(false) == (r == 2), "%s: key %d lit = %d", skinName, r, r == 2); }
        }
        press("Controls follow Radio 3", "selector", 3);
        press("Controls follow Radio 1", "selector", 1);

        // ---- STOP Rn is live only while that radio runs; STOP ALL / START ALL ----
        {
            const auto stop = one("STOP R2");
            CHECKV(stop.has_value() && stop->accessible_enabled().value_or(false), "%s: STOP R2 is live while radio 2 runs", skinName);
            press("STOP R2", "stop-selected", -1);
            g.set_selected_running(false);
            render(ui, W, H);
            const auto dead = one("STOP R2");
            CHECKV(dead.has_value() && !dead->accessible_enabled().value_or(true), "%s: STOP R2 is dead while radio 2 is stopped", skinName);
            g.set_selected_running(true);
            press("STOP ALL", "stop-all", -1);
            g.set_stop_all_text(ss("START ALL"));
            render(ui, W, H);
            CHECKV(one("START ALL").has_value() && !one("STOP ALL").has_value(), "%s: START ALL replaces STOP ALL", skinName);
            press("START ALL", "stop-all", -1);
            g.set_stop_all_text(ss("STOP ALL"));
            render(ui, W, H);
        }

        // ---- the MASTER lamps: one per open radio, each a key; green, dark, red ----
        {
            CHECKV(find("Radio 1 master").size() == 1 && find("Radio 2 master").size() == 1 && find("Radio 3 master").size() == 1 && find("Radio 4 master").empty(), "%s: one lamp per open radio", skinName);
            const slint::Color lampGreen = ui->global<Theme>().get_lamp_green(), lampRed = ui->global<Theme>().get_lamp_red(), lampOff = ui->global<Theme>().get_lamp_off(), lampRing = ui->global<Theme>().get_lamp_ring();
            f = render(ui, W, H);
            // the dot's own colour at its centre (4 px in, half the key's height down), matched to the nearest of the skin's three lamp colours: 0 dark, 1 green, 2 red
            const auto kindOf = [&](int radio) {
                const auto k = one("Radio " + std::to_string(radio) + " master");
                if (!k) { return -1; }
                const Rgb px = f.at(static_cast<int>(k->absolute_position().x) + 9, static_cast<int>(k->absolute_position().y + k->size().height / 2));
                const auto dist = [&](const slint::Color& c) { const int dr = px.r - c.red(), dg = px.g - c.green(), db = px.b - c.blue(); return dr * dr + dg * dg + db * db; };
                const int d0 = dist(lampOff), d1 = dist(lampGreen), d2 = dist(lampRed);
                return d1 < d0 && d1 < d2 ? 1 : d2 < d0 && d2 < d1 ? 2 : 0;
            };
            const auto apart = [&](const slint::Color& a, const slint::Color& b) { const int dr = a.red() - b.red(), dg = a.green() - b.green(), db = a.blue() - b.blue(); return dr * dr + dg * dg + db * db; };
            CHECKV(apart(lampGreen, lampRed) > 1600 && apart(lampGreen, lampOff) > 1600 && apart(lampRed, lampOff) > 1600, "%s: the three lamp colours are told apart", skinName);
            (void)lampRing;
            CHECKV(kindOf(1) == 1, "%s: radio 1's lamp is green (kind %d)", skinName, kindOf(1));
            CHECKV(kindOf(3) == 2, "%s: radio 3's lamp is red (kind %d)", skinName, kindOf(3));
            CHECKV(kindOf(2) == 0, "%s: radio 2's lamp is dark (kind %d)", skinName, kindOf(2));
            press("Radio 3 master", "master", 3);
            press("Radio 1 master", "master", 1);
        }

        // ---- the Audio mix: a route key, MUTE, SOLO and a volume slider named with the radio ----
        {
            for (int r = 1; r <= 3; ++r) {
                const std::string n = std::to_string(r);
                CHECKV(find("Output of Radio " + n).size() == 1 && find("Mute Radio " + n).size() == 1 && find("Solo Radio " + n).size() == 1 && find("Radio " + n + ": VOLUME", true).size() == 1, "%s: row %d is named", skinName, r);
                press("Output of Radio " + n, "route", r);
                press("Mute Radio " + n, "mute", r);
                press("Solo Radio " + n, "solo", r);
            }
            CHECKV(find("Output of Radio 4").empty(), "%s: no row for a radio that is not open", skinName);
            CHECKV(one("Mute Radio 2")->accessible_checked().value_or(false) && !one("Mute Radio 1")->accessible_checked().value_or(true), "%s: MUTE is lit for radio 2 only", skinName);
            CHECKV(one("Solo Radio 3")->accessible_checked().value_or(false) && !one("Solo Radio 2")->accessible_checked().value_or(true), "%s: SOLO is lit for radio 3 only", skinName);
            CHECKV(std::string(one("Output of Radio 2")->accessible_description().value_or(slint::SharedString())).find("Left") != std::string::npos, "%s: radio 2's route says Left", skinName);
            // the slider: a press at its middle reports about half, for ITS radio
            const auto sl = one("Radio 3: VOLUME", true);
            if (sl) {
                events = 0; lastEvent.clear();
                {   // a press at a quarter of the track in the second skin: a slider keeps the position the user put it at until the host moves `value`, and the same press would change nothing
                    const float frac = std::string(skinName) == "night" ? 0.3f : 0.5f;
                    const auto p = sl->absolute_position();
                    const auto sz = sl->size();
                    click(win, p.x + sz.width * frac, p.y + sz.height / 2);
                    settle(milliseconds(20));
                }
                CHECKV(lastEvent == "volume" && lastRadio == 3 && lastVolume > 0.2f && lastVolume < 0.7f, "%s: the volume of radio 3 gave '%s' %d %.2f", skinName, lastEvent.c_str(), lastRadio, static_cast<double>(lastVolume));
            }
        }

        // ---- the R-tag over the panel: the colour of the selected radio ----
        {
            const slint::Color want = skin.radio_2;
            f = render(ui, W, H);
            CHECKV(f.near(12 + 4, 104 + 10 + 3, rgbOf(want), 6), "%s: the panel's tag is radio 2's colour", skinName);
            g.set_selected_radio(1);
            f = render(ui, W, H);
            CHECKV(f.near(12 + 4, 104 + 10 + 3, rgbOf(skin.radio_1), 6), "%s: the tag follows the selected radio", skinName);
            g.set_selected_radio(2);
        }

        // ---- the card: choices, the pane's number, the rescan key, the engine's note unchanged ----
        {
            CHECKV(find("Add Signal generator").size() == 2 && find("Add Airspy Mini").size() == 2, "%s: the cell's card and the sheet's both list the choices", skinName);
            {
                const auto v = find("Add RTL-SDR #0 (native)");
                CHECK(v.size() == 2);
                if (v.size() == 2) {
                    events = 0; lastEvent.clear();
                    clickCentre(v[0]);
                    CHECKV(lastEvent == "add" && lastRadio == 3 && lastChoice == 1, "%s: the cell's card asks for pane 3, choice 1 (got %s %d %d)", skinName, lastEvent.c_str(), lastRadio, lastChoice);
                    events = 0; lastEvent.clear();
                    clickCentre(v[1]);
                    CHECKV(lastEvent == "add" && lastRadio == 0 && lastChoice == 1, "%s: the sheet's card asks for the lowest free slot (got %s %d %d)", skinName, lastEvent.c_str(), lastRadio, lastChoice);
                }
            }
            CHECKV(find("Look for radios again").size() == 2, "%s: the rescan key is on both cards", skinName);
            {
                const auto v = find("Look for radios again");
                if (!v.empty()) { events = 0; lastEvent.clear(); clickCentre(v[0]); CHECKV(lastEvent == "rescan", "%s: rescan", skinName); }
            }
            CHECKV(find("Add a radio to pane 3", true).size() == 1, "%s: the cell's card names its pane", skinName);
            // the engine's words, character for character
            const std::string words = "in use by Radio 1 and by patch radio 2";
            CHECKV(find(words, true).empty(), "%s: no note before a refusal", skinName);
            g.set_add_note(ss(words));
            render(ui, W, H);
            CHECKV(find(words, true).size() == 2, "%s: the note is on both cards, unchanged", skinName);
            g.set_add_note(ss(""));
            // nothing to offer
            g.set_add_choices(modelOf(std::vector<AddChoice>{}));
            Frame empty = render(ui, W, H);
            CHECKV(find("Add Signal generator").empty(), "%s: no choices left", skinName);
            empty.savePng(shotDir() + "strip-" + skinName + "-nochoices.png");
            // SCREEN STAGE S6: a source that needs something typed is a field and an Add key (the I/Q file, rtl_tcp, Pluto), the sound card is an ordinary key the engine will refuse
            {
                AddChoice file = choice("I/Q file");
                file.entry = true;
                file.placeholder = ss("I/Q file path");
                AddChoice card = choice("Sound card");
                g.set_add_choices(modelOf(std::vector<AddChoice>{choice("Signal generator"), file, card}));
                render(ui, W, H);
                CHECKV(find("Add I/Q file").size() == 2, "%s: the file's Add key is on both cards (%zu)", skinName, find("Add I/Q file").size());
                CHECKV(!find("I/Q file").empty(), "%s: the file's field is drawn", skinName);
                CHECKV(find("Add Sound card").size() == 2, "%s: the sound card is offered (the engine refuses it in its own words)", skinName);
                CHECKV(find("Add Signal generator").size() == 2, "%s: the generator is still offered as a plain key", skinName);
            }
            g.set_add_choices(modelOf(std::vector<AddChoice>{choice("Signal generator"), choice("RTL-SDR #0 (native)"), choice("Airspy Mini")}));
        }

        // ---- the block holds its width: the strip's other blocks keep their room (the extent of its own controls, from the first key to the last lamp) ----
        {
            float left = 1e9f, right = 0, top = 1e9f, bottom = 0;
            const char* names[] = {"Controls follow Radio 1", "Controls follow Radio 4", "STOP R2", "STOP ALL", "Radio 1 master", "Radio 3 master"};
            for (const char* n : names) {
                const auto k = one(n);
                if (!k) { continue; }
                left = std::min(left, k->absolute_position().x); right = std::max(right, k->absolute_position().x + k->size().width);
                top = std::min(top, k->absolute_position().y); bottom = std::max(bottom, k->absolute_position().y + k->size().height);
            }
            std::printf("   block's controls: %.0f x %.0f at %.0f,%.0f\n", static_cast<double>(right - left), static_cast<double>(bottom - top), static_cast<double>(left), static_cast<double>(top));
            CHECKV(right - left <= 262.f && right - left > 100.f, "%s: the controls span %.0f px", skinName, static_cast<double>(right - left));
            CHECKV(bottom <= 104.f && top >= 0.f, "%s: the controls are inside the 104 px strip (%.0f to %.0f)", skinName, static_cast<double>(top), static_cast<double>(bottom));
        }
        f = render(ui, W, H);
        f.savePng(shotDir() + "strip-" + skinName + ".png");
    };
    skinRun("workbench", workbench);
    skinRun("night", night);

    // ---- the FLAT form (a compact window's workspace bar, radios' groups alone, as on the PATCH and SCOPE tabs): the same keys and lamps in one 30 px row, in both skins ----
    {
        ui->set_flat(true);
        for (const char* skinName : {"workbench", "night"}) {
            ui->global<Theme>().set_skin(std::string(skinName) == "night" ? night : workbench);
            Frame f = render(ui, W, H);
            const auto sel1 = one("Controls follow Radio 1"), sel4 = one("Controls follow Radio 4"), stopAll = one("STOP ALL"), lamp3 = one("Radio 3 master");
            CHECKV(sel1 && sel4 && stopAll && lamp3 && one("STOP R2"), "%s (flat): the keys are there", skinName);
            if (sel1 && lamp3) {
                const float w = lamp3->absolute_position().x + lamp3->size().width - sel1->absolute_position().x;
                std::printf("   flat row: %.0f px wide, %.0f high\n", static_cast<double>(w), static_cast<double>(lamp3->size().height));
                CHECKV(w <= 480.f && w > 150.f, "%s (flat): the row is %.0f px wide", skinName, static_cast<double>(w));
                CHECKV(lamp3->absolute_position().y >= 0.f && lamp3->absolute_position().y + lamp3->size().height <= 30.f, "%s (flat): the lamps are inside the 30 px bar", skinName);
            }
            press("Controls follow Radio 3", "selector", 3);
            press("STOP ALL", "stop-all", -1);
            press("Radio 3 master", "master", 3);
            f.savePng(shotDir() + "strip-" + skinName + "-flat.png");
        }
        ui->set_flat(false);
        ui->global<Theme>().set_skin(workbench);
    }

    // ---- eight radios: the mix scrolls inside its height, the lamps all fit ----
    {
        ui->global<Theme>().set_skin(workbench);
        std::vector<MasterLamp> lamps;
        std::vector<MixRow> rows;
        for (int r = 1; r <= 8; ++r) { lamps.push_back(master(r, r % 3, r == 2)); rows.push_back(mixRow(r, r % 4, r % 2 == 0, false, 0.5f)); }
        g.set_masters(modelOf(lamps));
        g.set_mix(modelOf(rows));
        g.set_radio_count(8);
        Frame f = render(ui, W, H);
        f.savePng(shotDir() + "strip-workbench-eight.png");
        CHECK(find("Radio 8 master").size() == 1);
        const auto last = one("Radio 8 master");
        if (last) { CHECKV(last->absolute_position().x + last->size().width <= 16 + 280, "radio 8's lamp ends at %.0f", static_cast<double>(last->absolute_position().x + last->size().width)); }
        // the mix block is no taller than 200 px: its foot is the sheet's panel foot (504)
        const auto mixBlock = one("Audio mix", true);
        if (!mixBlock) { for (const auto& e : find("Audio mix", true)) { if (e.accessible_role() == slint::language::AccessibleRole::Groupbox) { CHECKV(e.size().height <= 200.5f, "the mix is %.0f px tall", static_cast<double>(e.size().height)); } } }
    }

    // ---- the one-radio rule at this level: a model with nothing in it draws nothing of the block's rows ----
    {
        g.set_masters(modelOf(std::vector<MasterLamp>{}));
        g.set_mix(modelOf(std::vector<MixRow>{}));
        g.set_selector(modelOf(std::vector<StripRadio>{}));
        render(ui, W, H);
        CHECK(find("Radio 1 master").empty() && find("Output of Radio 1").empty() && find("Controls follow Radio 1").empty());
    }

    return testSummary("test_slint_radio_strip");
}
