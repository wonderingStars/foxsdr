// input_script.cpp - the parser and the driver (see the header).
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "input_script.hpp"

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <sstream>

namespace foxsdr::slintdrive {

namespace {

constexpr long kTickMs = 33;

// A cursor over one line's text.
struct Cursor {
    const std::string& s;
    std::size_t at = 0;
    void skipSpaces() { while (at < s.size() && (s[at] == ' ' || s[at] == '\t')) { ++at; } }
    bool atEnd() {
        skipSpaces();
        return at >= s.size() || s[at] == '#';
    }
    // A bare word (up to a space, a '#' or the end).
    std::string word() {
        skipSpaces();
        std::size_t b = at;
        while (at < s.size() && s[at] != ' ' && s[at] != '\t' && s[at] != '#') { ++at; }
        return s.substr(b, at - b);
    }
    // A "quoted" string with \" and \\; false when the next thing is not a quote or the quote never closes.
    bool quoted(std::string& out) {
        skipSpaces();
        if (at >= s.size() || s[at] != '"') { return false; }
        ++at;
        out.clear();
        while (at < s.size()) {
            const char c = s[at++];
            if (c == '\\' && at < s.size()) { out += s[at++]; continue; }
            if (c == '"') {
                // "Close"#2 - the index suffix rides on the closing quote (a '#' followed by digits, no space); a '#' followed by
                // anything else after the quote is a comment, as everywhere
                if (at + 1 < s.size() && s[at] == '#' && std::isdigit(static_cast<unsigned char>(s[at + 1]))) {
                    out += s[at++];
                    while (at < s.size() && std::isdigit(static_cast<unsigned char>(s[at]))) { out += s[at++]; }
                }
                return true;
            }
            out += c;
        }
        return false;
    }
    // A word or a "quoted" string.
    bool wordOrQuoted(std::string& out) {
        skipSpaces();
        if (at < s.size() && s[at] == '"') { return quoted(out); }
        out = word();
        return !out.empty();
    }
};

bool isNumber(const std::string& t, double& v) {
    if (t.empty()) { return false; }
    char* end = nullptr;
    v = std::strtod(t.c_str(), &end);
    return end != nullptr && *end == '\0' && std::isfinite(v);
}

bool knownKey(const std::string& k) {
    static const char* const names[] = {"enter", "return", "escape", "tab", "backspace", "delete", "space", "up", "down", "left", "right"};
    for (const char* n : names) { if (k == n) { return true; } }
    if (k.size() >= 2 && k[0] == 'f') {
        const int n = std::atoi(k.c_str() + 1);
        bool digits = true;
        for (std::size_t i = 1; i < k.size(); ++i) { if (!std::isdigit(static_cast<unsigned char>(k[i]))) { digits = false; } }
        return digits && n >= 1 && n <= 12;
    }
    if (k.size() == 1 && std::isprint(static_cast<unsigned char>(k[0])) && k[0] != ' ') { return true; }   // one character, typed as itself
    return k.size() == 6 && k.rfind("ctrl+", 0) == 0 && std::isalnum(static_cast<unsigned char>(k[5]));   // ctrl+<letter> or ctrl+<digit> (Ctrl+0, docs 6.61)
}

}  // namespace

const char* verbName(Verb v) {
    switch (v) {
        case Verb::Click: return "click";
        case Verb::Press: return "press";
        case Verb::Type: return "type";
        case Verb::Key: return "key";
        case Verb::Slider: return "slider";
        case Verb::Scroll: return "scroll";
        case Verb::Choose: return "choose";
        case Verb::Expect: return "expect";
        case Verb::State: return "state";
        case Verb::Shot: return "shot";
        case Verb::Dump: return "dump";
        case Verb::Radio: return "radio";
    }
    return "?";
}

long ticksForMs(long ms) {
    if (ms <= 0) { return 1; }
    return (ms + kTickMs - 1) / kTickMs;
}

bool splitLabelIndex(std::string& label, int& index) {
    index = 1;
    const std::size_t hash = label.rfind('#');
    if (hash == std::string::npos || hash == 0 || hash + 1 >= label.size()) { return true; }
    if (label[hash - 1] == ' ') { return true; }   // "Channel #12": the '#' is part of the words
    for (std::size_t i = hash + 1; i < label.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(label[i]))) { return true; }
    }
    const int n = std::atoi(label.c_str() + hash + 1);
    if (n < 1) { return false; }
    index = n;
    label.erase(hash);
    return true;
}

