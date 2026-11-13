// test_engine_api_wire_a_scan.cpp - THE SCANNER GROUP OF WIRING LANE wire-a, held against the real engine on its generator source through the API (no window): the real Scanner walks the real
// plan, the engine tunes the radio, and the test reads where the radio is (fact.tuned.1.hz / .mode / .bandwidthHz) as the scan moves.
//
//   S1  SCAN PLAN: two ranges (AM 25 kHz steps, NFM 12.5 kHz steps) are scanned in order, each stop in its range's mode and bandwidth; the same plan backward visits the points in reverse; SCAN_BACK steps back
//       and carries on from there; a plan that is refused (bad JSON, a range with a stop below its start, a version) changes nothing; clear; the plan reaches scan-plan.json and a new engine reads it.
//   S2  SCAN TRIGGER: with the squelch always open a scan would hold on its first point; with the trigger on at a level nothing reaches it keeps scanning; with the trigger on at a level everything reaches
//       it holds although the squelch is shut; off again, the squelch decides.
//   S3  SCANNER EXTRAS: a locked-out point is never visited; the operator's lockout is cleared by its op; a per-entry threshold holds the scan on that entry only; priority and mute facts; bad values refused.
//   S4  SAVE HIT: a point becomes a bookmark in the group "Scan hits" with the range's mode and bandwidth; again is NO_CHANGE.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cmath>
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "wire_a_rig.hpp"

using namespace wire_a_rig;

namespace {

fs::path g_dir;

const char* kPlan =
    R"({"version":1,"direction":"%DIR%","trigger":{"version":1,"mode":"relative","fixedDb":-70,"marginDb":8,"hysteresisDb":2,"riseDbPerReading":0.1},)"
    R"("ranges":[{"startHz":100000000,"stopHz":100075000,"stepHz":25000,"mode":"AM","bandwidthHz":10000,"label":"A"},)"
    R"({"startHz":144000000,"stopHz":144025000,"stepHz":12500,"mode":"NFM","bandwidthHz":12500,"label":"B"}]})";

std::string plan(const char* dir) {
    std::string s = kPlan;
    s.replace(s.find("%DIR%"), 5, dir);
    return s;
}

const double kPoints[7] = {100.0e6, 100.025e6, 100.05e6, 100.075e6, 144.0e6, 144.0125e6, 144.025e6};

double hzNow(Rig& r) {
    const std::string f = r.fact("tuned.1.hz");
    return f == "<absent>" ? 0.0 : std::strtod(f.c_str(), nullptr);
}

// The places the radio is at, in order, each once, over `ms` of passes.
std::vector<double> visited(Rig& r, int ms) {
    std::vector<double> v;
    r.until([&] {
        const double hz = hzNow(r);
        if (hz > 0.0 && (v.empty() || std::fabs(v.back() - hz) > 0.5)) { v.push_back(hz); }
        return false;
    }, ms);
    return v;
}

bool nearHz(double a, double b) { return std::fabs(a - b) < 2.0; }

FoxCommand scanCfg(double dwellMs) {
    FoxCommand c = testing::cmd(FOXAPI_OP_SCANNER_CONFIG, dwellMs);
    c.num[1] = 400.0;   // hold
    c.num[2] = 400.0;   // resume
    c.num[3] = 0.0;
    return c;
}

void startPlan(Rig& r) {
    CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_SCAN_PLAN, 0.0, 1)).status(), FOXAPI_OK);
}

