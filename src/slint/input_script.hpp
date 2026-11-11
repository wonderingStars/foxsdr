// input_script.hpp - the Slint window's scripted input: a text file of steps run against the live window inside the process
// (docs/engine-stage4.md 6.47). The classic window's FOXSDR_INPUT_SCRIPT (gui/input_script.hpp) feeds pointer positions into
// ImGui's queue; this one aims at a CONTROL BY ITS ACCESSIBLE LABEL - the label every Key, Checkbox, Field, Slider, DropKey and tab
// of ui/*.slint carries for the Windows accessibility tree (docs 6.46 drove the window through that tree from outside) - and
// reads the effect back from the engine through the table, so a step that "saved but did not apply" fails on the read-back.
//
// ONLY IN A BOUNDED --frames RUN, off unless FOXSDR_INPUT_SCRIPT names a file (main.cpp), like every other test hook of this
// window. The window runs on the hidden desktop in a test; nothing of the operating system's pointer or keyboard is involved,
// and nothing outside the process reaches a scripted control.
//
// THE FORMAT, one step per line; '#' starts a comment (not inside a quoted string); a quoted string is "..." with \" and \\.
//
//   [<tick> | +<n>] <verb> <args>
//
//   A step runs at its tick (the Nth 33 ms tick of the run, from 1). <tick> is absolute, +<n> is n after the previous
//   step's tick, neither = the previous step's tick (the first step: 1). A tick may not go backwards.
//
//   click "<label>"                     the control's default action (a key, a checkbox, a radio key, a tab, a list row)
//   press "<label>"                     a pointer click at the control's centre (the real gesture; a control outside the window is refused)
//   type "<label>" "<text>"             a field: pointer-focused, its text selected, the text typed (no Enter: `key enter` commits)
//   key <name>                          a key to the focused control: enter, escape, tab, backspace, delete, space, up, down, left,
//                                       right, pageup, pagedown, home, end, f1..f12, ctrl+<letter|digit>, any chord of ctrl+ shift+ alt+
//                                       (in that order) in front of a named key or one character (ctrl+pagedown, alt+pageup, ctrl+8),
//                                       or ONE printable character typed as itself (a letter into whatever has the focus - a
//                                       popup's filter field, say - without a pointer)
//   slider "<label>" <0..1>             a slider: a pointer press at that fraction of its track
//   scroll "<label>" <pixels>           a wheel scroll at the control's centre: positive shows what is BELOW (the content moves up),
//                                       negative what is above - a section body taller than the window is reached this way
//   choose "<label>" "<item>"           a dropdown: the key is pressed on the step's tick and the item row with that label on the NEXT
//                                       tick (a popup's rows exist only after a frame); the steps after it wait for the pick
//   wait <ms>                           not a step: the next step is ceil(ms / 33) ticks later than the step before it
//   expect "<label>" <what> <cmp> "<text>"   <what>: value | label | checked | enabled (checked and enabled read "true" / "false");
//                                       <cmp>: equals | contains
//   state <name> <cmp> <value>          the engine's read-back (main.cpp names the states: tunedHz, mode, running, count.bookmarks,
//                                       fact.<key>, list.<id>.names ...); <cmp>: == != < > <= >= contains; <value> a word or "..."
//                                       - numbers compare as numbers, anything else as the exact text
//   shot "<file>"                       a PNG of the window, beside the settings file's screenshots
//   dump                                every control with an accessible label, one per line on stderr: the author's map
//   radio add [<n> [<toneHz>]]          TEST ONLY: opens a GENERATOR radio through the engine's real RADIO_ADD (number n, 0 or none = the lowest free; the tone in Hz from its
//                                       centre, none = 25 kHz times its number), started running (docs/multi-radio-records/screen-s4.md); the effect is read back with `state`
//   radio remove <n>                    TEST ONLY: RADIO_REMOVE of radio n (2 or more), through the engine
//   radio range <n> <loHz> <hiHz>       TEST ONLY: radio n (1 to 8) is given a tuning range for LINK TUNING (docs/multi-radio-records/screen-s5.md): the follower's request is clamped to it, as a device
//                                       that clamps to its edge would. The product cannot ask a device for its range (the API has none), so a limit can be shown only by a seam; "radio range <n> 0 0" removes it
//
//   "<label>"#<n> picks the n-th control (from 1) when several carry the label (a sheet's Close beside the title bar's).
//
// The run's verdict: every step ran and none failed = exit 0; a refused or failed step, or a step whose tick never came, = exit 3
// (printed with the bounded run's exit line); a script that does not parse = exit 2 before the window opens, every bad line named.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace foxsdr::slintdrive {