Script parseInputScript(const std::string& text) {
    Script out;
    std::istringstream in(text);
    std::string line;
    int lineNo = 0;
    long cursorTick = 1;
    bool anyStep = false;
    while (std::getline(in, line)) {
        ++lineNo;
        if (!line.empty() && line.back() == '\r') { line.pop_back(); }
        Cursor c{line};
        if (c.atEnd()) { continue; }
        const auto bad = [&](const std::string& why) { out.errors.push_back("line " + std::to_string(lineNo) + ": " + why); };

        // the tick
        std::string first = c.word();
        long tick = cursorTick;
        bool consumedVerb = false;
        if (!first.empty() && (std::isdigit(static_cast<unsigned char>(first[0])) || first[0] == '+')) {
            const bool relative = first[0] == '+';
            const std::string digits = relative ? first.substr(1) : first;
            bool ok = !digits.empty();
            for (char ch : digits) { if (!std::isdigit(static_cast<unsigned char>(ch))) { ok = false; } }
            if (!ok) { bad("a tick is a number or +number: \"" + first + "\""); continue; }
            const long n = std::atol(digits.c_str());
            tick = relative ? cursorTick + n : n;
            if (!relative && (tick < cursorTick || tick < 1)) { bad("tick " + std::to_string(tick) + " goes backwards (the cursor is at " + std::to_string(cursorTick) + ")"); continue; }
            if (!relative && !anyStep && tick < 1) { bad("the first tick is 1"); continue; }
        } else {
            consumedVerb = true;
        }
        const std::string verb = consumedVerb ? first : c.word();
        if (verb.empty()) { bad("no verb"); continue; }

        if (verb == "wait") {
            const std::string ms = c.word();
            double v = 0;
            if (!isNumber(ms, v) || v < 1) { bad("wait takes milliseconds, at least 1: \"" + ms + "\""); continue; }
            if (!c.atEnd()) { bad("trailing words after the wait"); continue; }
            cursorTick = tick + ticksForMs(static_cast<long>(v));
            continue;
        }

        Step st;
        st.line = lineNo;
        st.tick = tick;
        bool ok = true;
        if (verb == "click" || verb == "press") {
            st.verb = verb == "click" ? Verb::Click : Verb::Press;
            if (!c.quoted(st.label) || st.label.empty()) { bad(verb + " takes a quoted label"); continue; }
        } else if (verb == "type") {
            st.verb = Verb::Type;
            if (!c.quoted(st.label) || st.label.empty()) { bad("type takes a quoted label"); continue; }
            if (!c.quoted(st.text)) { bad("type takes a quoted text after the label"); continue; }
        } else if (verb == "key") {
            st.verb = Verb::Key;
            st.text = c.word();
            if (!knownKey(st.text)) { bad("unknown key \"" + st.text + "\" (enter escape tab backspace delete space up down left right f1..f12 ctrl+<letter|digit> or one character)"); continue; }
        } else if (verb == "slider") {
            st.verb = Verb::Slider;
            if (!c.quoted(st.label) || st.label.empty()) { bad("slider takes a quoted label"); continue; }
            double v = 0;
            const std::string num = c.word();
            if (!isNumber(num, v) || v < 0.0 || v > 1.0) { bad("slider takes a fraction 0..1: \"" + num + "\""); continue; }
            st.number = v;
        } else if (verb == "scroll") {
            st.verb = Verb::Scroll;
            if (!c.quoted(st.label) || st.label.empty()) { bad("scroll takes a quoted label"); continue; }
            double v = 0;
            const std::string num = c.word();
            if (!isNumber(num, v) || v == 0.0 || std::fabs(v) > 100000.0) { bad("scroll takes a number of pixels, not 0: \"" + num + "\""); continue; }
            st.number = v;
        } else if (verb == "choose") {
            st.verb = Verb::Choose;
            if (!c.quoted(st.label) || st.label.empty()) { bad("choose takes a quoted label"); continue; }
            if (!c.quoted(st.text) || st.text.empty()) { bad("choose takes a quoted item after the label"); continue; }
        } else if (verb == "expect") {
            st.verb = Verb::Expect;
            if (!c.quoted(st.label) || st.label.empty()) { bad("expect takes a quoted label"); continue; }
            st.what = c.word();
            if (st.what != "value" && st.what != "label" && st.what != "checked" && st.what != "enabled") { bad("expect reads value, label, checked or enabled, not \"" + st.what + "\""); continue; }
            st.compare = c.word();
            if (st.compare != "equals" && st.compare != "contains") { bad("expect compares with equals or contains, not \"" + st.compare + "\""); continue; }
            if (!c.quoted(st.text)) { bad("expect takes a quoted text last"); continue; }
        } else if (verb == "state") {
            st.verb = Verb::State;
            st.label = c.word();
            if (st.label.empty()) { bad("state takes a name"); continue; }
            st.what = c.word();
            static const char* const cmps[] = {"==", "!=", "<", ">", "<=", ">=", "contains"};
            bool known = false;
            for (const char* k : cmps) { if (st.what == k) { known = true; } }
            if (!known) { bad("state compares with == != < > <= >= or contains, not \"" + st.what + "\""); continue; }
            if (!c.wordOrQuoted(st.text)) { bad("state takes a value last"); continue; }
        } else if (verb == "shot") {
            st.verb = Verb::Shot;
            if (!c.quoted(st.label) || st.label.empty()) { bad("shot takes a quoted file name"); continue; }
        } else if (verb == "dump") {
            st.verb = Verb::Dump;
        } else if (verb == "radio") {
            st.verb = Verb::Radio;
            st.label = c.word();
            if (st.label != "add" && st.label != "remove") { bad("radio takes add or remove, not \"" + st.label + "\""); continue; }
            double v = 0;
            std::string num = c.word();
            if (!num.empty()) {
                if (!isNumber(num, v) || v < 0.0 || v > 8.0 || v != std::floor(v)) { bad("radio takes a radio number 0..8: \"" + num + "\""); continue; }
                st.radioNo = static_cast<int>(v);
            }
            if (st.label == "remove" && st.radioNo < 2) { bad("radio remove takes the number of a radio from 2 to 8"); continue; }
            num = c.word();
            if (!num.empty()) {
                if (st.label == "remove" || !isNumber(num, v) || std::fabs(v) > 1.0e7) { bad("radio add takes a tone in Hz last: \"" + num + "\""); continue; }
                st.number = v;
            }
        } else {
            bad("unknown verb \"" + verb + "\"");
            continue;
        }
        if (ok && !c.atEnd()) { bad("trailing words after the " + verb); continue; }
        int index = 1;
        std::string probe = st.label;
        if ((st.verb == Verb::Click || st.verb == Verb::Press || st.verb == Verb::Type || st.verb == Verb::Slider || st.verb == Verb::Choose || st.verb == Verb::Expect) && !splitLabelIndex(probe, index)) {
            bad("a label's #index counts from 1: \"" + st.label + "\"");
            continue;
        }
        cursorTick = tick;
        anyStep = true;
        out.steps.push_back(std::move(st));
    }
    return out;
}

