// The Slint window's scripted input (src/slint/input_script.hpp): the script's parser and the driver that runs its steps against a
// target, tested here against a FAKE target that records every call and answers what the test tells it to. No Slint, no engine:
// the real target (input_target.hpp) is exercised by test_slint_drive_run, which runs the window itself.
//
// What is held: the grammar (ticks absolute and relative, the wait, quoted labels with escapes, every verb's arguments, a '#'
// comment, the refusals - a tick going backwards, an unknown verb, a missing argument, a bad fraction, a bad key name); the
// driver (steps run in order at their tick and never twice, a refusal counts as a failure and the run goes on, a comparison of
// numbers is numeric and of words is exact, "contains" is a substring, a step whose tick never comes is "not reached", the
// summary line says all four counts); and the label index suffix ("Close"#2).
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "input_script.hpp"
#include "test_check.hpp"

#include <cstdio>
#include <map>
#include <string>
#include <vector>

using namespace foxsdr::slintdrive;

namespace {

struct FakeTarget : DriveTarget {
    std::vector<std::string> calls;
    std::map<std::string, std::string> values;   // "<label>/<what>" -> what read() answers
    std::map<std::string, std::string> states;   // name -> what state() answers
    std::string refuseLabel;                      // click/press/type/slider/choose on this label is refused

    std::string refusal(const std::string& label) const { return label == refuseLabel ? "no control labelled \"" + label + "\"" : std::string(); }
    std::string click(const std::string& label) override { calls.push_back("click " + label); return refusal(label); }
    std::string press(const std::string& label) override { calls.push_back("press " + label); return refusal(label); }
    std::string type(const std::string& label, const std::string& text) override { calls.push_back("type " + label + "=" + text); return refusal(label); }
    std::string key(const std::string& name) override { calls.push_back("key " + name); return std::string(); }
    std::string slider(const std::string& label, double frac) override { calls.push_back("slider " + label + "=" + std::to_string(frac)); return refusal(label); }
    std::string scroll(const std::string& label, double px) override { calls.push_back("scroll " + label + "=" + std::to_string(px)); return refusal(label); }
    std::string open(const std::string& label) override { calls.push_back("open " + label); return refusal(label); }
    std::string pick(const std::string& item) override { calls.push_back("pick " + item); return refusal(item); }
    std::string read(const std::string& label, const std::string& what, std::string& out) override {
        calls.push_back("read " + label + "/" + what);
        const auto it = values.find(label + "/" + what);
        if (it == values.end()) { return "\"" + label + "\" has no " + what; }
        out = it->second;
        return std::string();
    }
    std::string state(const std::string& name, std::string& out) override {
        calls.push_back("state " + name);
        const auto it = states.find(name);
        if (it == states.end()) { return "no state named " + name; }
        out = it->second;
        return std::string();
    }
    std::string shot(const std::string& file) override { calls.push_back("shot " + file); return std::string(); }
    std::string dumpControls() override { calls.push_back("dump"); return std::string(); }
};

std::vector<std::string> said;
void say(const std::string& s) { said.push_back(s); }

bool anySaid(const std::string& needle) {
    for (const auto& s : said) { if (s.find(needle) != std::string::npos) { return true; } }
    return false;
}

}  // namespace