enum class Verb { Click, Press, Type, Key, Slider, Scroll, Drag, Choose, Expect, State, Shot, Dump, Radio };

const char* verbName(Verb v);

struct Step {
    int line = 0;         // the script line, for the log
    long tick = 0;        // the tick it runs at (from 1)
    Verb verb = Verb::Click;
    std::string label;    // the control's label (click, press, type, slider, choose, expect), the state's name (state), the file (shot)
    std::string text;     // type: the text; choose: the item; key: the key name; expect / state: the wanted value
    std::string what;     // expect: value | label | checked | enabled; state: the comparison
    std::string compare;  // expect: equals | contains
    double number = 0;    // slider: the fraction; scroll: the pixels; radio: the tone in Hz (add), the low end in Hz (range)
    double number2 = 0;   // radio range: the high end in Hz; drag: the pointer's offset down in pixels (number is the offset right)
    int radioNo = 0;      // radio: the radio's number
};

struct Script {
    std::vector<Step> steps;
    std::vector<std::string> errors;   // "line N: <why>" - a script with any error does not run
};

Script parseInputScript(const std::string& text);

// "Close#2" -> "Close", 2; "Close" -> "Close", 1; "Channel #12" stays whole (the suffix is '#' then digits at the very end,
// after a non-space). False for an index below 1.
bool splitLabelIndex(std::string& label, int& index);

// Ticks a `wait <ms>` stands for: ceil(ms / 33), at least 1.
long ticksForMs(long ms);

// What the driver drives. Every call answers "" when it did what was asked, else one sentence saying why not (the step fails
// with that sentence in the log). `read` and `state` put the value in `out`.
class DriveTarget {
public:
    virtual ~DriveTarget() = default;
    virtual std::string click(const std::string& label) = 0;
    virtual std::string press(const std::string& label) = 0;
    virtual std::string type(const std::string& label, const std::string& text) = 0;
    virtual std::string key(const std::string& name) = 0;
    virtual std::string slider(const std::string& label, double frac) = 0;
    virtual std::string scroll(const std::string& label, double pixels) = 0;
    // A pointer drag from the control's centre by (dx, dy) logical pixels: press, a few moves, release. A target that cannot drag says so.
    virtual std::string drag(const std::string& label, double dx, double dy) { (void)label; (void)dx; (void)dy; return "this target cannot drag"; }
    virtual std::string open(const std::string& label) = 0;   // a dropdown key: its default action opens the popup
    virtual std::string pick(const std::string& item) = 0;    // the popup row with that label, the tick after
    virtual std::string read(const std::string& label, const std::string& what, std::string& out) = 0;
    virtual std::string state(const std::string& name, std::string& out) = 0;
    virtual std::string shot(const std::string& file) = 0;
    virtual std::string dumpControls() = 0;   // the `dump` verb (named apart from nlohmann's dump() for tests/test_json_dump_policy)
    // The `radio` verb (a bounded run's test seam): what is "add", "remove" or "range", n the radio's number (0 = the lowest free), toneHz a generator radio's tone (0 = the engine's default) or, for a range, its low end, hz2 its high end.
    virtual std::string radio(const std::string& what, int n, double toneHz, double hz2 = 0) { (void)what; (void)n; (void)toneHz; (void)hz2; return "this target cannot add or remove radios"; }
};

struct DriveReport {
    int steps = 0;
    int ran = 0;
    int failed = 0;
    int notReached = 0;   // steps whose tick is past the last tick the driver saw
};

// Compares as numbers when both parse whole as numbers (== within a millionth of the larger magnitude), else as text.
// cmp: == != < > <= >= contains equals (equals is ==).
bool compareValues(const std::string& got, const std::string& cmp, const std::string& want);

class Driver {
public:
    Driver(Script script, DriveTarget& target);
    // Runs, in order, every step whose tick is at or before `tick` and has not run; `say` gets one line per step
    // ("input script line 3: click "A" -> ok" or "... -> FAILED: <why>").
    void tick(long tick, const std::function<void(const std::string&)>& say);
    DriveReport report() const;
    bool allRun() const;   // every step ran and none failed
    // "input script: 9 steps, 8 ran, 3 failed, 1 not reached"
    std::string summaryLine() const;

private:
    Script script_;
    DriveTarget& target_;
    std::size_t next_ = 0;
    bool opened_ = false;   // the choose at next_ has opened its key; the pick is due on the next tick
    long lastTick_ = 0;
    int failed_ = 0;
};

}  // namespace foxsdr::slintdrive
