// The mapping layer: JSON round trip, unknown fields kept, absent devices, malformed and hostile files, the transmit refusal, acceleration, the shuttle ring's
// dead zone and repeat rate, the modifier button, and a disconnect. Pure: time is injected.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cmath>
#include <string>

#include "input/hid_parse.hpp"
#include "input/mapping.hpp"
#include "test_check.hpp"

using namespace cascade::input;
using nlohmann::json;

static const std::string kShuttle = "hid:0b33:0020";
static const std::string kMidi = "midi:Test Controller";

static InputEvent rel(const std::string& dev, std::uint16_t ctl, int raw, std::int64_t tUs) {
    InputEvent e;
    e.device = deviceIdFor(dev);
    e.control = ctl;
    e.kind = EventKind::Relative;
    e.raw = raw;
    e.timeUs = tUs;
    return e;
}
static InputEvent btn(const std::string& dev, std::uint16_t ctl, bool down, std::int64_t tUs = 0) {
    InputEvent e;
    e.device = deviceIdFor(dev);
    e.control = ctl;
    e.kind = down ? EventKind::ButtonDown : EventKind::ButtonUp;
    e.value = down ? 1.0f : 0.0f;
    e.timeUs = tUs;
    return e;
}
static InputEvent ring(const std::string& dev, int raw, std::int64_t tUs) {
    InputEvent e;
    e.device = deviceIdFor(dev);
    e.control = kRingControl;
    e.kind = EventKind::Absolute;
    e.raw = raw;
    e.value = 0.5f + raw / 14.0f;
    e.timeUs = tUs;
    return e;
}

