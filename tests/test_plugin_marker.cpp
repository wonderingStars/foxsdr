// test_plugin_marker.cpp - the crash-loop breaker's rules (plugin containment P1, item 1; docs/engine-stage4.md 6.28, core/plugin_marker.hpp).
//
//   A  WHO IS GUILTY (suspectsOf): a table of what a dead run's marker held and who is suspected - the stinginess is the feature.
//   B  THE STRIKES (planStart / graduate): the whole story, step by step - strike one skips ONE start, probation, strike two disables, a clean
//      run takes a strike off, a person's re-enable keeps the count, the application's own crash never costs a plugin.
//   C  THE MARKER'S FILE: phases kept in a memory-mapped file survive "the process dying" (the mapping is simply dropped), blame is a plain
//      store, an orderly stop leaves nothing, the table is bounded, and many threads setting and clearing lose nothing.
//   D  THE STORE: a round trip, a damaged file is an empty store that says so, an empty store is no file.
//   E  THE ENGINE'S HANDLE (Containment), across two simulated runs in one scratch folder: the gate, the notices, a re-enable, and safe mode
//      (which refuses everything and changes no record).
//   F  FIRST-PASS HAS A CLOCK.
//   G  THE DEEP REVIEW OF engine/stage4, BATCH A (docs/engine-stage4.md 6.31): ONE ID EVERYWHERE (a plugin whose id is longer than a slot, or written in
//      capitals, is found again by the gate, the record and the re-enable); BRACKETS THAT OVERLAP leave the phase where the plugin RESTS; TWO SETTINGS
//      FILES IN ONE FOLDER keep a marker and a store of their own; and the marker's pages STAY IN THE WORKING SET when memory is trimmed.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "core/diag_log.hpp"
#include "core/plugin_fault.hpp"
#include "core/plugin_marker.hpp"
#include "core/plugin_run.hpp"
#include "test_check.hpp"

#if defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#endif

namespace fs = std::filesystem;
namespace marker = cascade::core::marker;
using cascade::core::Containment;
using cascade::core::StartPlan;
using cascade::core::StrikeRecord;
using cascade::core::StrikeState;
using marker::InPlay;
using marker::Phase;