int main() {
    std::printf("test_slint_host_input\n");

    // ---- the grammar ----
    {
        const Script s = parseInputScript(
            "# a comment\n"
            "\n"
            "20 dump\n"
            "click \"Stop the receiver (Ctrl+F2)\"   # same tick as the dump\n"
            "+10 state running == 0\n"
            "+1 type \"Bookmark name\" \"Probe \\\"one\\\"\"\n"
            "wait 500\n"
            "click \"Add current\"\n"
            "+15 state count.bookmarks >= 1\n"
            "expect \"Bookmark name\" value equals \"\"\n"
            "key enter\n"
            "key ctrl+a\n"
            "slider \"Volume\" 0.25\n"
            "choose \"Tune step\" \"12.5 kHz\"\n"
            "press \"NFM\"#2\n"
            "state mode contains FM\n"
            "scroll \"Sections\" -300\n"
            "shot \"drive.png\"\n");
        CHECK(s.errors.empty());
        for (const auto& e : s.errors) { std::printf("  unexpected error: %s\n", e.c_str()); }
        CHECK(s.steps.size() == 15);
        CHECK(s.steps.size() >= 14 && s.steps[13].verb == Verb::Scroll && s.steps[13].label == "Sections" && s.steps[13].number == -300);
        if (s.steps.size() == 14) {
            CHECK(s.steps[0].verb == Verb::Dump && s.steps[0].tick == 20 && s.steps[0].line == 3);
            CHECK(s.steps[1].verb == Verb::Click && s.steps[1].tick == 20 && s.steps[1].label == "Stop the receiver (Ctrl+F2)");
            CHECK(s.steps[2].verb == Verb::State && s.steps[2].tick == 30 && s.steps[2].label == "running" && s.steps[2].what == "==" && s.steps[2].text == "0");
            CHECK(s.steps[3].verb == Verb::Type && s.steps[3].tick == 31 && s.steps[3].label == "Bookmark name" && s.steps[3].text == "Probe \"one\"");
            // wait 500 ms = 16 ticks of 33 ms (rounded up), from the tick of the step before it
            CHECK(s.steps[4].verb == Verb::Click && s.steps[4].tick == 47 && s.steps[4].label == "Add current");
            CHECK(s.steps[5].verb == Verb::State && s.steps[5].tick == 62 && s.steps[5].what == ">=" && s.steps[5].text == "1");
            CHECK(s.steps[6].verb == Verb::Expect && s.steps[6].tick == 62 && s.steps[6].label == "Bookmark name" && s.steps[6].what == "value" && s.steps[6].compare == "equals" && s.steps[6].text.empty());
            CHECK(s.steps[7].verb == Verb::Key && s.steps[7].text == "enter");
            CHECK(s.steps[8].verb == Verb::Key && s.steps[8].text == "ctrl+a");
            CHECK(s.steps[9].verb == Verb::Slider && s.steps[9].label == "Volume" && s.steps[9].number > 0.249 && s.steps[9].number < 0.251);
            CHECK(s.steps[10].verb == Verb::Choose && s.steps[10].label == "Tune step" && s.steps[10].text == "12.5 kHz");
            CHECK(s.steps[11].verb == Verb::Press && s.steps[11].label == "NFM#2");
            CHECK(s.steps[12].verb == Verb::State && s.steps[12].what == "contains" && s.steps[12].text == "FM");
            CHECK(s.steps[13].verb == Verb::Shot && s.steps[13].label == "drive.png");
        }
    }
    // the first step without a tick runs at tick 1; a quoted value for state
    {
        const Script s = parseInputScript("click \"A\"\nstate deviceName == \"Signal generator\"\n");
        CHECK(s.errors.empty());
        CHECK(s.steps.size() == 2 && s.steps[0].tick == 1 && s.steps[1].text == "Signal generator");
    }
    // the refusals, each named by its line
    {
        const Script s = parseInputScript(
            "30 click \"A\"\n"          // 1
            "20 click \"B\"\n"          // 2: backwards
            "frob \"A\"\n"              // 3: unknown verb
            "click\n"                   // 4: missing label
            "slider \"V\" 1.5\n"        // 5: fraction out of range
            "key wobble\n"              // 6: unknown key
            "expect \"A\" colour equals \"x\"\n"   // 7: unknown property
            "state x ~ 1\n"             // 8: unknown comparison
            "type \"A\" unquoted\n"     // 9: text not quoted
            "wait 0\n"                  // 10: a wait of nothing
            "click \"A\" extra\n"       // 11: trailing words
            "scroll \"A\" far\n");      // 12: not a number of pixels
        CHECK(s.errors.size() == 11);
        for (const auto& e : s.errors) { std::printf("  (expected) %s\n", e.c_str()); }
        bool lines[13] = {};
        for (const auto& e : s.errors) {
            int n = 0;
            if (std::sscanf(e.c_str(), "line %d", &n) == 1 && n >= 1 && n <= 12) { lines[n] = true; }
        }
        for (int n = 2; n <= 12; ++n) { CHECK(lines[n]); }
        CHECK(!lines[1]);
        CHECK(s.steps.size() == 1);   // the good line still parses: a report of EVERY bad line, not just the first
    }
    // the label index suffix
    {
        std::string label = "Close#2";
        int index = 0;
        CHECK(splitLabelIndex(label, index) && label == "Close" && index == 2);
        label = "Close";
        CHECK(splitLabelIndex(label, index) && label == "Close" && index == 1);
        label = "Channel #12";   // a '#' inside the label is not an index
        CHECK(splitLabelIndex(label, index) && label == "Channel #12" && index == 1);
        label = "A#0";
        CHECK(!splitLabelIndex(label, index));
    }

    // ---- the driver ----
    {
        const Script s = parseInputScript(
            "5 click \"A\"\n"
            "state running == 1\n"
            "+5 click \"Nope\"\n"
            "state tunedHz == 100300000\n"
            "state mode contains FM\n"
            "expect \"Volume\" value equals \"72 %\"\n"
            "expect \"Volume\" checked equals \"true\"\n"
            "state tunedHz < 1e6\n"
            "+100 dump\n");
        CHECK(s.errors.empty());
        FakeTarget t;
        t.refuseLabel = "Nope";
        t.states["running"] = "1";
        t.states["tunedHz"] = "100300000.000000";   // the host prints doubles: compared as numbers
        t.states["mode"] = "WFM";
        t.values["Volume/value"] = "72 %";
        Driver d(s, t);
        said.clear();
        for (long tick = 1; tick <= 4; ++tick) { d.tick(tick, say); }
        CHECK(t.calls.empty());                    // nothing before tick 5
        d.tick(5, say);
        CHECK(t.calls.size() == 2 && t.calls[0] == "click A" && t.calls[1] == "state running");
        d.tick(5, say);                            // the same tick again runs nothing twice
        CHECK(t.calls.size() == 2);
        for (long tick = 6; tick <= 12; ++tick) { d.tick(tick, say); }
        CHECK(t.calls.size() == 8);
        const DriveReport r = d.report();
        CHECK(r.steps == 9);
        CHECK(r.ran == 8);
        CHECK(r.notReached == 1);
        // failed: "Nope" refused, Volume has no "checked", tunedHz < 1e6 false
        CHECK(r.failed == 3);
        CHECK(anySaid("line 3") && anySaid("no control labelled \"Nope\""));
        CHECK(anySaid("line 7") && anySaid("has no checked"));
        CHECK(anySaid("line 8") && anySaid("100300000"));
        CHECK(anySaid("line 1") && anySaid("ok"));
        CHECK(d.summaryLine() == "input script: 9 steps, 8 ran, 3 failed, 1 not reached");
        CHECK(!d.allRun());
    }
    // a clean run says so
    {
        const Script s = parseInputScript("1 click \"A\"\n2 state v == 2.0\n");
        FakeTarget t;
        t.states["v"] = "2";
        Driver d(s, t);
        said.clear();
        d.tick(1, say); d.tick(2, say);
        CHECK(d.allRun());
        CHECK(d.report().failed == 0);
        CHECK(d.summaryLine() == "input script: 2 steps, 2 ran, 0 failed, 0 not reached");
    }
    // a choose spans TWO ticks: the key is opened on the step's tick and the row picked on the next (the popup's rows exist only after a
    // frame), the steps after it wait for the pick; an open that is refused fails the step at once and never picks
    {
        const Script s = parseInputScript("3 choose \"Mode\" \"AM\"\nstate m == AM\n+2 choose \"Nope\" \"X\"\n+1 click \"B\"\n");
        CHECK(s.errors.empty());
        FakeTarget t;
        t.refuseLabel = "Nope";
        t.states["m"] = "AM";
        Driver d(s, t);
        said.clear();
        d.tick(1, say); d.tick(2, say);
        CHECK(t.calls.empty());
        d.tick(3, say);
        CHECK(t.calls == std::vector<std::string>{"open Mode"});          // opened, not yet picked; the state step waits
        CHECK(d.report().ran == 0);
        d.tick(4, say);
        CHECK((t.calls == std::vector<std::string>{"open Mode", "pick AM", "state m"}));
        CHECK(d.report().ran == 2 && d.report().failed == 0);
        CHECK(anySaid("line 1: choose \"Mode\" \"AM\" -> ok"));
        d.tick(5, say);
        CHECK(t.calls.size() == 4 && t.calls[3] == "open Nope");            // refused: failed at once
        CHECK(d.report().ran == 3 && d.report().failed == 1);
        CHECK(anySaid("line 3") && anySaid("no control labelled \"Nope\""));
        d.tick(6, say);
        CHECK(t.calls.size() == 5 && t.calls[4] == "click B");             // no pick after a refused open; the next step runs on its tick
        CHECK(d.allRun() == false);
        CHECK(d.summaryLine() == "input script: 4 steps, 4 ran, 1 failed, 0 not reached");
    }
    // a pick that is refused (no such row) fails the choose step with the row's name
    {
        const Script s = parseInputScript("1 choose \"Mode\" \"Nope\"\n");
        FakeTarget t;
        t.refuseLabel = "Nope";
        Driver d(s, t);
        said.clear();
        d.tick(1, say); d.tick(2, say);
        CHECK((t.calls == std::vector<std::string>{"open Mode", "pick Nope"}));
        CHECK(d.report().failed == 1);
        CHECK(anySaid("after opening \"Mode\": no control labelled \"Nope\""));
    }
    // a single character is a key of its own (typed into whatever has the focus); two are not
    {
        const Script s = parseInputScript("1 key a\nkey 7\nkey ab\nkey f\n");
        CHECK(s.errors.size() == 1);
        CHECK(!s.errors.empty() && s.errors[0].find("line 3") != std::string::npos && s.errors[0].find("unknown key") != std::string::npos);
    }
    // Ctrl with a letter or a digit is a chord (Ctrl+0 is the interface size's Auto, docs 6.61); Ctrl with a symbol or a second character is not
    {
        const Script s = parseInputScript("1 key ctrl+a\nkey ctrl+0\nkey ctrl+9\nkey ctrl+!\nkey ctrl+ab\n");
        CHECK(s.errors.size() == 2);
        CHECK(s.errors.size() == 2 && s.errors[0].find("line 4") != std::string::npos && s.errors[1].find("line 5") != std::string::npos);
        CHECK(s.steps.size() == 3);
    }
    // CHORDS (screen stage S5): ctrl+ shift+ alt+, in that order, once each, in front of a named key, a function key or one letter or digit: ctrl+pagedown, alt+pageup, ctrl+8, ctrl+shift+f5; refused: a chord with
    // no key, the modifiers twice or out of order, an unknown name, a chord of a symbol
    {
        const Script s = parseInputScript("1 key alt+pageup\nkey alt+pagedown\nkey ctrl+8\nkey ctrl+pagedown\nkey ctrl+shift+f5\nkey pageup\nkey home\nkey end\nkey shift+alt+x\n");
        CHECK(s.errors.empty() && s.steps.size() == 9);
        CHECK(s.steps.size() == 9 && s.steps[0].text == "alt+pageup" && s.steps[3].text == "ctrl+pagedown" && s.steps[4].text == "ctrl+shift+f5" && s.steps[5].text == "pageup");
        const Script bad = parseInputScript("1 key alt+\nkey ctrl+ctrl+a\nkey alt+ctrl+a\nkey ctrl+nokey\nkey alt+!\nkey ctrl+f13\nkey shift+ctrl+a\n");
        CHECK(bad.errors.size() == 7 && bad.steps.empty());
    }
    // THE `radio range` VERB (screen stage S5): a made-up tuning range for Link tuning, radio 1 to 8, low then high in Hz, low not above high; 0 0 takes it away
    {
        const Script s = parseInputScript("1 radio range 2 88000000 108000000\nradio range 1 0 0\nradio range 0 1 2\nradio range 2 5\nradio range 2 9 3\nradio range 2 x y\nradio range 9 1 2\nradio range 3 1 2 3\n");
        CHECK(s.steps.size() == 2 && s.errors.size() == 6);
        CHECK(s.steps.size() == 2 && s.steps[0].verb == Verb::Radio && s.steps[0].label == "range" && s.steps[0].radioNo == 2 && s.steps[0].number == 88000000 && s.steps[0].number2 == 108000000);
        struct RangeTarget : FakeTarget {
            std::string radio(const std::string& what, int n, double lo, double hi) override { calls.push_back("radio " + what + " " + std::to_string(n) + " " + std::to_string(static_cast<long long>(lo)) + " " + std::to_string(static_cast<long long>(hi))); return std::string(); }
        };
        RangeTarget rt;
        Driver d(s, rt);
        said.clear();
        d.tick(1, say);
        CHECK((rt.calls == std::vector<std::string>{"radio range 2 88000000 108000000", "radio range 1 0 0"}) && d.allRun());
    }
    // THE `radio` VERB (docs/multi-radio-records/screen-s4.md): add [<n> [<toneHz>]] and remove <n>; the target is asked with the verb, the radio's number and the tone; a target that cannot
    // do it (a fake of the older interface) says so and the step fails; remove needs a number from 2; add takes a number 0..8 and a tone; anything else is a line of its own in the errors
    {
        const Script s = parseInputScript("1 radio add\nradio add 3\nradio add 4 60000\nradio remove 4\nradio remove 1\nradio add 9\nradio drop 2\nradio remove 3 5\nradio add 2.5\n\n");
        CHECK(s.steps.size() == 4);
        CHECK(s.errors.size() == 5);
        CHECK(s.errors.size() == 5 && s.errors[0].find("line 5") != std::string::npos && s.errors[1].find("line 6") != std::string::npos && s.errors[2].find("line 7") != std::string::npos
              && s.errors[3].find("line 8") != std::string::npos && s.errors[4].find("line 9") != std::string::npos);
        if (s.steps.size() == 4) {
            CHECK(s.steps[0].verb == Verb::Radio && s.steps[0].label == "add" && s.steps[0].radioNo == 0 && s.steps[0].number == 0);
            CHECK(s.steps[1].radioNo == 3 && s.steps[2].radioNo == 4 && s.steps[2].number == 60000);
            CHECK(s.steps[3].label == "remove" && s.steps[3].radioNo == 4);
        }
        struct RadioTarget : FakeTarget {
            std::string radio(const std::string& what, int n, double tone, double hz2) override { calls.push_back("radio " + what + " " + std::to_string(n) + " " + std::to_string(static_cast<int>(tone)) + (hz2 != 0.0 ? " " + std::to_string(static_cast<long long>(hz2)) : std::string())); return std::string(); }
        };
        const Script ok = parseInputScript("1 radio add 2 60000\nradio remove 2\n\n");
        RadioTarget rt;
        Driver d(ok, rt);
        said.clear();
        d.tick(1, say);
        CHECK((rt.calls == std::vector<std::string>{"radio add 2 60000", "radio remove 2 0"}));
        CHECK(d.allRun());
        FakeTarget plain;   // the older interface: no radio hook
        Driver d2(ok, plain);
        said.clear();
        d2.tick(1, say);
        CHECK(d2.report().failed == 2 && anySaid("cannot add or remove radios"));
    }
    // a word comparison is exact, and != works on words and numbers
    {
        const Script s = parseInputScript("1 state w == abc\nstate w != abd\nstate n != 4\nstate n == 3.0\nstate w == ABC\n");
        FakeTarget t;
        t.states["w"] = "abc";
        t.states["n"] = "3";
        Driver d(s, t);
        said.clear();
        d.tick(1, say);
        CHECK(d.report().failed == 1);   // only "ABC"
    }

    return testSummary("test_slint_host_input");
}
