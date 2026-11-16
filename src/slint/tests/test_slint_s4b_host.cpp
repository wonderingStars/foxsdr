// Screen lane S4b's pure rules (src/slint/s4b_logic.hpp; docs/multi-radio-records/screen-s4b.md): the list of locked-out frequencies typed in MHz, the engine's text read back, the lockout lines.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <string>

#include "s4b_logic.hpp"
#include "test_check.hpp"

using namespace foxsdr::slints4b;

int main() {
    // typed MHz to the engine's Hz
    {
        const HzListResult r = parseMhzList("100.2, 118.1;145.5\n433.92");
        CHECK(r.problem.empty() && r.hz.size() == 4);
        CHECK(r.hz.size() == 4 && r.hz[0] == 100200000.0 && r.hz[1] == 118100000.0 && r.hz[2] == 145500000.0 && r.hz[3] == 433920000.0);
        CHECK(hzListToWire(r.hz) == "100200000;118100000;145500000;433920000");
    }
    // empty clears; a repeat is dropped; spaces and stray separators are fine
    CHECK(parseMhzList("").problem.empty() && parseMhzList("").hz.empty());
    CHECK(parseMhzList(" , ;; ").problem.empty() && parseMhzList(" , ;; ").hz.empty());
    CHECK(parseMhzList("100.2 100.2000, 100.2").hz.size() == 1);
    // refused: not a number, zero, negative, too big, trailing letters, a hostile length
    CHECK(!parseMhzList("abc").problem.empty() && parseMhzList("abc").hz.empty());
    CHECK(!parseMhzList("0").problem.empty() && !parseMhzList("-5").problem.empty() && !parseMhzList("100001").problem.empty());
    CHECK(!parseMhzList("100.2MHz").problem.empty() && !parseMhzList("nan").problem.empty() && !parseMhzList("inf").problem.empty());
    CHECK(parseMhzList("100000").problem.empty() && parseMhzList("0.001").problem.empty());
    CHECK(!parseMhzList(std::string(70000, '1')).problem.empty());
    CHECK(!parseMhzList(std::string(30, '1')).problem.empty());   // one number cannot be 30 characters
    // the count: 1000 are fine, 1001 are refused
    {
        std::string t;
        for (int i = 0; i < 1000; ++i) { t += std::to_string(1 + i) + ".5,"; }
        CHECK(parseMhzList(t).problem.empty() && parseMhzList(t).hz.size() == 1000);
        t += "5000.1";
        CHECK(!parseMhzList(t).problem.empty());
    }
    // the engine's text back as the line the person edits
    CHECK(hzListToMhzLine("100200000.000;118100000.000") == "100.2000, 118.1000");
    CHECK(hzListToMhzLine("") == "" && hzListToMhzLine(";;") == "" && hzListToMhzLine("x;0;-1") == "");
    CHECK(parseMhzList(hzListToMhzLine("100200000.000;118100000.000")).hz.size() == 2);
    // the engine's current lockouts
    {
        const std::vector<Lockout> l = parseLockouts("100200000.000@0;100300000.000@592");
        CHECK(l.size() == 2 && l[0].hz == 100200000.0 && l[0].secondsLeft == 0 && l[1].secondsLeft == 592);
        CHECK(parseLockouts("").empty() && parseLockouts("junk;@5;5@;100@x;-1@3;100@-4").empty());
        CHECK(parseLockouts("1@1;2@2;3@3", 2).size() == 2);   // bounded
        CHECK(lockoutLine(l[0]).find("100.2000 MHz") == 0 && lockoutLine(l[0]).find("until cleared") != std::string::npos);
        CHECK(lockoutLine(l[1]) == "100.3000 MHz: 9 min 52 s left");
        CHECK(lockoutLine({145500000.0, 7}) == "145.5000 MHz: 7 s left");
    }
    // the rotator's bearings
    CHECK(parseDegrees("123.4", 0, 450) == 123.4 && parseDegrees(" 90 ", 0, 450) == 90.0 && parseDegrees("12,5", 0, 450) == 12.5 && parseDegrees("0", 0, 450) == 0.0 && parseDegrees("450", 0, 450) == 450.0);
    CHECK(!parseDegrees("450.1", 0, 450) && !parseDegrees("-1", 0, 450) && !parseDegrees("", 0, 450) && !parseDegrees("abc", 0, 450) && !parseDegrees("1e2", 0, 450) && !parseDegrees("1.2.3", 0, 450) && !parseDegrees(".", 0, 450));
    CHECK(!parseDegrees("nan", 0, 450) && !parseDegrees("inf", 0, 450) && !parseDegrees("12345678901234", 0, 450) && !parseDegrees("1 2", 0, 450));
    CHECK(rotatorTurnJson(123.456, 7) == "{\"action\":\"turn\",\"az\":123.46,\"el\":7.00}" && rotatorActionJson("stop") == "{\"action\":\"stop\"}");
    return testSummary("test_slint_s4b_host");
}