namespace {

InPlay play(const char* id, Phase phase, bool blamed = false, const char* version = "1.0.0") {
    InPlay p;
    p.id = id;
    p.version = version;
    p.phase = phase;
    p.blamed = blamed;
    return p;
}

std::vector<std::string> ids(const std::vector<InPlay>& v) {
    std::vector<std::string> out;
    for (const InPlay& p : v) { out.push_back(p.id); }
    return out;
}

const StrikeRecord* rec(const StartPlan& plan, const char* id) {
    for (const StrikeRecord& r : plan.records) {
        if (r.id == id) { return &r; }
    }
    return nullptr;
}

bool skips(const StartPlan& plan, const char* id) {
    for (const std::string& s : plan.skip) {
        if (s == id) { return true; }
    }
    return false;
}

// ---- A ----------------------------------------------------------------------------------------------------------------------------
void testSuspects() {
    using V = std::vector<InPlay>;
    // inside a call at death
    CHECK(ids(cascade::core::suspectsOf(V{play("x", Phase::Load)})) == std::vector<std::string>{"x"});
    CHECK(ids(cascade::core::suspectsOf(V{play("x", Phase::Create)})) == std::vector<std::string>{"x"});
    CHECK(ids(cascade::core::suspectsOf(V{play("x", Phase::Unload)})) == std::vector<std::string>{"x"});
    // the steady state proves nothing
    CHECK(cascade::core::suspectsOf(V{play("x", Phase::Running)}).empty());
    CHECK(cascade::core::suspectsOf(V{play("x", Phase::Idle)}).empty());
    CHECK(cascade::core::suspectsOf(V{}).empty());
    // a thirty-plugin run that died with the application's own bug: every plugin Running, nobody blamed
    {
        V all;
        for (int i = 0; i < 30; ++i) { all.push_back(play(("p" + std::to_string(i)).c_str(), Phase::Running)); }
        CHECK(cascade::core::suspectsOf(all).empty());
    }
    // blamed: suspect whatever the phase
    CHECK(ids(cascade::core::suspectsOf(V{play("x", Phase::Running, true)})) == std::vector<std::string>{"x"});
    CHECK(ids(cascade::core::suspectsOf(V{play("x", Phase::Idle, true)})) == std::vector<std::string>{"x"});
    // first-pass: ONE alone is the evidence there is; two prove nothing about either; one beside a plugin inside a call is overshadowed
    CHECK(ids(cascade::core::suspectsOf(V{play("x", Phase::FirstPass)})) == std::vector<std::string>{"x"});
    CHECK(cascade::core::suspectsOf(V{play("x", Phase::FirstPass), play("y", Phase::FirstPass)}).empty());
    CHECK(ids(cascade::core::suspectsOf(V{play("x", Phase::FirstPass), play("y", Phase::Create)})) == std::vector<std::string>{"y"});
    CHECK(ids(cascade::core::suspectsOf(V{play("x", Phase::FirstPass), play("y", Phase::Running)})) == std::vector<std::string>{"x"});
    // the case the three created plugins of a rebuild make: the 4th faulted in create(); the first three are in first-pass: only the 4th
    CHECK(ids(cascade::core::suspectsOf(V{play("a", Phase::FirstPass), play("b", Phase::FirstPass), play("c", Phase::FirstPass), play("d", Phase::Create)})) ==
          std::vector<std::string>{"d"});
    // two at once inside calls (two threads): both
    CHECK(cascade::core::suspectsOf(V{play("x", Phase::Create), play("y", Phase::Unload)}).size() == 2);
    // blamed and in first-pass beside another first-pass: the blame stands, the other is not dragged in
    CHECK(ids(cascade::core::suspectsOf(V{play("x", Phase::FirstPass, true), play("y", Phase::FirstPass)})) == std::vector<std::string>{"x"});
}

// ---- B ----------------------------------------------------------------------------------------------------------------------------
void testStrikes() {
    // 1. FIRST SUSPICION: skipped this start, with a notice that is not permanent
    StartPlan p1 = cascade::core::planStart({}, {play("bad", Phase::Create, false, "2.0.1")}, 1000);
    CHECK(skips(p1, "bad"));
    CHECK(rec(p1, "bad") != nullptr && rec(p1, "bad")->strikes == 1 && rec(p1, "bad")->state == StrikeState::Suspect);
    CHECK(rec(p1, "bad")->version == "2.0.1" && rec(p1, "bad")->phase == "create" && rec(p1, "bad")->whenUnix == 1000);
    CHECK(p1.notices.size() == 1 && p1.notices[0].id == "bad" && p1.notices[0].strikes == 1 && !p1.notices[0].permanent);
    CHECK(p1.suspects == std::vector<std::string>{"bad"});

    // 2. THE START AFTER: nothing was in play (the plugin was not loaded), so it is promoted to probation and LOADS; no notice
    StartPlan p2 = cascade::core::planStart(p1.records, {}, 2000);
    CHECK(!skips(p2, "bad"));
    CHECK(rec(p2, "bad")->state == StrikeState::Probation && rec(p2, "bad")->strikes == 1);
    CHECK(p2.notices.empty());

    // 3a. A CLEAN RUN in which it ran takes the strike off
    {
        const std::vector<StrikeRecord> after = cascade::core::graduate(p2.records, {"bad"});
        CHECK(after.empty());
        // ...a clean run in which it did NOT run (dormant all session) does not
        const std::vector<StrikeRecord> kept = cascade::core::graduate(p2.records, {"other"});
        CHECK(kept.size() == 1);
    }
    // 3b. ...but a graduation is for probation only: a suspect that never loaded has nothing to graduate from
    CHECK(cascade::core::graduate(p1.records, {"bad"}).size() == 1);

    // 4. SUSPECTED AGAIN on probation: STRIKE TWO, disabled, a permanent notice
    StartPlan p4 = cascade::core::planStart(p2.records, {play("bad", Phase::Create)}, 3000);
    CHECK(skips(p4, "bad"));
    CHECK(rec(p4, "bad")->strikes == 2 && rec(p4, "bad")->state == StrikeState::Disabled);
    CHECK(p4.notices.size() == 1 && p4.notices[0].permanent && p4.notices[0].strikes == 2);

    // 5. DISABLED STAYS DISABLED start after start, quietly (no notice: the person has been told)
    StartPlan p5 = cascade::core::planStart(p4.records, {}, 4000);
    CHECK(skips(p5, "bad") && rec(p5, "bad")->state == StrikeState::Disabled && p5.notices.empty());
    StartPlan p5b = cascade::core::planStart(p5.records, {}, 5000);
    CHECK(skips(p5b, "bad"));

    // 6. THE PERSON RE-ENABLES: probation, strikes kept - so a crash with it in play is strike THREE and disabled at once
    {
        std::vector<StrikeRecord> reenabled = p5b.records;
        reenabled[0].state = StrikeState::Probation;   // (what Containment::reenable does; tested in E)
        StartPlan p6 = cascade::core::planStart(reenabled, {}, 6000);
        CHECK(!skips(p6, "bad"));
        StartPlan p6b = cascade::core::planStart(reenabled, {play("bad", Phase::Load)}, 7000);
        CHECK(skips(p6b, "bad") && rec(p6b, "bad")->strikes == 3 && rec(p6b, "bad")->state == StrikeState::Disabled);
    }

    // 7. THE APPLICATION'S OWN CRASH NEVER COSTS A PLUGIN: thirty running, none blamed, none touched - even one on probation
    {
        StartPlan clean = cascade::core::planStart(p2.records, {}, 0);
        std::vector<InPlay> everyone;
        for (int i = 0; i < 30; ++i) { everyone.push_back(play(("p" + std::to_string(i)).c_str(), Phase::Running)); }
        everyone.push_back(play("bad", Phase::Running));
        StartPlan after = cascade::core::planStart(clean.records, everyone, 8000);
        CHECK(after.suspects.empty() && after.notices.empty() && after.skip.empty());
        CHECK(rec(after, "bad") != nullptr && rec(after, "bad")->strikes == 1 && rec(after, "bad")->state == StrikeState::Probation);
        CHECK(after.records.size() == 1);
    }

    // 8. TWO SUSPECTS AT ONCE are two strikes, in the order found; an unrelated record is carried over untouched
    {
        StrikeRecord other;
        other.id = "other";
        other.strikes = 1;
        other.state = StrikeState::Probation;
        StartPlan plan = cascade::core::planStart({other}, {play("x", Phase::Create), play("y", Phase::Unload)}, 9000);
        CHECK(plan.notices.size() == 2 && plan.notices[0].id == "x" && plan.notices[1].id == "y");
        CHECK(skips(plan, "x") && skips(plan, "y") && !skips(plan, "other"));
        CHECK(rec(plan, "other")->state == StrikeState::Probation);
    }

    // 9. A STRIKE-ONE RECORD THAT IS SUSPECTED AGAIN BEFORE ITS SECOND CHANCE (a session in between loaded it by a person's re-enable)
    {
        StartPlan again = cascade::core::planStart(p1.records, {play("bad", Phase::Create)}, 9500);
        CHECK(rec(again, "bad")->strikes == 2 && rec(again, "bad")->state == StrikeState::Disabled);
    }
}

// ---- C ----------------------------------------------------------------------------------------------------------------------------
std::string readAll(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void testMarkerFile(const fs::path& dir) {
    const std::string path = (dir / marker::kFileName).string();
    marker::resetForTest();

    // a first run: nothing before it
    CHECK(marker::open(path).empty());
    CHECK(marker::persistent());
    CHECK(fs::file_size(path) == marker::kFileBytes);
    marker::set("Alpha-1.0.0-abi3-win-x64.dll", Phase::Load, "1.0.0", cascade::core::fault::Origin::Official);
    marker::set("alpha", Phase::Create);   // the same plugin: file name, id, any case
    marker::set("beta.dll", Phase::Running, "2.1", cascade::core::fault::Origin::Site);
    marker::set("gamma", Phase::FirstPass);
    marker::set("delta", Phase::Unload);
    marker::clear("delta");
    CHECK(marker::phaseOf("ALPHA.dll") == Phase::Create);
    CHECK(marker::phaseOf("beta") == Phase::Running);
    CHECK(marker::phaseOf("delta") == Phase::Idle);
    CHECK(marker::phaseOf("nobody") == Phase::Idle);
    marker::blame("beta");
    marker::blame("nobody");   // no slot: not blamed, nothing created
    // the death: no cleanShutdown; the process is simply gone (the mapping dropped, the file as the page cache holds it)
    marker::resetForTest();
    const std::vector<InPlay> before = marker::open(path);
    std::vector<std::string> got = ids(before);
    std::sort(got.begin(), got.end());
    CHECK((got == std::vector<std::string>{"alpha", "beta", "gamma"}));
    for (const InPlay& p : before) {
        if (p.id == "alpha") {
            CHECK(p.phase == Phase::Create && p.version == "1.0.0" && p.origin == cascade::core::fault::Origin::Official && !p.blamed);
        }
        if (p.id == "beta") {
            CHECK(p.phase == Phase::Running && p.blamed && p.version == "2.1" && p.origin == cascade::core::fault::Origin::Site);
        }
        if (p.id == "gamma") { CHECK(p.phase == Phase::FirstPass && !p.blamed); }
    }
    // ...and the same bytes through the rules: alpha (inside create) and beta (blamed) are suspect, gamma (first-pass beside a call) is not
    {
        std::vector<std::string> s = ids(cascade::core::suspectsOf(before));
        std::sort(s.begin(), s.end());
        CHECK((s == std::vector<std::string>{"alpha", "beta"}));
    }
    // THIS run starts clean (open reset the file), and an orderly stop leaves nothing at all
    marker::set("alpha", Phase::Running);
    marker::cleanShutdown();
    marker::resetForTest();
    CHECK(marker::open(path).empty());

    // an Idle slot is a loaded module, not evidence; an Idle slot that was blamed is
    marker::set("idle-one", Phase::Idle);
    marker::set("idle-blamed", Phase::Idle);
    marker::blame("idle-blamed");
    marker::resetForTest();
    {
        const std::vector<InPlay> idleRun = marker::open(path);
        CHECK(idleRun.size() == 1 && idleRun[0].id == "idle-blamed" && idleRun[0].blamed);
    }

    // a file that is not ours (a stranger's bytes, a different layout, too short) reads as an empty previous run and is replaced
    marker::resetForTest();
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << "this is not a marker";
    }
    CHECK(marker::open(path).empty());
    CHECK(fs::file_size(path) == marker::kFileBytes);

    // the table is bounded: 64 slots, the rest counted as dropped and never a crash
    marker::resetForTest();
    marker::open(path);
    for (int i = 0; i < 80; ++i) { marker::set("many-" + std::to_string(i), Phase::Running); }
    CHECK(marker::dropped() == 80 - marker::kSlots);
    marker::cleanShutdown();

    // FIRST-PASS HAS A CLOCK (F): promoted once it has been alive for the interval, not before
    marker::resetForTest();
    marker::open(path);
    marker::set("late", Phase::FirstPass);
    const auto nowMs = static_cast<std::uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    marker::promoteFirstPass(nowMs + 4000, marker::kFirstPassMs);
    CHECK(marker::phaseOf("late") == Phase::FirstPass);
    CHECK(marker::ranThisSession().count("late") == 0);
    marker::promoteFirstPass(nowMs + 5100, marker::kFirstPassMs);
    CHECK(marker::phaseOf("late") == Phase::Running);
    CHECK(marker::ranThisSession().count("late") == 1);
    // a phase other than first-pass is left alone by the clock
    marker::set("create-now", Phase::Create);
    marker::promoteFirstPass(nowMs + 600000, marker::kFirstPassMs);
    CHECK(marker::phaseOf("create-now") == Phase::Create);
    marker::cleanShutdown();

    // THREADS: many setting and clearing at once lose nothing and do not corrupt a neighbour
    marker::resetForTest();
    marker::open(path);
    std::atomic<int> wrong{0};
    std::vector<std::thread> pool;
    for (int t = 0; t < 8; ++t) {
        pool.emplace_back([t, &wrong] {
            const std::string mine = "thread-" + std::to_string(t);
            for (int i = 0; i < 3000; ++i) {
                marker::set(mine, Phase::Create);
                if (marker::phaseOf(mine) != Phase::Create) { ++wrong; }
                marker::set(mine, Phase::Running);
                if (marker::phaseOf(mine) != Phase::Running) { ++wrong; }
                if ((i % 7) == 0) { marker::clear(mine); }
            }
        });
    }
    for (std::thread& th : pool) { th.join(); }
    CHECK(wrong.load() == 0);
    marker::cleanShutdown();
    marker::resetForTest();

    // an anonymous marker (a hermetic run): works, keeps nothing
    CHECK(marker::open("").empty());
    CHECK(!marker::persistent());
    marker::set("memory-only", Phase::Create);
    CHECK(marker::phaseOf("memory-only") == Phase::Create);
    marker::cleanShutdown();
    marker::resetForTest();
}

// ---- D ----------------------------------------------------------------------------------------------------------------------------
void testStore(const fs::path& dir) {
    const std::string path = (dir / "store.json").string();
    std::vector<StrikeRecord> in;
    StrikeRecord a;
    a.id = "alpha";
    a.version = "1.2.3";
    a.strikes = 2;
    a.state = StrikeState::Disabled;
    a.phase = "create";
    a.whenUnix = 1730000000;
    a.fromSite = true;
    StrikeRecord b;
    b.id = "beta";
    b.strikes = 1;
    b.state = StrikeState::Probation;
    in = {a, b};
    std::string err;
    CHECK(cascade::core::saveStore(path, in, err) && err.empty());
    std::vector<StrikeRecord> out;
    CHECK(cascade::core::loadStore(path, out, err));
    CHECK(out.size() == 2);
    if (out.size() == 2) {
        CHECK(out[0].id == "alpha" && out[0].version == "1.2.3" && out[0].strikes == 2 && out[0].state == StrikeState::Disabled &&
              out[0].phase == "create" && out[0].whenUnix == 1730000000 && out[0].fromSite);
        CHECK(out[1].id == "beta" && out[1].state == StrikeState::Probation && !out[1].fromSite);
    }
    CHECK(!fs::exists(path + ".tmp"));   // written atomically: no temp file left behind

    // missing: an empty store, no complaint
    std::vector<StrikeRecord> none;
    CHECK(cascade::core::loadStore((dir / "no-such.json").string(), none, err) && none.empty() && err.empty());
    // damaged: an empty store that says so
    {
        std::ofstream bad(path, std::ios::binary | std::ios::trunc);
        bad << "{ this is not json";
    }
    CHECK(!cascade::core::loadStore(path, none, err) && none.empty() && !err.empty());
    {
        std::ofstream alien(path, std::ios::binary | std::ios::trunc);
        alien << "[1,2,3]";
    }
    CHECK(!cascade::core::loadStore(path, none, err) && none.empty());
    {
        std::ofstream partial(path, std::ios::binary | std::ios::trunc);
        partial << R"({"plugins":[{"id":""},{"nope":1},{"id":"ok","strikes":3,"state":"disabled"},"x"]})";
    }
    CHECK(cascade::core::loadStore(path, none, err) && none.size() == 1 && none[0].id == "ok" && none[0].strikes == 3);

    // FOUND BY THE FUZZ TARGET `plugin_marker` (docs/engine-stage4.md 6.29): this file is read before anything else at every start, and a record whose
    // field had the wrong TYPE used to throw out of loadStore (nlohmann's value() converts nothing) - an uncaught exception at start-up, every
    // start-up, until a person found the file. A wrong type is a default; the record, and the plugin it names, stay.
    {
        std::ofstream typed(path, std::ios::binary | std::ios::trunc);
        typed << R"({"plugins":[{"id":"typed","strikes":"many","state":3,"when":"yesterday","site":"yes","version":7,"phase":[1]}]})";
    }
    CHECK(cascade::core::loadStore(path, none, err) && none.size() == 1 && none[0].id == "typed");
    if (none.size() == 1) {
        CHECK(none[0].strikes == 1 && none[0].state == StrikeState::Suspect && none[0].whenUnix == 0 && !none[0].fromSite);
        CHECK(none[0].version.empty() && none[0].phase.empty());
    }
    // numbers outside the type's range are a default too (the conversions are not undefined), and a count is never zero or absurd
    {
        std::ofstream big(path, std::ios::binary | std::ios::trunc);
        big << R"({"plugins":[{"id":"big","strikes":4294967296,"when":9223372036854775808},{"id":"neg","strikes":-5,"when":-1},{"id":"flt","strikes":1.5e300,"when":1e300}]})";
    }
    CHECK(cascade::core::loadStore(path, none, err) && none.size() == 3);
    for (const StrikeRecord& r : none) { CHECK(r.strikes >= 1 && r.strikes <= 1000); }
    if (none.size() == 3) { CHECK(none[1].whenUnix == -1); }
    // a plugin named twice (a hand-edited file) is ONE record, with the worse of what the two say
    {
        std::ofstream twice(path, std::ios::binary | std::ios::trunc);
        twice << R"({"plugins":[{"id":"x","strikes":1,"state":"probation","version":"1.0.0"},{"id":"x","strikes":2,"state":"disabled","version":"2.0.0"},{"id":"y"}]})";
    }
    CHECK(cascade::core::loadStore(path, none, err) && none.size() == 2);
    if (none.size() == 2) {
        CHECK(none[0].id == "x" && none[0].strikes == 2 && none[0].state == StrikeState::Disabled);
        CHECK(none[1].id == "y");
    }
    // an empty store is no file
    CHECK(cascade::core::saveStore(path, {}, err));
    CHECK(!fs::exists(path));
}

// ---- E ----------------------------------------------------------------------------------------------------------------------------
void testContainment(const fs::path& dir) {
    const std::string settings = (dir / "settings").string();
    fs::create_directories(settings);
    marker::resetForTest();

    // RUN ONE: a first start, clean; the plugin is mapped and then dies inside create()
    {
        Containment c;
        c.begin(settings, false, 100);
        CHECK(c.started() && !c.safeMode() && c.notices().empty());
        Containment::Gate g;
        CHECK(!c.gate("bad-1.0.0-abi3-win-x64.dll", g));
        marker::set("bad-1.0.0-abi3-win-x64.dll", Phase::Load, "1.0.0");
        marker::set("bad", Phase::Idle);
        marker::set("bad", Phase::Create);
        // (no cleanShutdown: the process died)
    }
    marker::resetForTest();

    // RUN TWO: the gate refuses it, by file name, with the sentence; others pass; one notice
    {
        Containment c;
        c.begin(settings, false, 200);
        Containment::Gate g;
        CHECK(c.gate("bad-1.0.0-abi3-win-x64.dll", g));
        CHECK(g.verdict == cascade::core::ModuleVerdict::Faulted);
        CHECK(g.sentence.rfind("Not loaded: ", 0) == 0 && g.sentence.find("RE-ENABLE") != std::string::npos);
        CHECK(c.gate("BAD-2.0.0-abi3-win-x64.dll", g));          // a newer file of the same plugin is the same plugin
        CHECK(!c.gate("good-1.0.0-abi3-win-x64.dll", g));
        CHECK(c.notices().size() == 1 && c.notices()[0].id == "bad" && !c.notices()[0].permanent);
        CHECK(c.recordOf("bad") != nullptr && c.recordOf("bad")->state == StrikeState::Suspect);
        CHECK(fs::exists(fs::path(settings) / cascade::core::kStoreFileName));
        // the person's one click
        CHECK(c.reenable("bad-1.0.0-abi3-win-x64.dll"));
        CHECK(!c.gate("bad-1.0.0-abi3-win-x64.dll", g));
        CHECK(c.recordOf("bad")->state == StrikeState::Probation && c.recordOf("bad")->strikes == 1);
        CHECK(!c.reenable("bad"));          // already enabled
        CHECK(!c.reenable("never-heard"));
        c.cleanShutdown();                   // an orderly stop in which it did not run: the strike stays
        CHECK(c.recordOf("bad") != nullptr);
    }
    marker::resetForTest();

    // RUN THREE: probation, it loads; it runs clean; the orderly stop takes the strike off
    {
        Containment c;
        c.begin(settings, false, 300);
        Containment::Gate g;
        CHECK(!c.gate("bad", g));
        CHECK(c.notices().empty());
        marker::set("bad", Phase::FirstPass);
        marker::promoteFirstPass(0xFFFFFFFFu, 0);   // (the clock: far enough)
        CHECK(marker::phaseOf("bad") == Phase::Running);
        c.cleanShutdown();
        CHECK(c.recordOf("bad") == nullptr);
        CHECK(!fs::exists(fs::path(settings) / cascade::core::kStoreFileName));   // an empty store is no file
    }
    marker::resetForTest();

    // SAFE MODE: everything refused with its own verdict, and the records are neither read as news nor changed
    {
        Containment c;
        c.begin(settings, false, 400);
        marker::set("worse", Phase::Create);
    }
    marker::resetForTest();
    const std::string recordsBefore = readAll(fs::path(settings) / cascade::core::kStoreFileName);
    {
        Containment c;
        c.begin(settings, true, 500);
        CHECK(c.safeMode());
        Containment::Gate g;
        CHECK(c.gate("anything-1.0.0-abi3-win-x64.dll", g) && g.verdict == cascade::core::ModuleVerdict::SafeMode);
        CHECK(g.sentence.find("safe mode") != std::string::npos);
        CHECK(c.notices().empty());
        c.cleanShutdown();
    }
    CHECK(readAll(fs::path(settings) / cascade::core::kStoreFileName) == recordsBefore);   // unchanged, byte for byte
    marker::resetForTest();
    // ...and the next normal start still finds the crash that safe mode did not consume? NO: safe mode opened the marker for ITS run, which
    // reset it. What the dead run left is judged only by the start that follows it; a person who starts in safe mode has chosen to look first.
    {
        Containment c;
        c.begin(settings, false, 600);
        CHECK(c.notices().empty());
    }
    marker::resetForTest();

    // A HERMETIC RUN (no folder): nothing read, nothing kept
    {
        Containment c;
        c.begin(std::string(), false, 700);
        Containment::Gate g;
        CHECK(!c.gate("bad", g) && c.notices().empty());
        CHECK(!marker::persistent());
        c.cleanShutdown();
    }
    marker::resetForTest();
}

// ---- G ----------------------------------------------------------------------------------------------------------------------------
void testContainmentIdIsOneId(const fs::path& dir) {
    namespace cc = cascade::core;
    CHECK(cc::containmentId("Acme-Decoder-1.0.0-abi3-win-x64.dll") == "acme-decoder");
    CHECK(cc::containmentId("acme-decoder") == "acme-decoder");
    CHECK(cc::containmentId(cc::containmentId("ACME-Decoder-2.dll")) == cc::containmentId("ACME-Decoder-2.dll"));   // idempotent, as pluginRunId is
    CHECK(cc::containmentId("").empty());
    CHECK(cc::containmentId(std::string(60, 'q')).size() == cc::kContainmentIdMax);
    CHECK(cc::containmentId(std::string(47, 'q')).size() == 47);
    // the marker's slot holds this very form: a plugin that died inside create() comes back from the file under it
    const std::string leaf = "Acme-Very-Long-Decoder-Name-For-A-Satellite-Modem-Pipeline-1.2.3-abi3-win-x64.dll";
    CHECK(cc::pluginRunId(leaf).size() > cc::kContainmentIdMax);
    const std::string path = (dir / "oneid.marker").string();
    marker::resetForTest();
    marker::open(path);
    marker::set(leaf, Phase::Create, "1.2.3");
    marker::resetForTest();
    const std::vector<InPlay> before = marker::open(path);
    CHECK(before.size() == 1 && before[0].id == cc::containmentId(leaf));
    marker::cleanShutdown();
    marker::resetForTest();
}

// A PLUGIN WHOSE ID IS LONGER THAN A SLOT IS FOUND AGAIN (batch A). The marker's slot and the strike record hold the id lower-cased and cut to 47 characters; the
// gate and the re-enable looked the plugin up by the full lower-cased id, which is never equal to a cut one - so for a plugin installed from a site under a long
// file name (the installer allows 128 characters) the breaker NEVER matched: it crashed the application in create() on every start, got its strikes, and was
// loaded again at every start after - the loop the breaker exists to end.
void testLongAndMixedCaseIdsAreFoundAgain(const fs::path& dir) {
    const std::string settings = (dir / "settings-long").string();
    fs::create_directories(settings);
    const std::string leaf = "Acme-Very-Long-Decoder-Name-For-A-Satellite-Modem-Pipeline-1.2.3-abi3-win-x64.dll";
    const std::string newer = "ACME-VERY-LONG-DECODER-NAME-FOR-A-SATELLITE-MODEM-PIPELINE-9.9.9-abi3-win-x64.dll";
    marker::resetForTest();
    {
        Containment c;
        c.begin(settings, false, 100);
        marker::set(leaf, Phase::Create, "1.2.3");   // (no cleanShutdown: the process died inside create())
    }
    marker::resetForTest();
    {
        Containment c;
        c.begin(settings, false, 200);
        CHECK(c.notices().size() == 1);
        Containment::Gate g;
        const bool refused = c.gate(leaf, g);
        if (!refused) { std::printf("FAIL the breaker did not match a plugin with a long id\n"); }
        CHECK(refused);
        CHECK(g.verdict == cascade::core::ModuleVerdict::Faulted);
        CHECK(c.gate(newer, g));   // a newer file of the same plugin, written in capitals, is the same plugin
        CHECK(c.recordOf(leaf) != nullptr);
        if (c.recordOf(leaf) != nullptr) { CHECK(c.recordOf(leaf)->state == StrikeState::Suspect); }
        CHECK(c.recordOf(newer) == c.recordOf(leaf));
        CHECK(c.reenable(leaf));   // and the person's click finds it too
        CHECK(!c.gate(leaf, g));
        CHECK(c.recordOf(leaf) != nullptr);
        if (c.recordOf(leaf) != nullptr) { CHECK(c.recordOf(leaf)->state == StrikeState::Probation && c.recordOf(leaf)->strikes == 1); }
    }
    marker::resetForTest();
    {   // the record was kept in the store under the cut id, and the next start reads it back
        Containment c;
        c.begin(settings, false, 300);
        CHECK(c.recordOf(leaf) != nullptr);
        c.cleanShutdown();
    }
    marker::resetForTest();
}

// BRACKETS THAT OVERLAP (batch A). The runner (the engine's thread) and the plugin UI (the window's) can both be building instances of one plugin - the bounded
// handshake that keeps them apart lapses when the window is minimised and a create() is slow - and two destroys of one plugin can overlap the same way. Each
// bracket saved the phase it found and put it back: the second saved the FIRST one's in-call phase, and whichever ended last restored that - the plugin was left
// in `create` or `unload` for good, so a later crash of the application for its own reasons put a strike on a plugin that was not in any call.
void testOverlappingBracketsSettleWhereThePluginRests(const fs::path& dir) {
    const std::string path = (dir / "overlap.marker").string();
    marker::resetForTest();
    marker::open(path);
    // two destroys overlap: the plugin was running, and it is running again when both are over
    marker::set("ov-a", Phase::Running);
    {
        auto a = std::make_unique<marker::CallPhase>("ov-a", Phase::Unload);
        auto b = std::make_unique<marker::CallPhase>("ov-a", Phase::Unload);
        CHECK(marker::phaseOf("ov-a") == Phase::Unload);
        a.reset();
        if (marker::phaseOf("ov-a") != Phase::Unload) { std::printf("FAIL the first bracket to end put the plugin out of its call while the second is still inside it (reads %s)\n", marker::phaseWord(marker::phaseOf("ov-a"))); }
        CHECK(marker::phaseOf("ov-a") == Phase::Unload);   // b is still inside its call: the plugin is still in play
        b.reset();
        if (marker::phaseOf("ov-a") != Phase::Running) { std::printf("FAIL after two overlapping destroys the plugin reads %s\n", marker::phaseWord(marker::phaseOf("ov-a"))); }
        CHECK(marker::phaseOf("ov-a") == Phase::Running);
    }
    // the same in the other order
    marker::set("ov-b", Phase::Running);
    {
        auto a = std::make_unique<marker::CallPhase>("ov-b", Phase::Unload);
        auto b = std::make_unique<marker::CallPhase>("ov-b", Phase::Unload);
        b.reset();
        CHECK(marker::phaseOf("ov-b") == Phase::Unload);
        a.reset();
        CHECK(marker::phaseOf("ov-b") == Phase::Running);
    }
    // two builds overlap and ONE of them made an instance: it rests in first-pass whichever ends first, and with none made it rests where it was
    {
        auto a = std::make_unique<marker::CallPhase>("ov-c", Phase::Create, "1.0.0");
        auto b = std::make_unique<marker::CallPhase>("ov-c", Phase::Create, "1.0.0");
        a->settle(Phase::FirstPass);
        a.reset();
        CHECK(marker::phaseOf("ov-c") == Phase::Create);
        b.reset();
        CHECK(marker::phaseOf("ov-c") == Phase::FirstPass);
        auto c = std::make_unique<marker::CallPhase>("ov-d", Phase::Create);
        auto d = std::make_unique<marker::CallPhase>("ov-d", Phase::Create);
        d->settle(Phase::FirstPass);
        c.reset();
        d.reset();
        CHECK(marker::phaseOf("ov-d") == Phase::FirstPass);
        auto e = std::make_unique<marker::CallPhase>("ov-e", Phase::Create);
        e.reset();   // made nothing: back to a loaded module's idle slot
        CHECK(marker::phaseOf("ov-e") == Phase::Idle);
    }
    // a plain bracket on its own is what it always was
    marker::set("ov-f", Phase::FirstPass);
    {
        marker::CallPhase one("ov-f", Phase::Unload);
        CHECK(marker::phaseOf("ov-f") == Phase::Unload);
    }
    CHECK(marker::phaseOf("ov-f") == Phase::FirstPass);
    // the first-pass clock still runs for a plugin that RESTS in first-pass after a bracket
    {
        marker::CallPhase one("ov-g", Phase::Create);
        one.settle(Phase::FirstPass);
    }
    CHECK(marker::phaseOf("ov-g") == Phase::FirstPass);
    marker::promoteFirstPass(0xFFFFFFFFu, 0);
    CHECK(marker::phaseOf("ov-g") == Phase::Running);
    marker::cleanShutdown();
    marker::resetForTest();
}

// TWO SETTINGS FILES IN ONE FOLDER (batch A). The config lock is per settings file, so `foxsdr-engine --config a.json` beside `--config b.json` - the engine's own
// refusal text advises exactly that - each hold their own lock, and they shared ONE marker and ONE store: the second start read the first's live slots as a dead
// predecessor's evidence (a strike for a plugin running fine) and zeroed them (so the first's own later crash had no blame). The pair is named by the settings file.
void testSettingsFilesKeepTheirOwnMarker(const fs::path& dir) {
    namespace cc = cascade::core;
    CHECK(cc::markerFileNameFor("") == "plugin-play.marker" && cc::storeFileNameFor("") == "plugin-faults.json");
    CHECK(cc::markerFileNameFor("C:\\Users\\x\\config.json") == "plugin-play.marker");   // the default settings file keeps the names it has always had
    CHECK(cc::markerFileNameFor("/home/x/CONFIG.JSON") == "plugin-play.marker");
    CHECK(cc::markerFileNameFor("config.json") == "plugin-play.marker");
    CHECK(cc::markerFileNameFor("D:/data/Second.json") == "plugin-play-second.marker");
    CHECK(cc::storeFileNameFor("D:/data/Second.json") == "plugin-faults-second.json");
    CHECK(cc::markerFileNameFor("we ird*.json") == "plugin-play-we_ird_.marker");
    CHECK(cc::markerFileNameFor("a.cfg") == "plugin-play-a.cfg.marker");   // a name that is not .json keeps its extension: a.cfg and a.json differ
    CHECK(cc::markerFileNameFor(std::string(100, 'z') + ".json").size() <= std::string("plugin-play-.marker").size() + 40);

    const std::string settings = (dir / "settings-two").string();
    fs::create_directories(settings);
    marker::resetForTest();
    {
        Containment a;
        a.begin(settings, false, 100, "a.json");
        marker::set("bad", Phase::Create, "1.0.0");   // a.json's program dies inside create()
    }
    marker::resetForTest();
    {
        Containment b;
        b.begin(settings, false, 150, "b.json");   // b.json's program starts in the same folder
        if (!b.notices().empty()) { std::printf("FAIL a second settings file read the first one's marker as its own\n"); }
        CHECK(b.notices().empty());
    }
    marker::resetForTest();
    {
        Containment a;
        a.begin(settings, false, 200, "a.json");   // and a.json's own next start still finds its own evidence
        CHECK(a.notices().size() == 1);
        Containment::Gate g;
        CHECK(a.gate("bad-1.0.0-abi3-win-x64.dll", g));
    }
    CHECK(fs::exists(fs::path(settings) / "plugin-play-a.marker") && fs::exists(fs::path(settings) / "plugin-play-b.marker"));
    CHECK(fs::exists(fs::path(settings) / "plugin-faults-a.json"));
    CHECK(!fs::exists(fs::path(settings) / "plugin-faults-b.json"));   // b has no strikes
    CHECK(!fs::exists(fs::path(settings) / "plugin-play.marker") && !fs::exists(fs::path(settings) / "plugin-faults.json"));
    marker::resetForTest();
}

#if defined(_WIN32)
// THE MARKER'S PAGES DO NOT GO WITH A TRIMMED WORKING SET (batch A). The marker is a file mapping that is stored into from inside a plugin's create(), outside any
// guard. When the settings folder is on a network share or a removable drive that goes away, a page that memory pressure had trimmed from the working set is read
// back in by the next store - and an in-page error there is a fatal crash of the host, with nothing to attribute it to a plugin. The mapping is locked into the
// working set when it is made (8 KiB): there is nothing to page back in. (A disconnect cannot be staged here; the premise of the failure - a trim - can.)
void testMarkerPagesStayResident(const fs::path& dir) {
    marker::resetForTest();
    marker::open((dir / "resident.marker").string());
    marker::set("resident", Phase::Running);
    const void* view = marker::viewForTest();
    CHECK(view != nullptr);
    if (view != nullptr) {
        ::EmptyWorkingSet(::GetCurrentProcess());   // what memory pressure does to a process's pages
        PSAPI_WORKING_SET_EX_INFORMATION info[2] = {};
        info[0].VirtualAddress = const_cast<void*>(view);
        info[1].VirtualAddress = static_cast<char*>(const_cast<void*>(view)) + 4096;
        CHECK(::QueryWorkingSetEx(::GetCurrentProcess(), info, sizeof(info)) != 0);
        const bool first = info[0].VirtualAttributes.Valid != 0;
        const bool second = info[1].VirtualAttributes.Valid != 0;
        if (!first || !second) { std::printf("FAIL the marker's pages left the working set when it was trimmed (page 0 %d, page 1 %d)\n", first ? 1 : 0, second ? 1 : 0); }
        CHECK(first && second);
        // and a store still lands
        marker::set("resident", Phase::Create);
        CHECK(marker::phaseOf("resident") == Phase::Create);
    }
    marker::cleanShutdown();
    marker::resetForTest();
}
#endif

}  // namespace

