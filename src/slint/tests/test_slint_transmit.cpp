// The TRANSMIT page (ui/transmit_page.slint): TransmitPage rendered headlessly from ui/specimens/transmit_specimen.slint in two states (no radio; a radio open with SPLIT, the
// tone, a latch, consent and an armed window) and driven with real pointer and key events. The controls are found by their accessible labels through the element tree (the way the
// window's scripted input finds them), every gesture is read back through the TransmitEvents global, and the colours of the keys that say "on air" and "latched" are probed.
//
// WHAT THIS HOLDS, and why it is a safety test as well as a drawing test:
//   - every control that can put RF out of a connector (PTT, LATCH, Allow remote transmit) is DISABLED with no radio open and reports NOTHING when pressed;
//   - the PTT key is a HOLD: it reports `true` while the pointer is down on it and `false` on release - also when the pointer was dragged off it before the release - and it has NO
//     accessible default action (a screen reader or a script cannot click it into a transmission);
//   - a key event reaches the host only while the PTT key has the keyboard focus, and the focus leaving it (Tab, a click elsewhere) reports the chord UP;
//   - the arming keys work with no radio (pressing one is how the page says why it cannot); OFF too;
//   - the keyed lamp is red, a latched LATCH key is checked and red, SPLIT likewise.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "render_harness.hpp"
#include "test_check.hpp"

#include "transmit_specimen.h"

#ifndef SLINT_FEATURE_EXPERIMENTAL
#define SLINT_FEATURE_EXPERIMENTAL 1
#endif
#include <slint-testing.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
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

Rgb tok(slint::Color c) { return {c.red(), c.green(), c.blue()}; }

slint::SharedString ss(const std::string& s) { return slint::SharedString(s); }

template <class T> std::shared_ptr<slint::Model<T>> model(std::vector<T> v) { return std::make_shared<slint::VectorModel<T>>(std::move(v)); }

// A model as the host maps it, with the arrays the page needs.
TransmitPageModel blank() {
    TransmitPageModel m{};
    std::vector<TxChoice> modes;
    for (const char* n : {"CW", "AM", "NFM", "USB", "LSB"}) { modes.push_back(TxChoice{ss(n), ss(std::string("the ") + n + " sentence")}); }
    m.modes = model(std::move(modes));
    m.inputs = model(std::vector<TxChoice>{TxChoice{ss("MIC"), ss("")}, TxChoice{ss("TONE"), ss("")}});
    m.arm_labels = model(std::vector<slint::SharedString>{ss("5 MIN"), ss("15 MIN"), ss("60 MIN")});
    m.lamp_tone = ChipTone::Neutral;
    m.data_flat = true;
    m.licence = ss("This transmits. Licence sentence.");
    return m;
}

struct Ev {
    std::string name, args;
    std::string str() const { return args.empty() ? name : name + " " + args; }
};

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
    float cx() const { return x + w / 2; }
    float cy() const { return y + h / 2; }
    bool ok() const { return w > 0; }
};

}  // namespace