void testPlan() {
    std::printf("S1: scan plan\n");
    const fs::path user = g_dir / "user1";
    {
        Rig r(user);
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RUN, 0, 1)).status(), FOXAPI_OK);
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SET_SQUELCH, 20.0)).status(), FOXAPI_OK);   // never open: the scan keeps moving
        CHECK_EQ(r.send(scanCfg(40.0)).status(), FOXAPI_OK);
        CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_SCAN_PLAN, 0.0, 1)).status(), FOXAPI_NO_CHANGE);   // no plan yet
        // refusals first: they change nothing
        const std::string notJson = "{nope", wrongVersion = R"({"version":2,"direction":"forward","trigger":{},"ranges":[]})";
        std::string reversed = plan("forward");
        reversed.replace(reversed.find("100075000"), 9, "99000000");
        CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_SCAN_PLAN, 0.0, 0), &notJson).status(), FOXAPI_BAD_ARGUMENT);
        CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_SCAN_PLAN, 0.0, 0), &wrongVersion).status(), FOXAPI_BAD_ARGUMENT);
        CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_SCAN_PLAN, 0.0, 0), &reversed).status(), FOXAPI_BAD_ARGUMENT);
        CHECK_EQ(r.fact("scan.plan"), std::string());
        CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_SCAN_PLAN, 0.0, 7)).status(), FOXAPI_OUT_OF_RANGE);
        // forward
        const std::string fwd = plan("forward");
        CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_SCAN_PLAN, 0.0, 0), &fwd).status(), FOXAPI_OK);
        CHECK(!r.fact("scan.plan").empty());
        startPlan(r);
        CHECK_EQ(r.fact("scan.plan.active"), std::string("1"));
        std::vector<double> v = visited(r, 1500);
        CHECK(v.size() >= 7u);
        for (std::size_t i = 0; i < 7 && i < v.size(); ++i) { CHECK(nearHz(v[i], kPoints[i])); }
        // a stop in each range carries the range's mode and bandwidth: stop on one and read
        // (the scan is moving: ask for the facts at a moment the radio is on an A point, then a B point)
        bool sawAm = false, sawNfm = false;
        r.until([&] {
            const double hz = hzNow(r);
            if (hz < 110e6 && r.fact("tuned.1.mode") == "AM" && r.fact("tuned.1.bandwidthHz") == "10000") { sawAm = true; }
            if (hz > 140e6 && r.fact("tuned.1.mode") == "NFM" && r.fact("tuned.1.bandwidthHz") == "12500") { sawNfm = true; }
            return sawAm && sawNfm;
        }, 3000);
        CHECK(sawAm && sawNfm);
        // SCAN_BACK: carry on from the point before the one the scan is on
        testing::Outcome o = r.send(testing::cmd(FOXAPP_OP_SCAN_BACK));
        CHECK_EQ(o.status(), FOXAPI_OK);
        const double target = o.result.applied[0];
        bool isPoint = false;
        for (const double p : kPoints) { isPoint = isPoint || nearHz(p, target); }
        CHECK(isPoint);
        v = visited(r, 600);
        CHECK(!v.empty() && nearHz(v.front(), target) || (v.size() > 1 && nearHz(v[1], target)));
        // the plan reaches the file
        CHECK(r.until([&] { return fs::exists(user / "scan-plan.json"); }, 6000));
        // stop with the existing op; plan mode ends when another scan starts
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SCANNER_RUN, 0, 0)).status(), FOXAPI_OK);
        CHECK_EQ(r.fact("scan.plan.active"), std::string("0"));
        CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_SCAN_BACK)).status(), FOXAPI_NO_CHANGE);   // nothing running
        // backward
        const std::string bwd = plan("backward");
        CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_SCAN_PLAN, 0.0, 0), &bwd).status(), FOXAPI_OK);
        startPlan(r);
        v = visited(r, 1500);
        CHECK(v.size() >= 7u);
        for (std::size_t i = 0; i < 7 && i < v.size(); ++i) { CHECK(nearHz(v[i], kPoints[6 - i])); }
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SCANNER_RUN, 0, 0)).status(), FOXAPI_OK);
        // clear
        CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_SCAN_PLAN, 0.0, 2)).status(), FOXAPI_OK);
        CHECK_EQ(r.fact("scan.plan"), std::string());
        CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_SCAN_PLAN, 0.0, 2)).status(), FOXAPI_NO_CHANGE);
        // set a plan again so the file holds one
        CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_SCAN_PLAN, 0.0, 0), &fwd).status(), FOXAPI_OK);
        r.settle(900);
    }
    {
        Rig r(user);
        r.settle(200);
        CHECK(r.fact("scan.plan").find("\"ranges\"") != std::string::npos);   // a new engine reads the plan back
    }
}

