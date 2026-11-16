// Screen lane S4b's pure rules (src/slint/s4b_logic.hpp; docs/multi-radio-records/screen-s4b.md): the list of locked-out frequencies typed in MHz, the engine's text read back, the lockout lines.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <string>

#include <nlohmann/json.hpp>

#include "foxsdr_api.h"
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
    // the macro recorder
    {
        CHECK(recordableName(FOXAPI_OP_SET_FREQUENCY) == "SET_FREQUENCY" && recordableName(FOXAPI_OP_SET_MODE) == "SET_MODE" && recordableName(FOXAPI_OP_SET_SQUELCH) == "SET_SQUELCH");
        CHECK(recordableName(FOXAPI_OP_SET_VOLUME) == "SET_VOLUME" && recordableName(FOXAPI_OP_SET_MUTED) == "SET_MUTED" && recordableName(FOXAPI_OP_SET_BANDWIDTH) == "SET_BANDWIDTH" && recordableName(FOXAPI_OP_SET_CENTRE) == "SET_CENTRE");
        CHECK(recordableName(0x8A20u).empty() && recordableName(0x8A00u).empty() && recordableName(FOXAPI_OP_RUN).empty() && recordableName(0).empty());
        MacroRecorder r;
        const double n1[4] = {7.1e6, 0, 0, 0}, nn[4] = {0, 0, 0, 0};
        const long long i0[2] = {0, 0}, i7[2] = {7, 0};
        CHECK(!r.note(FOXAPI_OP_SET_FREQUENCY, 0, n1, i0, 0.0) && r.size() == 0);                  // off: nothing is kept
        r.start();
        CHECK(r.active() && r.note(FOXAPI_OP_SET_FREQUENCY, 0, n1, i0, 1000.0) && r.note(FOXAPI_OP_SET_MODE, 1, nn, i7, 1500.0) && r.size() == 2);
        CHECK(!r.note(0x8A20u, 0, n1, i0, 1600.0) && !r.note(FOXAPI_OP_RUN, 0, n1, i0, 1600.0) && r.size() == 2);   // not a recordable op
        CHECK(!r.note(FOXAPI_OP_SET_FREQUENCY, 2, n1, i0, 1700.0) && r.skippedOtherRadio() == 1 && r.size() == 2);   // another radio
        CHECK(r.note(FOXAPI_OP_SET_VOLUME, 0, n1, i0, 200000.0) && r.size() == 3);
        r.stop();
        CHECK(!r.active() && !r.note(FOXAPI_OP_SET_VOLUME, 0, n1, i0, 300000.0) && r.size() == 3);
        const nlohmann::json j = nlohmann::json::parse(r.saveJson("Test"));
        CHECK(j["action"] == "save" && j["name"] == "Test" && j["steps"].size() == 3);
        CHECK(j["steps"][0]["cmd"] == "SET_FREQUENCY" && j["steps"][0]["num"][0] == 7.1e6 && j["steps"][0]["delayMs"] == 500);
        CHECK(j["steps"][1]["cmd"] == "SET_MODE" && j["steps"][1]["ival"][0] == 7 && j["steps"][1]["delayMs"] == 60000);   // a long pause is cut to a minute
        CHECK(j["steps"][2]["delayMs"] == 0);
        CHECK(MacroRecorder().saveJson("x").empty());
        // bounded
        MacroRecorder b;
        b.start();
        for (int k = 0; k < 800; ++k) { b.note(FOXAPI_OP_SET_VOLUME, 0, n1, i0, 10.0 * k); }
        CHECK(b.size() == MacroRecorder::kMaxSteps);
        r.clear();
        CHECK(!r.active() && r.size() == 0);
    }
    return testSummary("test_slint_s4b_host");
}