int main() {
    // ---- ROUND TRIP -------------------------------------------------------------------------------------------------------------------------------
    {
        Settings s = shuttleXpressPreset(kShuttle);
        s.bindings[0].accel.maxFactor = 4.0;
        s.bindings[0].invert = true;
        s.bindings[1].deadZone = 2;
        s.bindings[1].minRateHz = 3;
        s.bindings[1].maxRateHz = 40;
        Binding m;
        m.device = kMidi;
        m.control = midiCcControl(1, 20);
        m.action = "volume";
        m.encoding = MidiEncoding::RelBinaryOffset;
        m.params = {{"scale", 0.5}};
        s.bindings.push_back(m);
        s.modifier = Modifier{kShuttle, kButtonBase + 4, 7.0};
        const std::string text = settingsToText(s);
        LoadReport rep;
        Settings back = settingsFromText(text, &rep);
        CHECK(rep.bindingsRead == static_cast<int>(s.bindings.size()) && rep.skippedMalformed == 0 && rep.clamped == 0 && !rep.unparseable);
        CHECK(settingsToJson(back) == settingsToJson(s));
        CHECK(back.bindings.size() == s.bindings.size());
        CHECK(back.bindings[0].invert && back.bindings[0].accel.maxFactor == 4.0);
        CHECK(back.bindings[1].deadZone == 2 && back.bindings[1].minRateHz == 3 && back.bindings[1].maxRateHz == 40);
        CHECK(back.bindings.back().encoding == MidiEncoding::RelBinaryOffset && back.bindings.back().params["scale"] == 0.5);
        CHECK(back.modifier && back.modifier->factor == 7.0 && back.modifier->control == kButtonBase + 4);
        CHECK(settingsToText(back) == text);  // idempotent
    }
    // ---- UNKNOWN FIELDS are tolerated and kept ----------------------------------------------------------------------------------------------------
    {
        const char* text = R"({"version":3,"future":{"a":1},"bindings":[
            {"device":"hid:0b33:0020","control":1,"action":"tune","colour":"red","accel":{"maxFactor":2,"slowPerSec":5,"fastPerSec":10,"shape":"s"}}]})";
        LoadReport rep;
        Settings s = settingsFromText(text, &rep);
        CHECK(rep.bindingsRead == 1 && s.version == 3 && s.bindings.size() == 1 && s.bindings[0].accel.maxFactor == 2.0);
        const json out = settingsToJson(s);
        CHECK(out.contains("future") && out["future"].value("a", 0) == 1);
        CHECK(out["bindings"][0].value("colour", std::string()) == "red");
    }
    // ---- MALFORMED: nothing throws, bad entries are counted and skipped, good ones survive ----------------------------------------------------------
    {
        LoadReport rep;
        Settings s = settingsFromText("this is not json {", &rep);
        CHECK(rep.unparseable && s.bindings.empty());
        for (const char* t : {"", "[]", "42", "null", "\"x\"", "{}", R"({"bindings":5})", R"({"bindings":{}})", R"({"modifier":7})"}) {
            LoadReport r2;
            Settings x = settingsFromText(t, &r2);
            CHECK(x.bindings.empty());
        }
        LoadReport r3;
        Settings m = settingsFromText(R"({"bindings":[
            7, null, "x", {},
            {"device":"d","control":1},
            {"device":"d","action":"tune"},
            {"device":5,"control":1,"action":"tune"},
            {"device":"d","control":-1,"action":"tune"},
            {"device":"d","control":70000,"action":"tune"},
            {"device":"d","control":1.5,"action":"tune"},
            {"device":"","control":1,"action":"tune"},
            {"device":"good","control":9,"action":"volume"}]})", &r3);
        CHECK(m.bindings.size() == 1 && m.bindings[0].device == "good" && r3.skippedMalformed == 11 && r3.bindingsRead == 1);
    }
    // ---- CLAMPING ---------------------------------------------------------------------------------------------------------------------------------
    {
        LoadReport rep;
        Settings s = settingsFromText(R"({"bindings":[{"device":"d","control":2,"action":"tune.shuttle","deadZone":99,"minRateHz":500,"maxRateHz":-4,
            "accel":{"maxFactor":1000,"slowPerSec":50,"fastPerSec":10}}]})", &rep);
        CHECK(s.bindings.size() == 1 && rep.clamped >= 3);
        const Binding& b = s.bindings[0];
        CHECK(b.deadZone == 6 && b.minRateHz <= b.maxRateHz && b.minRateHz >= 0.1 && b.maxRateHz <= 200.0);
        CHECK(b.accel.maxFactor == 20.0 && b.accel.fastPerSec > b.accel.slowPerSec);
    }
    // ---- THE TRANSMIT REFUSAL ---------------------------------------------------------------------------------------------------------------------
    {
        CHECK(isForbiddenName("transmitPtt") && isForbiddenName("PTT") && isForbiddenName("ptt.hold") && isForbiddenName("Transmit"));
        CHECK(!isForbiddenName("tune") && !isForbiddenName("mute"));
        LoadReport rep;
        Settings s = settingsFromText(R"({"bindings":[
            {"device":"d","control":1,"action":"transmitPtt"},
            {"device":"d","control":2,"action":"command","params":{"name":"transmitPtt"}},
            {"device":"d","control":3,"action":"ptt"},
            {"device":"d","control":4,"action":"command","params":{"name":"mute"}}]})", &rep);
        CHECK(rep.refusedForbidden == 3 && s.bindings.size() == 1 && s.bindings[0].control == 4);
        Mapper m(s);
        CHECK(m.process(btn("d", 1, true)).empty());
    }
    // ---- DUPLICATES: a later entry for the same control replaces the earlier -----------------------------------------------------------------------
    {
        Settings s = settingsFromText(R"({"bindings":[{"device":"d","control":1,"action":"tune"},{"device":"d","control":1,"action":"volume"}]})");
        CHECK(s.bindings.size() == 1 && s.bindings[0].action == "volume");
    }
    // ---- KNOWN ACTIONS ----------------------------------------------------------------------------------------------------------------------------
    {
        CHECK(isKnownAction("tune") && isKnownAction("tune.shuttle") && isKnownAction("command") && !isKnownAction("explode"));
        CHECK(kActionCount == 12);
        // An unknown action loads (a newer file) and is passed through for the consumer to ignore.
        Settings s = settingsFromText(R"({"bindings":[{"device":"d","control":1,"action":"future.thing"}]})");
        CHECK(s.bindings.size() == 1);
    }
    // ---- ABSENT DEVICE: bindings for a device that never appears are inert; events from an unbound device and control do nothing --------------------
    {
        Mapper m(shuttleXpressPreset("hid:dead:beef"));
        CHECK(m.process(rel(kShuttle, 1, 1, 0)).empty());
        Mapper m2(shuttleXpressPreset(kShuttle));
        CHECK(m2.process(rel(kShuttle, 77, 1, 0)).empty());
        CHECK(m2.process(rel("hid:other", 1, 1, 0)).empty());
        CHECK(m2.tick(1000000).empty());
        Mapper none;
        CHECK(none.process(rel(kShuttle, 1, 1, 0)).empty() && none.tick(5).empty());
    }
    // ---- RELATIVE: amount, scale, invert, zero ----------------------------------------------------------------------------------------------------
    {
        Settings s;
        Binding b;
        b.device = kShuttle; b.control = 1; b.action = "tune"; b.params = {{"scale", 2.0}};
        s.bindings.push_back(b);
        Mapper m(s);
        auto a = m.process(rel(kShuttle, 1, 3, 0));
        CHECK(a.size() == 1 && a[0].action == "tune" && a[0].amount == 6.0 && !a[0].absolute);
        a = m.process(rel(kShuttle, 1, -1, 10));
        CHECK(a.size() == 1 && a[0].amount == -2.0);
        CHECK(m.process(rel(kShuttle, 1, 0, 20)).empty());
        s.bindings[0].invert = true;
        m.setSettings(s);
        a = m.process(rel(kShuttle, 1, 1, 0));
        CHECK(a.size() == 1 && a[0].amount == -2.0);
    }
    // ---- ACCELERATION -----------------------------------------------------------------------------------------------------------------------------
    {
        Settings s;
        Binding b;
        b.device = kShuttle; b.control = 1; b.action = "tune";
        b.accel = {10.0, 50.0, 5.0};
        s.bindings.push_back(b);
        Mapper m(s);
        // First click: no previous event, speed 0, factor 1.
        CHECK(m.process(rel(kShuttle, 1, 1, 0))[0].amount == 1.0);
        // 1 click after 1 s = 1 /s, below slowPerSec: factor 1.
        CHECK(m.process(rel(kShuttle, 1, 1, 1000000))[0].amount == 1.0);
        // 1 click after 10 ms = 100 /s, above fastPerSec: factor 5.
        CHECK(std::fabs(m.process(rel(kShuttle, 1, 1, 1010000))[0].amount - 5.0) < 1e-9);
        // 1 click after 50 ms = 20 /s: t = (20 - 10) / 40 = 0.25, factor 2.
        CHECK(std::fabs(m.process(rel(kShuttle, 1, 1, 1060000))[0].amount - 2.0) < 1e-9);
        // Direction keeps its sign.
        CHECK(m.process(rel(kShuttle, 1, -1, 1070000))[0].amount < -4.9);
        // maxFactor 1 (the default) is off however fast.
        s.bindings[0].accel.maxFactor = 1.0;
        m.setSettings(s);
        m.process(rel(kShuttle, 1, 1, 0));
        CHECK(m.process(rel(kShuttle, 1, 1, 1000))[0].amount == 1.0);
    }
    // ---- BUTTONS ----------------------------------------------------------------------------------------------------------------------------------
    {
        Mapper m(shuttleXpressPreset(kShuttle));
        auto a = m.process(btn(kShuttle, 0x11, true));
        CHECK(a.size() == 1 && a[0].action == "step.up" && a[0].amount == 1.0);
        CHECK(m.process(btn(kShuttle, 0x11, false)).empty());  // release does nothing
        a = m.process(btn(kShuttle, 0x13, true));
        CHECK(a.size() == 1 && a[0].action == "command" && a[0].params["name"] == "mute");
        // A button bound to a relative action steps by params.amount.
        Settings s;
        Binding b;
        b.device = kMidi; b.control = midiNoteControl(0, 36); b.action = "volume"; b.params = {{"amount", -2}};
        s.bindings.push_back(b);
        Mapper m2(s);
        a = m2.process(btn(kMidi, midiNoteControl(0, 36), true));
        CHECK(a.size() == 1 && a[0].amount == -2.0);
    }
    // ---- ABSOLUTE (volume / squelch / zoom), with invert; ignored for tune ------------------------------------------------------------------------
    {
        Settings s;
        Binding v;
        v.device = kMidi; v.control = midiCcControl(0, 7); v.action = "volume";
        Binding t = v; t.control = midiCcControl(0, 8); t.action = "tune";
        Binding i = v; i.control = midiCcControl(0, 9); i.invert = true;
        s.bindings = {v, t, i};
        Mapper m(s);
        InputEvent e;
        e.device = deviceIdFor(kMidi); e.kind = EventKind::Absolute; e.value = 0.25f; e.raw = 32;
        e.control = midiCcControl(0, 7);
        auto a = m.process(e);
        CHECK(a.size() == 1 && a[0].absolute && std::fabs(a[0].amount - 0.25) < 1e-6);
        e.control = midiCcControl(0, 8);
        CHECK(m.process(e).empty());
        e.control = midiCcControl(0, 9);
        a = m.process(e);
        CHECK(a.size() == 1 && std::fabs(a[0].amount - 0.75) < 1e-6);
    }
    // ---- MIDI CONFIG HAND-OVER -------------------------------------------------------------------------------------------------------------------
    {
        Settings s;
        Binding b;
        b.device = kMidi; b.control = midiCcControl(0, 30); b.action = "tune"; b.encoding = MidiEncoding::RelTwosComplement; b.invert = true;
        Binding o = b; o.device = "midi:Other";
        s.bindings = {b, o};
        Mapper m(s);
        const auto cfg = m.midiConfigsFor(kMidi);
        CHECK(cfg.size() == 1 && cfg[0].first == midiCcControl(0, 30) && cfg[0].second.encoding == MidiEncoding::RelTwosComplement && !cfg[0].second.invert);
        CHECK(m.midiConfigsFor("midi:Absent").empty());
    }
    // ---- SHUTTLE RING: dead zone, first detent at once, rate by deflection, direction, return to centre ------------------------------------------
    {
        Settings s;
        Binding r;
        r.device = kShuttle; r.control = kRingControl; r.action = "tune.shuttle";
        r.deadZone = 1; r.minRateHz = 2.0; r.maxRateHz = 20.0;
        s.bindings.push_back(r);
        Mapper m(s);
        // Inside the dead zone (|1| <= 1): nothing at all, and ticks stay silent.
        CHECK(m.process(ring(kShuttle, 1, 0)).empty());
        CHECK(m.tick(500000).empty() && m.tick(1000000).empty());
        // Deflection 2 = first step past the dead zone: one detent now, then minRate 2 /s.
        auto a = m.process(ring(kShuttle, 2, 2000000));
        CHECK(a.size() == 1 && a[0].amount == 1.0 && a[0].action == "tune.shuttle");
        CHECK(m.tick(2200000).empty());                       // 0.2 s * 2 = 0.4
        CHECK(m.tick(2450000).empty());                       // +0.25 s * 2 = 0.9
        a = m.tick(2550000);                                  // +0.1 s * 2 = 1.1: one detent
        CHECK(a.size() == 1 && a[0].amount == 1.0);
        // Full deflection 7: 20 /s. 0.25 s -> 5 detents in one tick.
        m.process(ring(kShuttle, 7, 2550000));
        a = m.tick(2800000);
        CHECK(a.size() == 1 && a[0].amount == 5.0);
        // Negative deflection: opposite sign, the first detent at once.
        a = m.process(ring(kShuttle, -7, 2800000));
        CHECK(a.size() == 1 && a[0].amount == -1.0);
        a = m.tick(3050000);
        CHECK(a.size() == 1 && a[0].amount == -5.0);
        // Back to centre: silence, repeat stops.
        CHECK(m.process(ring(kShuttle, 0, 3050000)).empty());
        CHECK(m.tick(4000000).empty());
        // A stall of seconds does not release a burst: one tick is capped at a quarter of a second.
        m.process(ring(kShuttle, 7, 5000000));
        a = m.tick(15000000);
        CHECK(a.size() == 1 && a[0].amount == 5.0);
        // Moving between two deflections on the same side adds no instant detent.
        CHECK(m.process(ring(kShuttle, 4, 15000000)).empty());
    }
    // ---- MODIFIER BUTTON: x factor while down -----------------------------------------------------------------------------------------------------
    {
        Settings s = shuttleXpressPreset(kShuttle);
        s.modifier = Modifier{kShuttle, kButtonBase + 4, 10.0};
        Mapper m(s);
        CHECK(m.process(rel(kShuttle, 1, 1, 0))[0].amount == 1.0);
        m.process(btn(kShuttle, kButtonBase + 4, true));
        CHECK(m.process(rel(kShuttle, 1, 2, 10))[0].amount == 20.0);
        m.process(btn(kShuttle, kButtonBase + 4, false));
        CHECK(m.process(rel(kShuttle, 1, 2, 20))[0].amount == 2.0);
    }
    // ---- DISCONNECT clears the ring and the modifier ---------------------------------------------------------------------------------------------
    {
        Settings s = shuttleXpressPreset(kShuttle);
        s.modifier = Modifier{kShuttle, kButtonBase + 4, 10.0};
        Mapper m(s);
        m.process(ring(kShuttle, 7, 0));
        m.process(btn(kShuttle, kButtonBase + 4, true));
        InputEvent d;
        d.device = deviceIdFor(kShuttle);
        d.kind = EventKind::Disconnected;
        CHECK(m.process(d).empty());
        CHECK(m.tick(200000).empty());
        CHECK(m.process(rel(kShuttle, 1, 1, 300000))[0].amount == 1.0);
    }
    // ---- A NaN or hostile number in params does not poison an amount -----------------------------------------------------------------------------
    {
        Settings s = settingsFromText(R"({"bindings":[{"device":"d","control":1,"action":"tune","params":{"scale":"loud"}},{"device":"d","control":2,"action":"tune","params":{"scale":1e300}}]})");
        Mapper m(s);
        CHECK(m.process(rel("d", 1, 1, 0))[0].amount == 1.0);
        CHECK(std::fabs(m.process(rel("d", 2, 1, 0))[0].amount) <= 1000.0);
    }
    // ---- PRESETS load back unchanged ------------------------------------------------------------------------------------------------------------
    {
        for (const Settings& s : {shuttleXpressPreset(kShuttle), powerMatePreset("hid:077d:0410")}) {
            LoadReport rep;
            Settings back = settingsFromText(settingsToText(s), &rep);
            CHECK(rep.bindingsRead == static_cast<int>(s.bindings.size()) && rep.refusedForbidden == 0 && back.bindings.size() == s.bindings.size());
            for (const Binding& b : s.bindings) { CHECK(isKnownAction(b.action) && !isForbiddenName(b.action)); }
        }
    }
    return testSummary("test_input_mapping");
}