void testTrigger() {
    std::printf("S2: scan trigger\n");
    Rig r;
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RUN, 0, 1)).status(), FOXAPI_OK);
    CHECK_EQ(r.send(scanCfg(40.0)).status(), FOXAPI_OK);
    const std::string fwd = plan("forward");
    CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_SCAN_PLAN, 0.0, 0), &fwd).status(), FOXAPI_OK);
    // the squelch always open: the scan holds on its first point
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SET_SQUELCH, -200.0)).status(), FOXAPI_OK);
    startPlan(r);
    std::vector<double> v = visited(r, 1200);
    CHECK_EQ(v.size(), std::size_t(1));
    // trigger on, fixed at +20 dB: nothing reaches it, so the scan keeps moving though the squelch is open
    const std::string never = R"({"version":1,"mode":"fixed","fixedDb":20,"marginDb":8,"hysteresisDb":2,"riseDbPerReading":0.1})";
    CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_SCAN_TRIGGER, 0.0, 1), &never).status(), FOXAPI_OK);
    CHECK_EQ(r.fact("scan.trigger.on"), std::string("1"));
    CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_SCAN_TRIGGER, 0.0, 1)).status(), FOXAPI_NO_CHANGE);   // on already
    startPlan(r);
    v = visited(r, 1200);
    CHECK(v.size() >= 4u);
    // trigger fixed at -160 dB: everything reaches it, so the scan holds though the squelch is shut
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SET_SQUELCH, 20.0)).status(), FOXAPI_OK);
    const std::string always = R"({"version":1,"mode":"fixed","fixedDb":-160,"marginDb":8,"hysteresisDb":2,"riseDbPerReading":0.1})";
    CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_SCAN_TRIGGER, 0.0, 1), &always).status(), FOXAPI_OK);
    startPlan(r);
    v = visited(r, 1200);
    CHECK_EQ(v.size(), std::size_t(1));
    // off: the squelch (shut) decides, the scan moves
    CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_SCAN_TRIGGER, 0.0, 0)).status(), FOXAPI_OK);
    startPlan(r);
    v = visited(r, 1200);
    CHECK(v.size() >= 4u);
    // refusals
    const std::string bad = R"({"version":1,"mode":"fixed","fixedDb":500,"marginDb":8,"hysteresisDb":2,"riseDbPerReading":0.1})";
    CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_SCAN_TRIGGER, 0.0, 1), &bad).status(), FOXAPI_BAD_ARGUMENT);
    CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_SCAN_TRIGGER, 0.0, 5)).status(), FOXAPI_OUT_OF_RANGE);
    CHECK_EQ(r.fact("scan.trigger.on"), std::string("0"));
}

void testExtras() {
    std::printf("S3: scanner extras\n");
    Rig r;
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RUN, 0, 1)).status(), FOXAPI_OK);
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SET_SQUELCH, 20.0)).status(), FOXAPI_OK);
    CHECK_EQ(r.send(scanCfg(40.0)).status(), FOXAPI_OK);
    CHECK_EQ(r.fact("scanner.extras"), std::string("1"));
    const std::string fwd = plan("forward");
    CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_SCAN_PLAN, 0.0, 0), &fwd).status(), FOXAPI_OK);
    // a configured lockout: 100.05 MHz is never visited
    FoxCommand locked = testing::cmd(FOXAPP_OP_SCANNER_EXTRAS, 0.0, 5);
    const std::string lockText = "100050000";
    CHECK_EQ(r.send(locked, &lockText).status(), FOXAPI_OK);
    CHECK_EQ(r.fact("scan.locked"), std::string("100050000.000"));
    startPlan(r);
    std::vector<double> v = visited(r, 1500);
    CHECK(v.size() >= 5u);
    for (const double hz : v) { CHECK(!nearHz(hz, 100.05e6)); }
    bool sawNext = false;
    for (const double hz : v) { sawNext = sawNext || nearHz(hz, 100.075e6); }
    CHECK(sawNext);
    // the operator's lockout of 144.0125 MHz, then cleared
    FoxCommand lo = testing::cmd(FOXAPP_OP_SCANNER_EXTRAS, 144.0125e6, 1);
    CHECK_EQ(r.send(lo).status(), FOXAPI_OK);
    v = visited(r, 1500);
    for (const double hz : v) { CHECK(!nearHz(hz, 144.0125e6)); }
    CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_SCANNER_EXTRAS, 0.0, 2)).status(), FOXAPI_OK);
    v = visited(r, 1500);
    bool sawLocked = false;
    for (const double hz : v) { sawLocked = sawLocked || nearHz(hz, 144.0125e6); }
    CHECK(sawLocked);
    // a per-entry threshold: the entry at 144.0 MHz counts as present at -200 dB, so the scan holds there only
    const std::string th = "144000000\t-200\n";
    CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_SCANNER_EXTRAS, 0.0, 4), &th).status(), FOXAPI_OK);
    CHECK_EQ(r.fact("scan.thresholds"), std::string("1"));
    v = visited(r, 1500);
    CHECK(!v.empty() && nearHz(v.back(), 144.0e6));
    // clear the thresholds; priority, mute and bad values
    const std::string none;
    CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_SCANNER_EXTRAS, 0.0, 4), &none).status(), FOXAPI_OK);
    CHECK_EQ(r.fact("scan.thresholds"), std::string("0"));
    FoxCommand pri = testing::cmd(FOXAPP_OP_SCANNER_EXTRAS, 500.0, 0);
    const std::string priText = "162550000;162400000";
    CHECK_EQ(r.send(pri, &priText).status(), FOXAPI_OK);
    CHECK_EQ(r.fact("scan.priority"), std::string("162550000.000;162400000.000"));
    CHECK_EQ(r.fact("scan.priority.interval"), std::string("500"));
    v = visited(r, 2500);
    bool sawPriority = false;
    for (const double hz : v) { sawPriority = sawPriority || nearHz(hz, 162.55e6) || nearHz(hz, 162.4e6); }
    CHECK(sawPriority);
    FoxCommand mute = testing::cmd(FOXAPP_OP_SCANNER_EXTRAS, 0.0, 3);
    mute.ival[1] = 1;
    CHECK_EQ(r.send(mute).status(), FOXAPI_OK);
    CHECK_EQ(r.fact("scan.mute"), std::string("1"));
    const std::string badHz = "abc", badTh = "144000000 -80\n", badTh2 = "144000000\t900\n";
    CHECK_EQ(r.send(pri, &badHz).status(), FOXAPI_BAD_ARGUMENT);
    CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_SCANNER_EXTRAS, 0.0, 4), &badTh).status(), FOXAPI_BAD_ARGUMENT);
    CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_SCANNER_EXTRAS, 0.0, 4), &badTh2).status(), FOXAPI_BAD_ARGUMENT);
    CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_SCANNER_EXTRAS, 0.0, 9)).status(), FOXAPI_OUT_OF_RANGE);
    FoxCommand badInterval = testing::cmd(FOXAPP_OP_SCANNER_EXTRAS, -5.0, 0);
    CHECK_EQ(r.send(badInterval, &priText).status(), FOXAPI_OUT_OF_RANGE);
    CHECK_EQ(r.fact("scan.priority.interval"), std::string("500"));   // refusals changed nothing
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SCANNER_RUN, 0, 0)).status(), FOXAPI_OK);
    // locking out the frequency the scan is on needs a running scan
    CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_SCANNER_EXTRAS, 0.0, 1)).status(), FOXAPI_NO_CHANGE);
}