// THE START'S LINES NAME A PLUGIN BY THE NAME THAT MAY LEAVE THE MACHINE (the plausible finding P1 of batch A; docs/engine-stage4.md 6.31). The log ring is copied into every
// report, and the lines the start writes about a dead run's suspect - "the last run did not end in order with X in play", "X starts the next session switched off" - named X
// by its file's id: a stranger's private plugin, in a report that is uploaded. The marker says where each plugin came from; an official one keeps its own name, any other is
// `site-` and a hash.
void testStartLogLinesNameThePublicId(const fs::path& dir) {
    namespace cc = cascade::core;
    const fs::path settings = dir / "p1-start";
    fs::create_directories(settings);
    marker::resetForTest();
    cc::DiagLog::instance().resetForTest();
    {
        Containment first;
        first.begin(settings.string(), false, 1000);
        // two plugins die inside create(): a stranger's, and one of FoxSDR's own
        marker::set("acme-private-crasher-1.0.0-abi3-win-x64.dll", Phase::Create, "1.0.0", cc::fault::Origin::Site);
        marker::set("pocsag-decoder-1.0.2-abi3-win-x64.dll", Phase::Create, "1.0.2", cc::fault::Origin::Official);
    }
    marker::resetForTest();   // (the process "ended" without an orderly stop: the marker is left as it was)
    cc::DiagLog::instance().resetForTest();
    {
        Containment second;
        second.begin(settings.string(), false, 2000);
        CHECK(second.notices().size() == 2);
    }
    const std::string hashed = cc::fault::publicIdFor(cc::containmentId("acme-private-crasher-1.0.0-abi3-win-x64.dll"), cc::fault::Origin::Site);
    CHECK(hashed.rfind("site-", 0) == 0);
    bool order = false;
    bool switchedOff = false;
    bool officialNamed = false;
    for (const std::string& line : cc::DiagLog::instance().ringSnapshot()) {
        if (line.find("acme-private-crasher") != std::string::npos) { std::printf("FAIL the start names a private plugin by its id: %s\n", line.c_str()); }
        CHECK(line.find("acme-private-crasher") == std::string::npos);
        order = order || (line.find("did not end in order") != std::string::npos && line.find(hashed) != std::string::npos);
        switchedOff = switchedOff || (line.find("starts the next session switched off") != std::string::npos && line.find(hashed) != std::string::npos);
        officialNamed = officialNamed || (line.find("pocsag-decoder") != std::string::npos);
    }
    CHECK(order);
    CHECK(switchedOff);
    CHECK(officialNamed);   // an official plugin keeps its own name
    marker::resetForTest();
}

int main() {
    const fs::path dir = fs::temp_directory_path() / ("foxsdr-test-plugin-marker-" + std::to_string(static_cast<unsigned long long>(
#if defined(_WIN32)
                                                                                           ::GetCurrentProcessId()
#else
                                                                                           1
#endif
                                                                                               )));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);

    testSuspects();
    testStrikes();
    testMarkerFile(dir);
    testStore(dir);
    testContainment(dir);
    testContainmentIdIsOneId(dir);
    testLongAndMixedCaseIdsAreFoundAgain(dir);
    testOverlappingBracketsSettleWhereThePluginRests(dir);
    testSettingsFilesKeepTheirOwnMarker(dir);
    testStartLogLinesNameThePublicId(dir);
#if defined(_WIN32)
    testMarkerPagesStayResident(dir);
#endif

    fs::remove_all(dir, ec);
    return testSummary("test_plugin_marker");
}