int main() {
    installHeadlessPlatform();
    auto ui = TransmitSpecimen::create();
    slint::Window& win = ui->window();

    const Skin wb = ui->global<Workbench>().get_value();
    const Rgb lampRed = tok(wb.lamp_red), keyBg = tok(wb.key_bg), keyDisBg = tok(wb.key_disabled_bg), dangerBg = tok(wb.danger_bg), accent = tok(wb.accent);

    std::vector<Ev> evs;
    auto& te = ui->global<TransmitEvents>();
    const auto rec = [&](const char* name) { return [&evs, name]() { evs.push_back({name, ""}); }; };
    te.on_open_clicked(rec("open-clicked"));
    te.on_close_clicked(rec("close-clicked"));
    te.on_split_clicked(rec("split-clicked"));
    te.on_latch_clicked(rec("latch-clicked"));
    te.on_address_edited([&](slint::SharedString t) { evs.push_back({"address-edited", std::string(t)}); });
    te.on_address_focus([&](bool f) { evs.push_back({"address-focus", f ? "1" : "0"}); });
    te.on_frequency_committed([&](slint::SharedString t) { evs.push_back({"frequency-committed", std::string(t)}); });
    te.on_frequency_stepped([&](int d) { evs.push_back({"frequency-stepped", std::to_string(d)}); });
    te.on_mode_clicked([&](int i) { evs.push_back({"mode-clicked", std::to_string(i)}); });
    te.on_power_changed([&](float v) { evs.push_back({"power-changed", v > 0.5f ? "high" : "low"}); });
    te.on_input_clicked([&](int i) { evs.push_back({"input-clicked", std::to_string(i)}); });
    te.on_tone_committed([&](slint::SharedString t) { evs.push_back({"tone-committed", std::string(t)}); });
    te.on_ptt_pointer([&](bool d) { evs.push_back({"ptt-pointer", d ? "1" : "0"}); });
    te.on_ptt_chord([&](slint::SharedString t, bool c, bool s, bool a, bool d) {
        evs.push_back({"ptt-chord", std::string(t) + "|" + (c ? "c" : "") + (s ? "s" : "") + (a ? "a" : "") + "|" + (d ? "down" : "up")});
    });
    te.on_remote_toggled([&](bool v) { evs.push_back({"remote-toggled", v ? "1" : "0"}); });
    te.on_monitor_toggled([&](bool v) { evs.push_back({"monitor-toggled", v ? "1" : "0"}); });
    te.on_arm_clicked([&](int m) { evs.push_back({"arm-clicked", std::to_string(m)}); });
    te.on_data_flat_toggled([&](bool v) { evs.push_back({"data-flat-toggled", v ? "1" : "0"}); });

    // ---- the two states ----
    TransmitPageModel idle = blank();
    idle.address = ss("uri=ip:192.168.2.1");
    idle.address_enabled = true;
    idle.open_enabled = true;
    idle.open_tip = ss("Connects to the board.");
    idle.radio_line = ss("No transmitter is open.");
    idle.frequency = ss("100.300000");
    idle.following_note = ss("following the receiver");
    idle.power_text = ss("QUIET");
    idle.lamp_word = ss("NO RADIO");
    idle.not_armed_line = ss("Not armed: no plugin can key this transmitter.");
    idle.refusal_line = ss("Not armed: no transmitter is open. Open the radio above first.");

    TransmitPageModel open = blank();
    open.address = ss("uri=ip:192.168.2.1");
    open.address_enabled = false;
    open.have = true;
    open.close_enabled = true;
    open.radio_line = ss("PlutoSDR (test)");
    open.frequency = ss("145.500000");
    open.frequency_enabled = true;
    open.split = true;
    open.split_enabled = true;
    open.mode = 2;
    open.modes_enabled = true;
    open.power = 0.25f;
    open.power_text = ss("-60.0 dB");
    open.power_enabled = true;
    open.input = 1;
    open.inputs_enabled = true;
    open.tone_shown = true;
    open.tone_hz = ss("1000");
    open.tone_enabled = true;
    open.meter = 0.4f;
    open.lamp_word = ss("READY");
    open.lamp_tone = ChipTone::Warn;
    open.key_enabled = true;
    open.key_note = ss("no latch closes within 1000 ms of the last one ending (the latch timed out)");
    open.armed = true;
    open.armed_minutes = 15;
    open.armed_line = ss("ARMED: Test plugin may key this transmitter, 4:32 left.");
    open.data_flat = false;
    ui->set_m_idle(idle);
    ui->set_m_open(open);

    const auto shot = [&] { return render(ui, 1240, 700); };
    shot();

    // the control carrying `label` that is not a text, on the left (side 0) or right (side 1) page
    const auto find = [&](const std::string& label, int side, int nth = 0) -> std::optional<slint::testing::ElementHandle> {
        const auto all = slint::testing::ElementHandle::find_by_accessible_label(ui, label);
        int seen = 0;
        for (const auto& e : all) {
            if (e.accessible_role() == slint::language::AccessibleRole::Text) { continue; }
            const bool right = e.absolute_position().x >= 600.0f;
            if (right != (side == 1)) { continue; }
            if (seen++ == nth) { return e; }
        }
        return std::nullopt;
    };
    const auto textShown = [&](const std::string& label, int side) {
        const auto all = slint::testing::ElementHandle::find_by_accessible_label(ui, label);
        for (const auto& e : all) {
            if ((e.absolute_position().x >= 600.0f) == (side == 1)) { return true; }
        }
        return false;
    };
    const auto rectOf = [&](const std::string& label, int side, int nth = 0) {
        Rect r;
        const auto e = find(label, side, nth);
        if (!e) { return r; }
        const auto p = e->absolute_position();
        const auto s = e->size();
        r = Rect{p.x, p.y, s.width, s.height};
        return r;
    };
    const auto enabledOf = [&](const std::string& label, int side) {
        const auto e = find(label, side);
        if (!e) { return std::optional<bool>(); }
        return e->accessible_enabled();
    };
    const auto joined = [&] { std::string s; for (const Ev& e : evs) { s += (s.empty() ? "" : " | ") + e.str(); } return s; };
    const auto clickAt = [&](const Rect& r) { click(win, r.cx(), r.cy()); settle(milliseconds(20)); };

    // ================= NO RADIO: what is there, what is grey =================
    for (const char* l : {"PTT", "LATCH", "Open", "OFF", "5 MIN", "15 MIN", "60 MIN", "DATA (flat)", "Allow remote transmit", "Listen while transmitting", "SPLIT", "RADIO"}) {
        CHECKV(find(l, 0).has_value(), "idle page: no control \"%s\"", l);
    }
    CHECKV(!find("Close", 0).has_value(), "no Close key with no radio");
    CHECK(enabledOf("PTT", 0) == std::optional<bool>(false));
    CHECK(enabledOf("LATCH", 0) == std::optional<bool>(false));
    CHECK(enabledOf("Allow remote transmit", 0) == std::optional<bool>(false));
    CHECK(enabledOf("Listen while transmitting", 0) == std::optional<bool>(false));
    CHECK(enabledOf("SPLIT", 0) == std::optional<bool>(false));
    CHECK(enabledOf("Open", 0) == std::optional<bool>(true));
    CHECK(enabledOf("5 MIN", 0) == std::optional<bool>(true) && enabledOf("OFF", 0) == std::optional<bool>(true));   // the arming keys stay live: a press is how the page says why it cannot
    CHECK(enabledOf("DATA (flat)", 0) == std::optional<bool>(true));
    CHECK(textShown("NO RADIO", 0) && textShown("No transmitter is open.", 0) && textShown("following the receiver", 0));
    CHECK(textShown("Not armed: no plugin can key this transmitter.", 0) && textShown("Not armed: no transmitter is open. Open the radio above first.", 0));
    CHECK(textShown("This transmits. Licence sentence.", 0));

    // pressing what is grey reports nothing
    evs.clear();
    for (const char* l : {"PTT", "LATCH", "Allow remote transmit", "SPLIT", "Listen while transmitting"}) {
        const Rect r = rectOf(l, 0);
        CHECKV(r.ok(), "idle \"%s\" has no size", l);
        press(win, r.cx(), r.cy());
        settle(milliseconds(20));
        release(win, r.cx(), r.cy());
        settle(milliseconds(20));
    }
    CHECKV(evs.empty(), "grey controls reported: %s", joined().c_str());
    // a script or a screen reader cannot "click" them either
    for (const char* l : {"LATCH", "Allow remote transmit"}) {
        const auto e = find(l, 0);
        if (e) { e->invoke_accessible_default_action(); }
    }
    settle(milliseconds(20));
    CHECKV(evs.empty(), "default actions of grey controls reported: %s", joined().c_str());

    // the keys that work with no radio
    evs.clear();
    clickAt(rectOf("5 MIN", 0));
    clickAt(rectOf("15 MIN", 0));
    clickAt(rectOf("60 MIN", 0));
    clickAt(rectOf("OFF", 0));
    CHECKV(joined() == "arm-clicked 5 | arm-clicked 15 | arm-clicked 60 | arm-clicked 0", "arming keys: %s", joined().c_str());
    evs.clear();
    clickAt(rectOf("Open", 0));
    CHECKV(joined() == "open-clicked", "Open: %s", joined().c_str());
    evs.clear();
    clickAt(rectOf("DATA (flat)", 0));
    CHECKV(joined() == "data-flat-toggled 0", "DATA (flat) on -> off: %s", joined().c_str());

    // the lamp and the keys at rest: PTT greyed ground
    {
        const Frame f = shot();
        const Rect ptt = rectOf("PTT", 0);
        CHECKV(f.near(static_cast<int>(ptt.x) + 6, static_cast<int>(ptt.cy()), keyDisBg, 6), "idle PTT ground is not the disabled ground: %s", hex(f.at(static_cast<int>(ptt.x) + 6, static_cast<int>(ptt.cy()))).c_str());
    }

    // ================= A RADIO OPEN =================
    for (const char* l : {"Close", "SPLIT", "CW", "AM", "NFM", "USB", "LSB", "MIC", "TONE", "PTT", "LATCH", "5 MIN", "OFF"}) {
        CHECKV(find(l, 1).has_value(), "open page: no control \"%s\"", l);
    }
    CHECKV(!find("Open", 1).has_value(), "no Open key with a radio open");
    CHECK(enabledOf("PTT", 1) == std::optional<bool>(true) && enabledOf("LATCH", 1) == std::optional<bool>(true) && enabledOf("Allow remote transmit", 1) == std::optional<bool>(true));
    CHECK(textShown("PlutoSDR (test)", 1) && textShown("READY", 1));
    CHECK(textShown("ARMED: Test plugin may key this transmitter, 4:32 left.", 1));
    CHECK(textShown("no latch closes within 1000 ms of the last one ending (the latch timed out)", 1));
    CHECK(!textShown("following the receiver", 1));
    // the mode: NFM is the lit one (radio-button role, checked), the others not
    {
        const auto nfm = find("NFM", 1);
        CHECK(nfm && nfm->accessible_checked() == std::optional<bool>(true));
        const auto cw = find("CW", 1);
        CHECK(cw && cw->accessible_checked() == std::optional<bool>(false));
        const auto tone = find("TONE", 1);
        CHECK(tone && tone->accessible_checked() == std::optional<bool>(true));
        const auto split = find("SPLIT", 1);
        CHECK(split && split->accessible_checked() == std::optional<bool>(true));
        const auto latch = find("LATCH", 1);
        CHECK(latch && latch->accessible_checked() == std::optional<bool>(false));
        const auto flat = find("DATA (flat)", 1);
        CHECK(flat && flat->accessible_checked() == std::optional<bool>(false));
    }

    // the gestures
    evs.clear();
    clickAt(rectOf("Close", 1));
    clickAt(rectOf("SPLIT", 1));
    clickAt(rectOf("CW", 1));
    clickAt(rectOf("LSB", 1));
    clickAt(rectOf("MIC", 1));
    clickAt(rectOf("LATCH", 1));
    clickAt(rectOf("-", 1));
    clickAt(rectOf("+", 1));
    CHECKV(joined() == "close-clicked | split-clicked | mode-clicked 0 | mode-clicked 4 | input-clicked 0 | latch-clicked | frequency-stepped -1 | frequency-stepped 1", "gestures: %s",
           joined().c_str());
    evs.clear();
    clickAt(rectOf("Allow remote transmit", 1));
    clickAt(rectOf("Listen while transmitting", 1));
    clickAt(rectOf("DATA (flat)", 1));
    CHECKV(joined() == "remote-toggled 1 | monitor-toggled 1 | data-flat-toggled 1", "tick boxes: %s", joined().c_str());

    // ---- THE PTT IS A HOLD ----
    {
        const Rect r = rectOf("PTT", 1);
        CHECKV(r.ok() && r.w >= 150 && r.h >= 50, "PTT is %gx%g", r.w, r.h);
        evs.clear();
        press(win, r.cx(), r.cy());
        settle(milliseconds(20));
        CHECKV(joined() == "ptt-pointer 1", "press: %s", joined().c_str());
        CHECK(ui->get_open_ptt_down());
        const Frame down = shot();
        CHECKV(down.near(static_cast<int>(r.x) + 6, static_cast<int>(r.cy()), accent, 10), "pressed PTT ground: %s", hex(down.at(static_cast<int>(r.x) + 6, static_cast<int>(r.cy()))).c_str());
        release(win, r.cx(), r.cy());
        settle(milliseconds(20));
        CHECKV(joined() == "ptt-pointer 1 | ptt-pointer 0", "release: %s", joined().c_str());
        CHECK(!ui->get_open_ptt_down());

        // dragged off the key and released elsewhere: the hold still ends
        evs.clear();
        press(win, r.cx(), r.cy());
        settle(milliseconds(20));
        moveTo(win, 1200.0f, 690.0f);
        settle(milliseconds(20));
        CHECKV(joined() == "ptt-pointer 1", "dragged off, still held: %s", joined().c_str());
        release(win, 1200.0f, 690.0f);
        settle(milliseconds(20));
        CHECKV(joined() == "ptt-pointer 1 | ptt-pointer 0", "released off the key: %s", joined().c_str());

        // no accessible default action: a script or a screen reader cannot hold it
        evs.clear();
        const auto ptt = find("PTT", 1);
        if (ptt) { ptt->invoke_accessible_default_action(); }
        settle(milliseconds(20));
        CHECKV(evs.empty(), "the accessible default action of PTT reported: %s", joined().c_str());
        CHECK(ptt && !ptt->accessible_checkable().value_or(false));
    }

    // ---- THE CHORD: only with the PTT key focused; the focus leaving lets go ----
    {
        const Rect r = rectOf("PTT", 1);
        evs.clear();
        click(win, r.cx(), r.cy());   // a click focuses the key (and is a press and a release of the hold)
        settle(milliseconds(20));
        evs.clear();
        keyDown(win, u8" ");
        settle(milliseconds(20));
        keyUp(win, u8" ");
        settle(milliseconds(20));
        CHECKV(joined() == "ptt-chord  ||down | ptt-chord  ||up", "space down and up: %s", joined().c_str());
        // the focus leaving: Tab moves it, and the chord is reported up
        evs.clear();
        keyTap(win, u8"\t");
        settle(milliseconds(20));
        bool upSeen = false;
        for (const Ev& e : evs) { if (e.name == "ptt-chord" && e.args == "||up") { upSeen = true; } }
        CHECKV(upSeen, "the focus left the PTT key without a chord-up: %s", joined().c_str());
        // keys typed elsewhere (not on the PTT key) report nothing
        evs.clear();
        keyTap(win, u8" ");
        settle(milliseconds(20));
        bool chordSeen = false;
        for (const Ev& e : evs) { if (e.name == "ptt-chord" && e.args.find("down") != std::string::npos) { chordSeen = true; } }
        CHECKV(!chordSeen, "a key reached the chord with the PTT key not focused: %s", joined().c_str());
    }

    // ---- the colours that say ON AIR and LATCHED ----
    {
        TransmitPageModel keyed = open;
        keyed.on_air = true;
        keyed.latched = true;
        keyed.lamp_word = ss("ON AIR");
        keyed.lamp_tone = ChipTone::Err;
        ui->set_m_open(keyed);
        const Frame f = shot();
        const Rect ptt = rectOf("PTT", 1);
        CHECKV(f.near(static_cast<int>(ptt.x) + 6, static_cast<int>(ptt.cy()), lampRed, 8), "keyed PTT ground: %s", hex(f.at(static_cast<int>(ptt.x) + 6, static_cast<int>(ptt.cy()))).c_str());
        const Rect latch = rectOf("LATCH", 1);
        CHECKV(f.near(static_cast<int>(latch.x) + 6, static_cast<int>(latch.cy()), dangerBg, 8), "latched LATCH ground: %s", hex(f.at(static_cast<int>(latch.x) + 6, static_cast<int>(latch.cy()))).c_str());
        const auto l = find("LATCH", 1);
        CHECK(l && l->accessible_checked() == std::optional<bool>(true));
        CHECK(textShown("ON AIR", 1));
        ui->set_m_open(open);
        shot();
    }

    const Frame last = shot();
    last.savePng(shotDir() + "transmit-specimen-final.png");
    return testSummary("test_slint_transmit");
}