void testSaveHit() {
    std::printf("S4: save a hit as a bookmark\n");
    Rig r;
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RUN, 0, 1)).status(), FOXAPI_OK);
    CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_SCAN_SAVE_HIT, 144.0125e6)).status(), FOXAPI_NO_CHANGE);   // no plan
    const std::string fwd = plan("forward");
    CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_SCAN_PLAN, 0.0, 0), &fwd).status(), FOXAPI_OK);
    testing::Outcome o = r.send(testing::cmd(FOXAPP_OP_SCAN_SAVE_HIT, 144.0125e6));
    CHECK_EQ(o.status(), FOXAPI_OK);
    const std::uint64_t id = r.bookmarkId(144.0125e6);
    CHECK(id != 0);
    FoxListItem it{};
    it.structSize = sizeof(it);
    const int32_t n = r.api()->read_list(r.local->raw(), FOXAPI_LIST_BOOKMARKS, 0, nullptr);
    bool found = false;
    for (int32_t i = 0; i < n; ++i) {
        r.api()->read_list(r.local->raw(), FOXAPI_LIST_BOOKMARKS, static_cast<uint32_t>(i), &it);
        if (it.id == id) {
            found = true;
            CHECK_EQ(std::string(it.detail), std::string("Scan hits"));
            CHECK(std::string(it.name).find("Scan 144.0125") == 0u);
            CHECK(std::fabs(it.value[2] - 12500.0) < 1.0);   // the NFM range's bandwidth
        }
    }
    CHECK(found);
    CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_SCAN_SAVE_HIT, 144.0125e6)).status(), FOXAPI_NO_CHANGE);   // there already
    // 0 = where the radio is: it is on the generator's own tune, not a plan point, so it is saved with the first range's mode
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SET_FREQUENCY, 100.025e6, 0)).status(), FOXAPI_OK);
    CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_SCAN_SAVE_HIT, 0.0)).status(), FOXAPI_OK);
    CHECK(r.bookmarkId(100.025e6) != 0);
}

}  // namespace

int main() {
    engine_api_test::hermetic("wire_a_scan");
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    g_dir = fs::temp_directory_path() / ("foxsdr_wire_a_scan_" + std::to_string(::_getpid()));
    std::error_code ec;
    fs::remove_all(g_dir, ec);
    fs::create_directories(g_dir, ec);
    testPlan();
    testTrigger();
    testExtras();
    testSaveHit();
    fs::remove_all(g_dir, ec);
    return check::finish("test_engine_api_wire_a_scan");
}