bool compareValues(const std::string& got, const std::string& cmp, const std::string& want) {
    if (cmp == "contains") { return got.find(want) != std::string::npos; }
    double a = 0, b = 0;
    if (isNumber(got, a) && isNumber(want, b)) {
        const double tol = 1e-6 * std::fmax(1.0, std::fmax(std::fabs(a), std::fabs(b)));
        if (cmp == "==" || cmp == "equals") { return std::fabs(a - b) <= tol; }
        if (cmp == "!=") { return std::fabs(a - b) > tol; }
        if (cmp == "<") { return a < b - tol; }
        if (cmp == ">") { return a > b + tol; }
        if (cmp == "<=") { return a <= b + tol; }
        if (cmp == ">=") { return a >= b - tol; }
        return false;
    }
    if (cmp == "==" || cmp == "equals") { return got == want; }
    if (cmp == "!=") { return got != want; }
    if (cmp == "<") { return got < want; }
    if (cmp == ">") { return got > want; }
    if (cmp == "<=") { return got <= want; }
    if (cmp == ">=") { return got >= want; }
    return false;
}

Driver::Driver(Script script, DriveTarget& target) : script_(std::move(script)), target_(target) {}

void Driver::tick(long tick, const std::function<void(const std::string&)>& say) {
    lastTick_ = std::max(lastTick_, tick);
    while (next_ < script_.steps.size() && script_.steps[next_].tick <= tick) {
        const Step& st = script_.steps[next_];
        std::string head = "input script line " + std::to_string(st.line) + ": " + verbName(st.verb);
        std::string why;
        std::string got;
        // A choose takes two ticks: the key opened now, the row picked on the next tick (the popup's rows are instantiated by the frame in
        // between). The steps after it wait: the loop stops here until the next tick.
        if (st.verb == Verb::Choose && !opened_) {
            why = target_.open(st.label);
            if (why.empty()) { opened_ = true; return; }
        }
        ++next_;
        switch (st.verb) {
            case Verb::Click: head += " \"" + st.label + "\""; why = target_.click(st.label); break;
            case Verb::Press: head += " \"" + st.label + "\""; why = target_.press(st.label); break;
            case Verb::Type: head += " \"" + st.label + "\" \"" + st.text + "\""; why = target_.type(st.label, st.text); break;
            case Verb::Key: head += " " + st.text; why = target_.key(st.text); break;
            case Verb::Slider: head += " \"" + st.label + "\" " + std::to_string(st.number); why = target_.slider(st.label, st.number); break;
            case Verb::Scroll: head += " \"" + st.label + "\" " + std::to_string(st.number); why = target_.scroll(st.label, st.number); break;
            case Verb::Choose:
                head += " \"" + st.label + "\" \"" + st.text + "\"";
                if (opened_) {
                    opened_ = false;
                    why = target_.pick(st.text);
                    if (!why.empty()) { why = "after opening \"" + st.label + "\": " + why; }
                }
                break;
            case Verb::Expect:
                head += " \"" + st.label + "\" " + st.what + " " + st.compare + " \"" + st.text + "\"";
                why = target_.read(st.label, st.what, got);
                if (why.empty() && !compareValues(got, st.compare, st.text)) { why = "it reads \"" + got + "\""; }
                break;
            case Verb::State:
                head += " " + st.label + " " + st.what + " " + st.text;
                why = target_.state(st.label, got);
                if (why.empty() && !compareValues(got, st.what, st.text)) { why = "it is " + got; }
                break;
            case Verb::Shot: head += " \"" + st.label + "\""; why = target_.shot(st.label); break;
            case Verb::Dump: why = target_.dumpControls(); break;
            case Verb::Radio:
                head += " " + st.label + " " + std::to_string(st.radioNo);
                why = target_.radio(st.label, st.radioNo, st.number);
                break;
        }
        if (why.empty()) {
            say(head + " -> ok");
        } else {
            ++failed_;
            say(head + " -> FAILED: " + why);
        }
    }
}

DriveReport Driver::report() const {
    DriveReport r;
    r.steps = static_cast<int>(script_.steps.size());
    r.ran = static_cast<int>(next_);
    r.failed = failed_;
    r.notReached = r.steps - r.ran;
    return r;
}

bool Driver::allRun() const {
    const DriveReport r = report();
    return r.ran == r.steps && r.failed == 0;
}

std::string Driver::summaryLine() const {
    const DriveReport r = report();
    return "input script: " + std::to_string(r.steps) + " steps, " + std::to_string(r.ran) + " ran, " + std::to_string(r.failed) + " failed, " +
           std::to_string(r.notReached) + " not reached";
}

}  // namespace foxsdr::slintdrive
