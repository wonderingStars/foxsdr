// Tests for the macro model (core/macros.hpp). The command vocabulary is the application's own: the list of names is built from
// src/core/app_commands.cpp (cmd::knownOps / cmd::opName), so the test also proves that every name there is usable as a macro command
// name, that the denied-prefix list still matches real commands, and which commands a macro may and may not name.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/macros.hpp"

#include "core/app_commands.hpp"
#include "test_check.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>

namespace {

namespace mc = cascade::core::macros;
namespace cmd = cascade::core::cmd;
namespace fs = std::filesystem;
using mc::Macro;
using mc::Step;

mc::KnownCommands& known() {
    static mc::KnownCommands k = [] {
        mc::KnownCommands v;
        std::size_t n = 0;
        const std::uint32_t* ops = cmd::knownOps(n);
        for (std::size_t i = 0; i < n; ++i) { v.add(cmd::opName(ops[i])); }
        return v;
    }();
    return k;
}

Step st(const std::string& c, std::int64_t delay = 0, const std::string& text = {}) {
    Step s;
    s.command = c;
    s.delayMs = delay;
    s.text = text;
    return s;
}

Macro mac(const std::string& name, std::vector<Step> steps, int repeat = 1) {
    Macro m;
    m.name = name;
    m.steps = std::move(steps);
    m.repeat = repeat;
    return m;
}

std::string bad(const Macro& m) { return mc::validateMacro(m, known()); }

// ---- the vocabulary ---------------------------------------------------------------------------------------------------------------------

void testVocabulary() {
    std::size_t n = 0;
    const std::uint32_t* ops = cmd::knownOps(n);
    CHECK(n > 50);
    std::map<std::string, std::uint32_t> byName;
    std::size_t allowed = 0, denied = 0, malformed = 0, duplicate = 0;
    const mc::MacroPolicy policy;
    std::vector<std::string> deniedNames;
    for (std::size_t i = 0; i < n; ++i) {
        const std::string name = cmd::opName(ops[i]);
        if (name == "?" || !mc::detail::validCommandName(name)) { ++malformed; std::printf("  malformed command name: %s\n", name.c_str()); }
        if (!byName.emplace(name, ops[i]).second) { ++duplicate; }
        if (mc::detail::denied(policy, name)) { ++denied; deniedNames.push_back(name); } else { ++allowed; }
    }
    CHECK(malformed == 0);      // every name in app_commands.cpp fits a macro's command-name rule
    CHECK(duplicate == 0);      // a name identifies one op
    std::printf("  application commands: %zu known, %zu allowed in a macro, %zu denied\n", n, allowed, denied);
    // the policy cannot rot: every denied prefix matches a real command
    for (const auto& pre : policy.deniedPrefixes) {
        bool hit = false;
        for (const auto& kv : byName) { if (kv.first.rfind(pre, 0) == 0) { hit = true; } }
        if (!hit) { std::printf("  denied prefix matches no known command: %s\n", pre.c_str()); }
        CHECK(hit);
    }
    // what a person expects to script is allowed, and what keys a transmitter or grants rights is not
    for (const char* ok : {"RUN", "SET_CENTRE", "SET_FREQUENCY", "SET_MODE", "SET_BANDWIDTH", "SET_SQUELCH", "SET_GAIN", "RECORD_IQ", "RECORD_AUDIO", "DECODER_START",
                           "DECODER_STOP", "BOOKMARK_TUNE", "SCANNER_RUN", "SELECT_SOURCE", "SET_VOLUME", "SET_MUTED"}) {
        CHECK(byName.count(ok) == 1);
        CHECK(!mc::detail::denied(policy, ok));
    }
    for (const char* no : {"TX_PTT", "TX_OPEN", "TX_SET_POWER", "TX_SET_FREQUENCY", "PLUGIN_GRANT", "STORE_INSTALL", "SERVER_CONFIG", "TELEMETRY_ENABLE",
                           "APP_TX_DATA_FLAT", "APP_TEST_HOLD_AUDIO_LOCK", "APP_PLUGIN_TX_GRANT", "APP_STORE_ADD_ALL", "BOOKMARK_REMOVE_GROUP"}) {
        CHECK(byName.count(no) == 1);
        CHECK(mc::detail::denied(policy, no));
    }
    // commands the engine does not implement for applyCommand are not in the vocabulary at all, so a macro naming them is refused as unknown
    for (const char* none : {"SETTING_SET", "PROBLEM_REPORT", "FEATURE_REQUEST", "UPDATE_CHECK", "USER_PRESET_FORGET", "PLUGIN_TX_GRANT"}) {
        CHECK(byName.count(none) == 0);
        CHECK(bad(mac("m", {st(none)})).find("unknown command") != std::string::npos);
    }
}

// ---- one macro ------------------------------------------------------------------------------------------------------------------------------

void testMacroValidation() {
    Macro ok = mac("Morning scan", {st("SET_FREQUENCY", 500), st("SET_MODE", 0), st("RECORD_IQ", 60000), st(mc::kWait, 1000), st("RECORD_IQ")});
    ok.steps[0].num[0] = 145.5e6;
    ok.steps[2].ival[0] = 1;
    CHECK(bad(ok).empty());
    CHECK(mc::validateSet({ok}, known()).empty());

    Macro m = ok;
    m.name = "";
    CHECK(!bad(m).empty());
    m.name = std::string(65, 'n');
    CHECK(!bad(m).empty());
    m.name = std::string(64, 'n');
    CHECK(bad(m).empty());
    m.name = "a\nb";
    CHECK(!bad(m).empty());
    m = ok; m.description = std::string(501, 'd'); CHECK(!bad(m).empty());
    m = ok; m.description = "x\x01"; CHECK(!bad(m).empty());
    m = ok; m.repeat = 0; CHECK(!bad(m).empty());
    m = ok; m.repeat = -3; CHECK(!bad(m).empty());
    m = ok; m.repeat = 1001; CHECK(!bad(m).empty());
    m = ok; m.steps.clear(); CHECK(bad(m).find("at least one") != std::string::npos);
    m = ok; m.steps.assign(501, st("SET_MODE", 1)); CHECK(bad(m).find("500") != std::string::npos);
    m = ok; m.steps.assign(500, st("SET_MODE", 1)); CHECK(bad(m).empty());
    m = ok; m.steps[1].delayMs = -1; CHECK(bad(m).find("step 2") != std::string::npos);
    m = ok; m.steps[1].delayMs = mc::kMaxDelayMs + 1; CHECK(bad(m).find("step 2") != std::string::npos);
    m = ok; m.steps[1].delayMs = mc::kMaxDelayMs; m.steps[0].delayMs = 0; m.steps[2].delayMs = 0; m.steps[3].delayMs = 0; CHECK(bad(m).empty());
    m = ok; m.steps[0].num[2] = std::nan(""); CHECK(bad(m).find("finite") != std::string::npos);
    m = ok; m.steps[0].num[3] = std::numeric_limits<double>::infinity(); CHECK(!bad(m).empty());
    m = ok; m.steps[0].text = std::string(256, 't'); CHECK(!bad(m).empty());
    m = ok; m.steps[0].text = std::string(255, 't'); CHECK(bad(m).empty());
    m = ok; m.steps[0].text = "a\tb"; CHECK(!bad(m).empty());
    m = ok; m.steps[0].command = "NO_SUCH_COMMAND"; CHECK(bad(m).find("unknown command") != std::string::npos);
    m = ok; m.steps[0].command = "set_frequency"; CHECK(!bad(m).empty());                  // case matters: names are the application's
    m = ok; m.steps[0].command = "SET FREQUENCY"; CHECK(!bad(m).empty());
    m = ok; m.steps[0].command = ""; CHECK(!bad(m).empty());
    m = ok; m.steps[0].command = std::string(70, 'A'); CHECK(!bad(m).empty());
    m = ok; m.steps[0].command = "SET_MODE\n"; CHECK(!bad(m).empty());
    m = ok; m.steps[0].command = "TX_PTT"; CHECK(bad(m).find("not allowed") != std::string::npos);
    m = ok; m.steps[0].command = "PLUGIN_GRANT"; CHECK(bad(m).find("not allowed") != std::string::npos);
    m = ok; m.steps[3].text = "x"; CHECK(bad(m).find("WAIT") != std::string::npos);
    m = ok; m.steps[3] = st(mc::kRunMacro); CHECK(bad(m).find("name") != std::string::npos);
    // repeat protection: a repeating macro must spend time
    m = mac("spin", {st("SET_MODE", 0)}, 2);
    CHECK(bad(m).find("spin") != std::string::npos);
    m = mac("spin2", {st("SET_MODE", 50), st("SET_MODE", 49)}, 2);
    CHECK(bad(m).find("spin") != std::string::npos);                               // 99 ms per pass
    m = mac("slow", {st("SET_MODE", 50), st("SET_MODE", 50)}, 2);
    CHECK(bad(m).empty());                                                         // exactly 100 ms
    m = mac("once", {st("SET_MODE", 0)}, 1);
    CHECK(bad(m).empty());                                                         // a single pass needs no delay
    m = mac("long", {st("SET_MODE", mc::kMaxDelayMs)}, 2);
    CHECK(bad(m).find("24 hours") != std::string::npos);
    m = mac("many", std::vector<Step>(11, st("SET_MODE", 1000)), 1000);
    CHECK(!bad(m).empty());                                                        // 11 s x 1000 = 3 h but 11000 steps
}

// ---- a set: cycles, depth, size --------------------------------------------------------------------------------------------------------

void testSetValidation() {
    auto runs = [](const std::string& target, std::int64_t delay = 0) { return st(mc::kRunMacro, delay, target); };
    CHECK(mc::validateSet({}, known()).empty());
    {   // names
        const auto e = mc::validateSet({mac("a", {st("SET_MODE")}), mac("a", {st("SET_MODE")})}, known());
        CHECK(e.find("twice") != std::string::npos);
    }
    {   // a missing target
        const auto e = mc::validateSet({mac("a", {runs("ghost")})}, known());
        CHECK(e.find("does not exist") != std::string::npos && e.find("ghost") != std::string::npos);
    }
    {   // direct, two-way and three-way loops
        CHECK(mc::validateSet({mac("a", {runs("a")})}, known()).find("runs itself") != std::string::npos);
        CHECK(mc::validateSet({mac("a", {runs("b")}), mac("b", {runs("a")})}, known()).find("runs itself") != std::string::npos);
        CHECK(mc::validateSet({mac("a", {runs("b")}), mac("b", {runs("c")}), mac("c", {st("SET_MODE"), runs("a")})}, known()).find("runs itself") != std::string::npos);
    }
    {   // a diamond is not a loop: a runs b and c, both run d
        const auto e = mc::validateSet({mac("a", {runs("b"), runs("c"), runs("d")}), mac("b", {runs("d")}), mac("c", {runs("d")}), mac("d", {st("SET_MODE")})}, known());
        CHECK(e.empty());
        // and the same macro run twice in a row
        CHECK(mc::validateSet({mac("a", {runs("d"), runs("d")}), mac("d", {st("SET_MODE")})}, known()).empty());
    }
    {   // depth: a chain of 4 is allowed, 5 is not
        std::vector<Macro> chain;
        for (int i = 1; i <= 4; ++i) { chain.push_back(mac("m" + std::to_string(i), {i < 4 ? runs("m" + std::to_string(i + 1)) : st("SET_MODE")})); }
        CHECK(mc::validateSet(chain, known()).empty());
        chain[3] = mac("m4", {runs("m5")});
        chain.push_back(mac("m5", {st("SET_MODE")}));
        CHECK(mc::validateSet(chain, known()).find("nested more than 4") != std::string::npos);
    }
    {   // an expansion bomb: each level runs the next 10 times (and repeats): 10 x 10 x 10 x 10 = 10000 plus the run steps themselves
        std::vector<Step> ten(10, runs("l2"));
        std::vector<Macro> bomb;
        bomb.push_back(mac("l1", ten));
        bomb.push_back(mac("l2", std::vector<Step>(10, runs("l3"))));
        bomb.push_back(mac("l3", std::vector<Step>(10, runs("l4"))));
        bomb.push_back(mac("l4", std::vector<Step>(10, st("SET_MODE"))));
        const auto e = mc::validateSet(bomb, known());
        CHECK(e.find("10000") != std::string::npos);
        // the same shape with fewer steps is fine
        bomb[1] = mac("l2", std::vector<Step>(5, runs("l3")));
        bomb[0] = mac("l1", std::vector<Step>(5, runs("l2")));
        CHECK(mc::validateSet(bomb, known()).empty());
    }
    {   // the set size
        std::vector<Macro> many;
        for (int i = 0; i < 201; ++i) { many.push_back(mac("m" + std::to_string(i), {st("SET_MODE")})); }
        CHECK(mc::validateSet(many, known()).find("200") != std::string::npos);
        many.pop_back();
        CHECK(mc::validateSet(many, known()).empty());
    }
    {   // one bad macro is named
        const auto e = mc::validateSet({mac("good", {st("SET_MODE")}), mac("evil", {st("TX_PTT")})}, known());
        CHECK(e.find("evil") != std::string::npos && e.find("TX_PTT") != std::string::npos);
    }
}

// ---- the player ---------------------------------------------------------------------------------------------------------------------------------

void testPlayer() {
    Macro m = mac("seq", {st("SET_FREQUENCY", 1000), st("SET_MODE", 500), st("RECORD_IQ", 0)});
    m.steps[0].num[0] = 7.1e6;
    mc::Player p;
    std::string err;
    CHECK(!p.start({m}, "nope", 0, &err) && err.find("nope") != std::string::npos);
    CHECK(!p.running() && p.nextDueMs() == -1);
    CHECK(p.start({m}, "seq", 10000, &err));
    CHECK(p.running() && p.nextDueMs() == 10000);
    CHECK(!p.start({m}, "seq", 10000, &err) && err.find("already") != std::string::npos);
    auto a = p.poll(9999);
    CHECK(a.empty());                                   // not before the first step is due
    a = p.poll(10000);
    CHECK(a.size() == 1 && a[0].step.command == "SET_FREQUENCY" && a[0].step.num[0] == 7.1e6 && a[0].index == 0 && a[0].macro == "seq" && a[0].pass == 0);
    CHECK(p.nextDueMs() == 11000);                      // the first step's delay
    CHECK(p.poll(10999).empty());
    a = p.poll(11000);
    CHECK(a.size() == 1 && a[0].step.command == "SET_MODE");
    CHECK(p.poll(11499).empty());
    a = p.poll(11500);
    CHECK(a.size() == 1 && a[0].step.command == "RECORD_IQ");
    CHECK(!p.running() && p.executed() == 3);           // finished
    CHECK(p.poll(99999).empty());                       // nothing is returned twice

    // a late poll catches up in one call and keeps the schedule (no drift)
    CHECK(p.start({m}, "seq", 0, &err));
    a = p.poll(100000);
    CHECK(a.size() == 3 && a[0].index == 0 && a[1].index == 1 && a[2].index == 2);
    // maxSteps limits one call
    CHECK(p.start({m}, "seq", 0, &err));
    a = p.poll(100000, 2);
    CHECK(a.size() == 2 && p.running());
    a = p.poll(100000, 2);
    CHECK(a.size() == 1 && !p.running());
    // cancel
    CHECK(p.start({m}, "seq", 0, &err));
    CHECK(p.poll(0).size() == 1);
    p.cancel();
    CHECK(!p.running() && p.poll(100000).empty() && p.nextDueMs() == -1);
    CHECK(p.stopReason().empty());
    CHECK(p.start({m}, "seq", 0, &err));               // and it can start again
    p.cancel();

    // WAIT is returned (the wiring may show it), repeats run in order with the pass number
    Macro r = mac("rep", {st("SET_MODE", 100), st(mc::kWait, 400)}, 3);
    CHECK(p.start({r}, "rep", 0, &err));
    a = p.poll(100000);
    CHECK(a.size() == 6);
    if (a.size() == 6) {
        CHECK(a[0].pass == 0 && a[1].step.command == mc::kWait && a[2].pass == 1 && a[5].pass == 2 && a[5].step.command == mc::kWait);
    }
    CHECK(!p.running() && p.executed() == 6);
    // the pass timing: pass 2 starts 500 ms after pass 1
    CHECK(p.start({r}, "rep", 0, &err));
    CHECK(p.poll(0).size() == 1 && p.poll(99).empty() && p.poll(100).size() == 1);
    CHECK(p.poll(499).empty() && p.poll(500).size() == 1);          // pass 1 begins at 500
    p.cancel();

    // a nested run: the inner macro's steps come between the outer's, and the delays add
    Macro outer = mac("outer", {st("SET_MODE", 100), st(mc::kRunMacro, 200, "inner"), st("SET_VOLUME", 0)});
    Macro inner = mac("inner", {st("SET_GAIN", 300), st("SET_SQUELCH", 0)});
    CHECK(mc::validateSet({outer, inner}, known()).empty());
    CHECK(p.start({outer, inner}, "outer", 0, &err));
    a = p.poll(0);
    CHECK(a.size() == 1 && a[0].macro == "outer");
    CHECK(p.nextDueMs() == 100);
    a = p.poll(100);                                    // RUN_MACRO is consumed; the inner's first step is due 200 ms after it
    CHECK(a.empty());
    CHECK(p.nextDueMs() == 300);
    a = p.poll(300);
    CHECK(a.size() == 1 && a[0].macro == "inner" && a[0].step.command == "SET_GAIN");
    a = p.poll(600);
    CHECK(a.size() == 2 && a[0].macro == "inner" && a[0].step.command == "SET_SQUELCH" && a[1].macro == "outer" && a[1].step.command == "SET_VOLUME");
    CHECK(!p.running());
    // a repeating inner macro inside a repeating outer one
    Macro o2 = mac("o2", {st(mc::kRunMacro, 100, "i2")}, 2);
    Macro i2 = mac("i2", {st("SET_MODE", 50), st("SET_MODE", 50)}, 3);
    CHECK(mc::validateSet({o2, i2}, known()).empty());
    CHECK(p.start({o2, i2}, "o2", 0, &err));
    a = p.poll(1000000);
    CHECK(a.size() == 12 && !p.running());              // 2 outer passes x 3 inner passes x 2 steps
}

void testPlayerSafetyNets() {
    // The player does not trust its input: a set that skipped validation still cannot loop or explode.
    mc::Player p;
    std::string err;
    {
        Macro self = mac("loop", {st("SET_MODE", 1), st(mc::kRunMacro, 1, "loop")});
        CHECK(p.start({self}, "loop", 0, &err));
        const auto a = p.poll(1000000);
        CHECK(!p.running() && p.stopReason().find("itself") != std::string::npos);
        CHECK(a.size() == 1);
    }
    {
        Macro a1 = mac("a", {st(mc::kRunMacro, 0, "b")}), b1 = mac("b", {st("SET_MODE"), st(mc::kRunMacro, 0, "a")});
        CHECK(p.start({a1, b1}, "a", 0, &err));
        p.poll(1000000);
        CHECK(!p.running() && p.stopReason().find("itself") != std::string::npos);
    }
    {   // depth 5
        std::vector<Macro> chain;
        for (int i = 1; i <= 5; ++i) { chain.push_back(mac("m" + std::to_string(i), {i < 5 ? st(mc::kRunMacro, 0, "m" + std::to_string(i + 1)) : st("SET_MODE")})); }
        CHECK(p.start(chain, "m1", 0, &err));
        p.poll(1000000);
        CHECK(!p.running() && p.stopReason().find("4 deep") != std::string::npos);
    }
    {   // a missing macro
        CHECK(p.start({mac("a", {st(mc::kRunMacro, 0, "ghost")})}, "a", 0, &err));
        p.poll(1000);
        CHECK(!p.running() && p.stopReason().find("does not exist") != std::string::npos);
    }
    {   // 10000 steps, however they are made: one macro of 500 steps repeated 1000 times
        Macro big = mac("big", std::vector<Step>(500, st("SET_MODE", 1)), 1000);
        CHECK(p.start({big}, "big", 0, &err));
        std::size_t total = 0;
        for (int i = 0; i < 100 && p.running(); ++i) { total += p.poll(1000000000LL, 5000).size(); }
        CHECK(!p.running() && total == 10000 && p.stopReason().find("10000") != std::string::npos);
    }
    {   // 24 hours
        Macro slow = mac("slow", {st("SET_MODE", mc::kMaxDelayMs), st("SET_MODE", mc::kMaxDelayMs), st("SET_MODE", 0)});
        CHECK(p.start({slow}, "slow", 0, &err));
        const auto a = p.poll(10LL * mc::kMaxDelayMs);
        CHECK(a.size() == 2 && !p.running() && p.stopReason().find("24 hours") != std::string::npos);
    }
}

// ---- file ------------------------------------------------------------------------------------------------------------------------------------

bool sameMacro(const Macro& a, const Macro& b) {
    if (a.name != b.name || a.description != b.description || a.repeat != b.repeat || a.steps.size() != b.steps.size()) { return false; }
    for (std::size_t i = 0; i < a.steps.size(); ++i) {
        const Step &x = a.steps[i], &y = b.steps[i];
        if (x.command != y.command || x.text != y.text || x.delayMs != y.delayMs || x.ival[0] != y.ival[0] || x.ival[1] != y.ival[1]) { return false; }
        for (int k = 0; k < 4; ++k) { if (x.num[k] != y.num[k]) { return false; } }
    }
    return true;
}

void testFile() {
    Macro a = mac("Evening \"net\" \xC3\xA9", {st("SET_FREQUENCY", 1000), st("SET_MODE", 250), st(mc::kRunMacro, 0, "tail"), st(mc::kWait, 5000)}, 2);
    a.description = "tunes and records";
    a.steps[0].num[0] = 3.5735e6;
    a.steps[0].num[3] = -1.5;
    a.steps[1].ival[0] = 5;
    a.steps[1].ival[1] = -7;
    a.steps[1].text = "USB";
    Macro tail = mac("tail", {st("RECORD_IQ", 60000), st("RECORD_IQ", 0)});
    tail.steps[0].ival[0] = 1;
    const std::string text = mc::macrosToJson({a, tail});
    const auto r = mc::parseMacros(text, known());
    CHECK(r.error.empty() && r.macros.size() == 2);
    if (r.macros.size() == 2) { CHECK(sameMacro(r.macros[0], a) && sameMacro(r.macros[1], tail)); }
    const fs::path d = fs::temp_directory_path() / "coref-macros";
    fs::create_directories(d);
    const std::string p = (d / "macros.json").string();
    std::string err;
    CHECK(mc::saveMacrosFile(p, {a, tail}, known(), &err));
    CHECK(!fs::exists(p + ".tmp"));
    auto l = mc::loadMacrosFile(p, known());
    CHECK(l.error.empty() && l.macros.size() == 2 && sameMacro(l.macros[0], a));
    // an invalid set is refused and the old file stays
    const auto before = fs::file_size(p);
    CHECK(!mc::saveMacrosFile(p, {mac("evil", {st("TX_PTT")})}, known(), &err) && err.find("TX_PTT") != std::string::npos);
    CHECK(fs::file_size(p) == before);
    CHECK(!mc::saveMacrosFile(p, {mac("loop", {st(mc::kRunMacro, 0, "loop")})}, known(), &err));
    { std::ofstream f(d / "bom.json", std::ios::binary); f << "\xEF\xBB\xBF" << text; }
    CHECK(mc::loadMacrosFile((d / "bom.json").string(), known()).macros.size() == 2);
    CHECK(!mc::loadMacrosFile((d / "missing.json").string(), known()).error.empty());
    CHECK(!mc::loadMacrosFile(d.string(), known()).error.empty());
    CHECK(!mc::saveMacrosFile((d / "nodir" / "x" / "m.json").string(), {tail}, known(), &err));
    {
        std::ofstream f(d / "big.json", std::ios::binary);
        std::string chunk(1 << 20, ' ');
        f << chunk << chunk;
    }
    CHECK(mc::loadMacrosFile((d / "big.json").string(), known()).error.find("1 MiB") != std::string::npos);
    std::error_code ec;
    fs::remove_all(d, ec);
    // a hand-written minimal file
    const auto m = mc::parseMacros("{\"version\":1,\"macros\":[{\"name\":\"m\",\"steps\":[{\"cmd\":\"SET_MODE\"}]}]}", known());
    CHECK(m.error.empty() && m.macros.size() == 1 && m.macros[0].repeat == 1 && m.macros[0].steps[0].delayMs == 0);
}

void testFileHostile() {
    auto refused = [](const std::string& text, const char* mention = nullptr) {
        const auto r = mc::parseMacros(text, known());
        const bool ok = !r.error.empty() && r.macros.empty() && (mention == nullptr || r.error.find(mention) != std::string::npos);
        if (!ok) { std::printf("  NOT REFUSED / wrong message: %.110s -> '%s'\n", text.c_str(), r.error.c_str()); }
        return ok;
    };
    const std::string pre = "{\"version\":1,\"macros\":[";
    auto one = [&](const std::string& stepJson, const std::string& extra = "") { return pre + "{\"name\":\"m\"" + extra + ",\"steps\":[" + stepJson + "]}]}"; };
    CHECK(refused(""));
    CHECK(refused("[]"));
    CHECK(refused("{\"macros\":[]}", "version"));
    CHECK(refused("{\"version\":3,\"macros\":[]}", "version"));
    CHECK(refused("{\"version\":1}", "macros"));
    CHECK(refused(pre + "7]}", "macro 1"));
    CHECK(refused(pre + "{}]}", "name"));
    CHECK(refused(pre + "{\"name\":\"m\"}]}", "steps"));
    CHECK(refused(pre + "{\"name\":\"m\",\"steps\":{}}]}", "steps"));
    CHECK(refused(pre + "{\"name\":\"m\",\"steps\":[]}]}", "at least one"));
    CHECK(refused(one("5"), "step 1"));
    CHECK(refused(one("{}"), "cmd"));
    CHECK(refused(one("{\"cmd\":5}"), "cmd"));
    CHECK(refused(one("{\"cmd\":\"NOPE\"}"), "unknown command"));
    CHECK(refused(one("{\"cmd\":\"TX_PTT\"}"), "not allowed"));
    CHECK(refused(one("{\"cmd\":\"SET_MODE\",\"num\":5}"), "num"));
    CHECK(refused(one("{\"cmd\":\"SET_MODE\",\"num\":[1,2,3,4,5]}"), "num"));
    CHECK(refused(one("{\"cmd\":\"SET_MODE\",\"num\":[\"1\"]}"), "num"));
    CHECK(refused(one("{\"cmd\":\"SET_MODE\",\"ival\":[1.5]}"), "ival"));
    CHECK(refused(one("{\"cmd\":\"SET_MODE\",\"ival\":[1,2,3]}"), "ival"));
    CHECK(refused(one("{\"cmd\":\"SET_MODE\",\"text\":7}"), "text"));
    CHECK(refused(one("{\"cmd\":\"SET_MODE\",\"text\":\"a\\u0001\"}"), "control"));
    CHECK(refused(one("{\"cmd\":\"SET_MODE\",\"delayMs\":-5}"), "delay"));
    CHECK(refused(one("{\"cmd\":\"SET_MODE\",\"delayMs\":1.5}"), "delayMs"));
    CHECK(refused(one("{\"cmd\":\"SET_MODE\",\"delayMs\":99999999999}"), "delay"));
    CHECK(refused(one("{\"cmd\":\"SET_MODE\",\"num\":[1e999]}")));                          // overflows to a non-number the parser refuses
    CHECK(refused(one("{\"cmd\":\"SET_MODE\"}", ",\"repeat\":0"), "repeat"));
    CHECK(refused(one("{\"cmd\":\"SET_MODE\"}", ",\"repeat\":1001"), "repeat"));
    CHECK(refused(one("{\"cmd\":\"SET_MODE\"}", ",\"repeat\":2.5"), "repeat"));
    CHECK(refused(one("{\"cmd\":\"SET_MODE\"}", ",\"repeat\":2"), "spin"));
    CHECK(refused(one("{\"cmd\":\"SET_MODE\"}", ",\"description\":5"), "description"));
    CHECK(refused(one("{\"cmd\":\"RUN_MACRO\",\"text\":\"m\"}"), "itself"));
    CHECK(refused(one("{\"cmd\":\"RUN_MACRO\",\"text\":\"ghost\"}"), "does not exist"));
    CHECK(refused(pre + "{\"name\":\"a\",\"steps\":[{\"cmd\":\"SET_MODE\"}]},{\"name\":\"a\",\"steps\":[{\"cmd\":\"SET_MODE\"}]}]}", "twice"));
    // the first bad macro and step are named
    {
        const auto r = mc::parseMacros(pre + "{\"name\":\"ok\",\"steps\":[{\"cmd\":\"SET_MODE\"}]},{\"name\":\"bad\",\"steps\":[{\"cmd\":\"SET_MODE\"},{\"cmd\":\"TX_OPEN\"}]}]}", known());
        CHECK(r.macros.empty() && r.error.find("macro 2 ('bad')") != std::string::npos && r.error.find("step 2") != std::string::npos);
    }
    CHECK(refused(std::string(2 << 20, ' '), "1 MiB"));
    CHECK(refused(std::string("{\"version\":1,\"macros\":[]}\0", 26), "NUL"));
    CHECK(refused(std::string(300000, '['), "nested"));
    CHECK(refused(one("{\"cmd\":\"SET_MODE\",\"x\":[[[[[[[[[1]]]]]]]]]}"), "nested"));
    CHECK(mc::parseMacros(pre + "{\"name\":\"[[[{{{\",\"steps\":[{\"cmd\":\"SET_MODE\",\"text\":\"]]]}}}\\\"[[\"}]}]}", known()).error.empty());
    CHECK(mc::parseMacros(pre + "{\"name\":\"m\",\"future\":1,\"steps\":[{\"cmd\":\"SET_MODE\",\"future\":[1]}]}],\"extra\":2}", known()).macros.size() == 1);
    {   // random bytes never crash
        unsigned s = 5;
        int wrong = 0;
        for (int n = 0; n < 200; ++n) {
            std::string t;
            for (int i = 0; i < 300; ++i) { s = s * 1103515245u + 12345u; t.push_back(static_cast<char>((s >> 16) & 0xFF)); }
            const auto r = mc::parseMacros(t, known());
            if (!r.error.empty() && !r.macros.empty()) { ++wrong; }
        }
        CHECK(wrong == 0);
    }
    // the denied commands are refused whatever the file says; a policy with no denials (the owner's choice) accepts them
    mc::MacroPolicy none;
    none.deniedPrefixes.clear();
    CHECK(mc::parseMacros(one("{\"cmd\":\"TX_PTT\"}"), known(), none).error.empty());
}

}  // namespace

int main() {
    std::printf("macros\n");
    testVocabulary();
    testMacroValidation();
    testSetValidation();
    testPlayer();
    testPlayerSafetyNets();
    testFile();
    testFileHostile();
    return testSummary("test_macros");
}
