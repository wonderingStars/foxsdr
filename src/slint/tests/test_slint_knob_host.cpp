// LANE X, row G5-16 (docs/multi-radio-records/lane-x.md): the Slint host's consumer of the knob layer (knob_host.cpp) with MADE-UP controllers: no device of any kind is opened by this test.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "knob_host.hpp"
#include "test_check.hpp"

#include <string>
#include <vector>

using namespace foxsdr::slintknobs;
namespace ci = cascade::input;

namespace {

struct Log {
    std::vector<std::string> calls;
    int opens = 0, closes = 0;
    bool failOpen = false;
};

Callbacks callbacks(Log& l) {
    Callbacks c;
    c.tune = [&l](int n) { l.calls.push_back("tune " + std::to_string(n)); };
    c.tuneFine = [&l](int n, int d) { l.calls.push_back("fine " + std::to_string(n) + "/" + std::to_string(d)); };
    c.volume = [&l](double a) { l.calls.push_back("volume " + std::to_string(static_cast<int>(a))); };
    c.squelch = [&l](double a) { l.calls.push_back("squelch " + std::to_string(static_cast<int>(a))); };
    c.zoom = [&l](double f) { l.calls.push_back(f > 1.0 ? "zoom in" : "zoom out"); };
    c.stepSize = [&l](int d) { l.calls.push_back("step " + std::to_string(d)); };
    c.mode = [&l](int d) { l.calls.push_back("mode " + std::to_string(d)); };
    c.radioSelect = [&l](int r) { l.calls.push_back("radio " + std::to_string(r)); };
    c.command = [&l](const std::string& id) { l.calls.push_back("command " + id); };
    return c;
}

KnobHost::Env env(Log& l) {
    KnobHost::Env e;
    e.list = [] {
        std::vector<FoundDevice> v;
        v.push_back({"hid:0b33:0020", "ShuttleXpress", 0, 0});
        v.push_back({"hid:077d:0410", "PowerMate", 1, 1});
        return v;
    };
    e.open = [&l](const FoundDevice& d, ci::EventQueue&, std::string& error) -> std::shared_ptr<void> {
        if (l.failOpen) { error = "Access is denied."; return nullptr; }
        ++l.opens;
        // a shared object whose end is the device's close
        return std::shared_ptr<void>(new int(static_cast<int>(d.token)), [&l](void* p) { ++l.closes; delete static_cast<int*>(p); });
    };
    e.act = callbacks(l);
    e.nowUs = [] { return std::int64_t{1000000}; };
    return e;
}

ci::InputEvent ev(const char* dev, std::uint16_t control, ci::EventKind kind, int raw) {
    ci::InputEvent e;
    e.device = ci::deviceIdFor(dev);
    e.control = control;
    e.kind = kind;
    e.raw = raw;
    e.timeUs = 1000000;
    return e;
}

ci::ActionOut act(const char* name, double amount, nlohmann::json params = nlohmann::json::object()) {
    ci::ActionOut a;
    a.action = name;
    a.amount = amount;
    a.params = std::move(params);
    return a;
}

void actions() {
    Log l;
    const Callbacks cb = callbacks(l);
    CHECK(applyAction(act("tune", 3), cb) && applyAction(act("tune", -2), cb));
    CHECK(applyAction(act("tune.shuttle", 1), cb));
    CHECK(applyAction(act("tune.fine", 2, {{"divisor", 5}}), cb) && applyAction(act("tune.fine", 2), cb));
    CHECK(applyAction(act("volume", 3), cb) && applyAction(act("squelch", -2), cb));
    CHECK(applyAction(act("zoom", 1), cb) && applyAction(act("zoom", -1), cb));
    CHECK(applyAction(act("step.up", 1), cb) && applyAction(act("step.down", 1), cb));
    CHECK(applyAction(act("mode.next", 1), cb) && applyAction(act("mode.prev", 1), cb));
    CHECK(applyAction(act("radio.select", 1, {{"n", 3}}), cb));
    CHECK(applyAction(act("command", 1, {{"name", "mute"}}), cb));
    const std::vector<std::string> want = {"tune 3", "tune -2", "tune 1", "fine 2/5", "fine 2/10", "volume 3", "squelch -2", "zoom in", "zoom out", "step 1", "step -1", "mode 1", "mode -1", "radio 3", "command mute"};
    CHECK(l.calls == want);
    // nothing to do, or not understood: false and no call
    l.calls.clear();
    CHECK(!applyAction(act("tune", 0), cb) && !applyAction(act("volume", 0), cb) && !applyAction(act("nonsense", 1), cb));
    CHECK(!applyAction(act("radio.select", 1, {{"n", 0}}), cb) && !applyAction(act("radio.select", 1, {{"n", 9}}), cb) && !applyAction(act("radio.select", 1), cb));
    CHECK(!applyAction(act("command", 1, {{"name", ""}}), cb) && !applyAction(act("command", 1), cb));
    CHECK(l.calls.empty());
    // THE SECOND LOCK: a knob never keys a transmitter, by whatever route
    CHECK(!applyAction(act("command", 1, {{"name", "transmit"}}), cb) && !applyAction(act("command", 1, {{"name", "PTT"}}), cb));
    CHECK(!applyAction(act("transmit.key", 1), cb) && !applyAction(act("ptt", 1), cb));
    CHECK(l.calls.empty());
    // a callback the window did not give is no crash
    CHECK(!applyAction(act("tune", 3), Callbacks{}) && !applyAction(act("command", 1, {{"name", "mute"}}), Callbacks{}));
}

void nothingOpensUntilAsked() {
    Log l;
    KnobHost h(env(l));
    CHECK(!h.enabled() && l.opens == 0 && h.status().empty());
    h.scan();                                            // a listing only
    CHECK(h.status().size() == 2 && l.opens == 0);
    CHECK(!h.status()[0].use && !h.status()[0].open);
    CHECK(h.setUse("hid:0b33:0020", true));              // marked, but the master switch is off: still nothing open
    CHECK(l.opens == 0 && h.status()[0].use && !h.status()[0].open);
    h.setEnabled(true);                                  // now the marked one opens, the other does not
    CHECK(l.opens == 1 && h.status()[0].open && !h.status()[1].open);
    CHECK(h.setUse("hid:077d:0410", true) && l.opens == 2);
    CHECK(!h.setUse("hid:dead:beef", true));             // not listed
    h.setUse("hid:0b33:0020", false);                    // unmarked closes it
    CHECK(l.closes == 1 && !h.status()[0].open);
    h.setEnabled(false);                                 // the switch off closes the rest
    CHECK(l.closes == 2 && !h.status()[1].open && h.status()[1].use);   // (the choice is kept)
    h.setEnabled(true);
    CHECK(l.opens == 3 && h.status()[1].open);           // and back on, it opens again
}

void savedChoice() {
    Log l;
    KnobHost h(env(l));
    h.setUsedIds({"hid:077d:0410"});
    CHECK(h.usedIds() == std::vector<std::string>{"hid:077d:0410"} && l.opens == 0);
    h.scan();
    CHECK(l.opens == 0);                                 // marked, switch off
    h.setEnabled(true);
    CHECK(l.opens == 1 && h.status()[1].open && !h.status()[0].open);
}

void failures() {
    Log l;
    l.failOpen = true;
    KnobHost h(env(l));
    h.scan();
    h.setEnabled(true);
    h.setUse("hid:0b33:0020", true);
    CHECK(!h.status()[0].open && h.status()[0].error == "Access is denied.");    // the reason is in the status, which the window shows
    l.failOpen = false;
    h.scan();                                                                    // looking again tries again
    CHECK(h.status()[0].open && h.status()[0].error.empty());
}

void events() {
    Log l;
    KnobHost h(env(l));
    h.scan();
    h.setEnabled(true);
    h.setUse("hid:0b33:0020", true);
    auto& q = h.queue();
    // the jog: control 1, relative; a button: 0x13 (the fourth) is mute; 0x12 next mode; 0x10 / 0x11 the step
    q.push(ev("hid:0b33:0020", 1, ci::EventKind::Relative, 2));
    q.push(ev("hid:0b33:0020", 0x13, ci::EventKind::ButtonDown, 1));
    q.push(ev("hid:0b33:0020", 0x12, ci::EventKind::ButtonDown, 1));
    q.push(ev("hid:0b33:0020", 0x10, ci::EventKind::ButtonDown, 1));
    q.push(ev("hid:0b33:0020", 0x11, ci::EventKind::ButtonDown, 1));
    q.push(ev("hid:0b33:0020", 0x14, ci::EventKind::ButtonDown, 1));
    CHECK(h.tick() == 6);
    CHECK((l.calls == std::vector<std::string>{"tune 2", "command mute", "mode 1", "step -1", "step 1", "command record"}));
    CHECK(h.eventsSeen() == 6);
    // an event of a device that is not in use does nothing
    l.calls.clear();
    q.push(ev("hid:077d:0410", 1, ci::EventKind::Relative, 1));
    q.push(ev("hid:9999:0001", 1, ci::EventKind::Relative, 1));
    CHECK(h.tick() == 0 && l.calls.empty());
    // with the master switch off nothing is read at all
    h.setEnabled(false);
    q.push(ev("hid:0b33:0020", 1, ci::EventKind::Relative, 1));
    CHECK(h.tick() == 0 && l.calls.empty());
}

void unplug() {
    Log l;
    KnobHost h(env(l));
    h.scan();
    h.setEnabled(true);
    h.setUse("hid:077d:0410", true);
    CHECK(h.status()[1].open);
    h.queue().push(ev("hid:077d:0410", 0, ci::EventKind::Disconnected, 0));
    h.tick();
    CHECK(!h.status()[1].open && h.status()[1].error == "The device was unplugged." && l.closes == 1);
    h.queue().push(ev("hid:077d:0410", 1, ci::EventKind::Relative, 1));   // a late event of the lost one does nothing
    CHECK(h.tick() == 0 && l.calls.empty());
}

void powerMate() {
    Log l;
    KnobHost h(env(l));
    h.scan();
    h.setEnabled(true);
    h.setUse("hid:077d:0410", true);
    h.queue().push(ev("hid:077d:0410", 1, ci::EventKind::Relative, -3));
    h.queue().push(ev("hid:077d:0410", 0x10, ci::EventKind::ButtonDown, 1));
    CHECK(h.tick() == 2 && (l.calls == std::vector<std::string>{"tune -3", "command mute"}));
}

}  // namespace

int main() {
    actions();
    nothingOpensUntilAsked();
    savedChoice();
    failures();
    events();
    unplug();
    powerMate();
    return testSummary("test_slint_knob_host");
}
