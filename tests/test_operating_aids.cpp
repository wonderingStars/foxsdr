// Tests for the operating-aid models (core/operating_aids.hpp): frequency and step lock, band-stacking registers, VFO A/B with swap, copy and split,
// and the per-mode memory. Expected values are worked out by hand. The JSON loaders are fuzzed with mutated documents.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/operating_aids.hpp"
#include "test_check.hpp"

#include <cstdio>
#include <random>
#include <string>

namespace {
using namespace cascade::core::aids;
using nlohmann::json;

TuneState ts(double hz, const char* mode, double bw = 0.0) { TuneState s; s.hz = hz; s.mode = mode; s.bandwidthHz = bw; return s; }

void testTuneLock() {
    TuneLock l;
    // unlocked: everything valid passes
    for (int i = 0; i < kTuneSourceCount; ++i) {
        const TuneDecision d = l.requestFrequency(7.0e6, 7.1e6, static_cast<TuneSource>(i));
        CHECK(d.allowed && d.hz == 7.1e6 && d.reason.empty());
    }
    // locked, default policy: the hand-driven sources are blocked, the explicit ones pass
    l.setFrequencyLocked(true);
    const bool wantBlocked[kTuneSourceCount] = {true, true, true, true, true, true, false, false, false, false, false};
    int blocked = 0;
    for (int i = 0; i < kTuneSourceCount; ++i) {
        const TuneDecision d = l.requestFrequency(7.0e6, 7.1e6, static_cast<TuneSource>(i));
        CHECK(d.allowed == !wantBlocked[i]);
        CHECK(d.hz == (wantBlocked[i] ? 7.0e6 : 7.1e6));
        CHECK(d.reason == (wantBlocked[i] ? "frequency locked" : ""));
        if (wantBlocked[i]) { ++blocked; }
    }
    CHECK(blocked == 6);
    // invalid requests are refused whether locked or not
    for (bool locked : {false, true}) {
        l.setFrequencyLocked(locked);
        for (double bad : {-1.0, 1e14, std::nan(""), static_cast<double>(INFINITY), -static_cast<double>(INFINITY)}) {
            const TuneDecision d = l.requestFrequency(7.0e6, bad, TuneSource::Bookmark);
            CHECK(!d.allowed && d.hz == 7.0e6 && d.reason == "not a valid frequency");
        }
        CHECK(l.requestFrequency(7.0e6, 0.0, TuneSource::Bookmark).allowed);        // 0 Hz is a valid number
        CHECK(l.requestFrequency(7.0e6, 1e13, TuneSource::Bookmark).allowed);       // the upper bound is inclusive
    }
    // a changed policy: lock everything, then nothing
    l.setFrequencyLocked(true);
    LockPolicy all; all.blocked.fill(true);
    l.setPolicy(all);
    for (int i = 0; i < kTuneSourceCount; ++i) { CHECK(!l.requestFrequency(1e6, 2e6, static_cast<TuneSource>(i)).allowed); }
    LockPolicy none; none.blocked.fill(false);
    l.setPolicy(none);
    for (int i = 0; i < kTuneSourceCount; ++i) { CHECK(l.requestFrequency(1e6, 2e6, static_cast<TuneSource>(i)).allowed); }
    // an out-of-range source value never indexes out of bounds
    CHECK(l.requestFrequency(1e6, 2e6, static_cast<TuneSource>(99)).allowed);
    CHECK(l.requestFrequency(1e6, 2e6, static_cast<TuneSource>(-1)).allowed);

    // step lock
    TuneLock s;
    CHECK(s.requestStep(1000, 5000).allowed && s.requestStep(1000, 5000).stepHz == 5000);
    for (double bad : {0.0, 0.5, -10.0, 1.1e7, std::nan(""), static_cast<double>(INFINITY)}) { const auto d = s.requestStep(1000, bad); CHECK(!d.allowed && d.stepHz == 1000 && d.reason == "not a valid step"); }
    CHECK(s.requestStep(1000, 1.0).allowed && s.requestStep(1000, 1e7).allowed);       // bounds inclusive
    s.setStepLocked(true);
    { const auto d = s.requestStep(1000, 5000); CHECK(!d.allowed && d.stepHz == 1000 && d.reason == "tuning step locked"); }
    // frequency lock and step lock are independent
    CHECK(s.requestFrequency(1e6, 2e6, TuneSource::Wheel).allowed);
    s.setStepLocked(false); s.setFrequencyLocked(true);
    CHECK(!s.requestFrequency(1e6, 2e6, TuneSource::Wheel).allowed && s.requestStep(1000, 5000).allowed);
    // stepping through a list
    TuneLock t;
    const std::vector<double> steps = {1, 10, 100, 1000, 5000, 9000, 10000, 12500, 25000};
    CHECK(t.stepThrough(steps, 1000, +1).stepHz == 5000);
    CHECK(t.stepThrough(steps, 1000, -1).stepHz == 100);
    CHECK(t.stepThrough(steps, 25000, +1).stepHz == 25000);        // clamps at the top
    CHECK(t.stepThrough(steps, 1, -1).stepHz == 1);                // and at the bottom
    CHECK(t.stepThrough(steps, 4000, +1).stepHz == 9000);   // 4000 is not in the list: the nearest is 5000, one further is 9000
    CHECK(!t.stepThrough({}, 1000, 1).allowed);
    CHECK(!t.stepThrough(steps, 1000, 0).allowed);
    t.setStepLocked(true);
    { const auto d = t.stepThrough(steps, 1000, +1); CHECK(!d.allowed && d.stepHz == 1000); }
    // JSON round trip and refusals
    TuneLock a; a.setFrequencyLocked(true); a.setStepLocked(true);
    LockPolicy p; p.blocked.fill(false); p.blocked[6] = true; a.setPolicy(p);
    TuneLock b; CHECK(b.fromJson(a.toJson()));
    CHECK(b.frequencyLocked() && b.stepLocked() && b.policy().blocked == p.blocked);
    TuneLock c; c.setFrequencyLocked(true);
    CHECK(!c.fromJson(json::array()));
    CHECK(!c.fromJson(json{{"frequencyLocked", "yes"}}));
    CHECK(!c.fromJson(json{{"blocked", json::array({true, false})}}));          // wrong length
    CHECK(!c.fromJson(json{{"blocked", json::array({true, true, true, true, true, true, true, true, true, true, 1})}}));   // wrong type inside
    CHECK(c.frequencyLocked());                                                   // unchanged by every refusal
    CHECK(c.fromJson(json::object()) && c.frequencyLocked());                     // an empty object changes nothing
    std::printf("tune lock: 11 sources x lock on/off (6 blocked by default), invalid requests refused with or without the lock, policy changes, step lock, step list, JSON round trip and refusals\n");
}

void testBandStacking() {
    BandStacking bs;
    std::string err;
    CHECK(bs.setBands(exampleBandPlan(), &err));
    CHECK(bs.bandCount() == 7);
    CHECK(bs.bandOf(7.0e6) == 2 && bs.bandOf(7.3e6) == 2 && bs.bandOf(7.3e6 + 1) == -1 && bs.bandOf(7.0e6 - 1) == -1);   // edges inclusive
    CHECK(bs.bandOf(14.2e6) == 3 && bs.bandOf(94.1e6) == 5 && bs.bandOf(121.5e6) == 6 && bs.bandOf(1e9) == -1 && bs.bandOf(std::nan("")) == -1 && bs.bandOf(-5) == -1);
    // initial registers are the plan's defaults
    CHECK(*bs.read(2, 0) == ts(7.074e6, "USB") && *bs.read(2, 1) == ts(7.15e6, "LSB") && *bs.read(2, 2) == ts(7.03e6, "CW"));
    CHECK(bs.activeRegister(2) == 0 && bs.currentBand() == -1);
    // coming from outside the band: the active register (0)
    auto r = bs.pressBand(2);
    CHECK(r && *r == ts(7.074e6, "USB") && bs.currentBand() == 2);
    // the operator tunes within 40 m and changes mode / filter: register 0 follows
    TuneState mine = ts(7.100e6, "LSB", 2400); mine.hasEdges = true; mine.lowCutHz = 300; mine.highCutHz = 2700;
    CHECK(bs.track(mine));
    CHECK(*bs.read(2, 0) == mine);
    // the same band button again: the next register, then the next, then round
    r = bs.pressBand(2); CHECK(r && *r == ts(7.15e6, "LSB") && bs.activeRegister(2) == 1);
    r = bs.pressBand(2); CHECK(r && *r == ts(7.03e6, "CW") && bs.activeRegister(2) == 2);
    r = bs.pressBand(2); CHECK(r && *r == mine && bs.activeRegister(2) == 0);      // back to the operator's own state
    // go to register 1 and retune there; leave for 20 m; coming back returns where the operator last was (register 1)
    bs.pressBand(2);
    CHECK(bs.track(ts(7.160e6, "LSB", 2700)));
    CHECK(*bs.read(2, 1) == ts(7.160e6, "LSB", 2700) && *bs.read(2, 0) == mine);   // register 0 untouched
    r = bs.pressBand(3); CHECK(r && *r == ts(14.074e6, "USB") && bs.currentBand() == 3);
    r = bs.pressBand(2); CHECK(r && *r == ts(7.160e6, "LSB", 2700) && bs.activeRegister(2) == 1);
    // tracking outside every band, or an invalid state, is not remembered
    CHECK(!bs.track(ts(1.0e6, "AM")));
    CHECK(!bs.track(ts(std::nan(""), "USB")));
    CHECK(!bs.track(ts(7.1e6, "USB", -5)));
    CHECK(!bs.track(ts(7.1e6, "THISMODENAMEISFAR TOO LONG", 0)));
    CHECK(bs.currentBand() == 2);
    // a tune in a DIFFERENT band moves the current band: the register of that band follows
    CHECK(bs.track(ts(14.2e6, "USB", 2700)) && bs.currentBand() == 3 && *bs.read(3, 0) == ts(14.2e6, "USB", 2700));
    // explicit store / read, refusals
    CHECK(bs.store(4, 2, ts(28.5e6, "USB")) && *bs.read(4, 2) == ts(28.5e6, "USB"));
    CHECK(!bs.store(-1, 0, ts(1, "AM")) && !bs.store(99, 0, ts(1, "AM")) && !bs.store(4, 3, ts(1, "AM")) && !bs.store(4, -1, ts(1, "AM")) && !bs.store(4, 0, ts(-1, "AM")));
    CHECK(!bs.read(99, 0) && !bs.read(4, 3) && !bs.pressBand(-1) && !bs.pressBand(7));
    CHECK(bs.band(2)->name == "40 m" && bs.band(7) == nullptr && bs.activeRegister(99) == -1);
    // reset one band
    bs.reset(2);
    CHECK(bs.activeRegister(2) == 0 && *bs.read(2, 1) == ts(7.15e6, "LSB") && *bs.read(2, 0) == ts(7.074e6, "USB"));
    bs.reset(-3); bs.reset(99);   // harmless
    // JSON round trip
    bs.track(ts(7.111e6, "USB", 2400)); bs.pressBand(2);
    const json saved = bs.toJson();
    BandStacking bs2; CHECK(bs2.setBands(exampleBandPlan(), &err));
    CHECK(bs2.fromJson(saved));
    CHECK(bs2.toJson() == saved);
    CHECK(*bs2.read(2, 0) == *bs.read(2, 0) && bs2.activeRegister(2) == bs.activeRegister(2));
    // hostile documents leave the object unchanged
    const json before = bs2.toJson();
    auto mutate = [&](auto fn) { json j = saved; fn(j); return j; };
    CHECK(!bs2.fromJson(json::object()));
    CHECK(!bs2.fromJson(mutate([](json& j) { j[2]["registers"][0]["hz"] = 9.0e6; })));          // outside its own band
    CHECK(!bs2.fromJson(mutate([](json& j) { j[2]["registers"][0]["hz"] = "7100000"; })));      // wrong type
    CHECK(!bs2.fromJson(mutate([](json& j) { j[2]["registers"][0]["mode"] = 5; })));
    CHECK(!bs2.fromJson(mutate([](json& j) { j[2]["registers"] = json::array({1, 2}); })));     // wrong length
    CHECK(!bs2.fromJson(mutate([](json& j) { j[2]["active"] = 3; })));
    CHECK(!bs2.fromJson(mutate([](json& j) { j[2]["active"] = -1; })));
    CHECK(!bs2.fromJson(mutate([](json& j) { j[2]["active"] = 1.5; })));
    CHECK(!bs2.fromJson(mutate([](json& j) { j[2]["registers"][1]["bw"] = -4; })));
    CHECK(!bs2.fromJson(mutate([](json& j) { j[2]["registers"][1]["valid"] = "x"; })));
    CHECK(!bs2.fromJson(mutate([](json& j) { j[0] = 5; })));
    CHECK(bs2.toJson() == before);
    // an unknown band name is ignored (a plan that lost a band), the rest loads; matching is by name, not position
    json shifted = saved;
    shifted.erase(shifted.begin());                                                              // the plan "lost" 160 m: the others shift position
    BandStacking bs3; CHECK(bs3.setBands(exampleBandPlan(), &err));
    CHECK(bs3.fromJson(shifted));
    CHECK(*bs3.read(2, 0) == *bs.read(2, 0));                                                    // 40 m registers landed on 40 m
    json extra = saved; extra.push_back({{"name", "2 m"}, {"active", 0}, {"registers", json::array({{{"hz", 1.0}, {"mode", "NFM"}}, {{"hz", 1.0}, {"mode", "NFM"}}, {{"hz", 1.0}, {"mode", "NFM"}}})}});
    CHECK(bs3.fromJson(extra));
    // an invalid register is skipped by the cycle
    json holes = saved;
    holes[2]["active"] = 0; holes[2]["registers"][1]["valid"] = false;
    BandStacking bs4; CHECK(bs4.setBands(exampleBandPlan(), &err)); CHECK(bs4.fromJson(holes));
    bs4.pressBand(3); bs4.pressBand(2);                                                          // in 40 m at register 0
    r = bs4.pressBand(2); CHECK(r && bs4.activeRegister(2) == 2);                                // 0 -> 2, register 1 skipped
    json dead = saved; dead[2]["active"] = 1; dead[2]["registers"][1]["valid"] = false;
    CHECK(!bs4.fromJson(dead));                                                                  // the active register itself must be valid
    // plan validation
    BandStacking bp;
    std::vector<BandDef> bad = exampleBandPlan(); bad[0].lowHz = 5e6; bad[0].highHz = 1e6;
    CHECK(!bp.setBands(bad, &err) && !err.empty());
    bad = exampleBandPlan(); bad[1].name = "";  CHECK(!bp.setBands(bad, &err));
    bad = exampleBandPlan(); bad[1].name = std::string(40, 'x'); CHECK(!bp.setBands(bad, &err));
    bad = exampleBandPlan(); bad[1].lowHz = std::nan(""); CHECK(!bp.setBands(bad, &err));
    bad = exampleBandPlan(); bad[1].defaults[0].hz = -2; CHECK(!bp.setBands(bad, &err));
    CHECK(bp.bandCount() == 0);
    // a default left at 0 Hz becomes the middle of the band
    BandDef d; d.name = "x"; d.lowHz = 10e6; d.highHz = 11e6;
    BandStacking bm; CHECK(bm.setBands({d}, &err)); CHECK(bm.read(0, 0)->hz == 10.5e6 && bm.read(0, 2)->hz == 10.5e6);
    // empty plan
    BandStacking be; CHECK(be.setBands({}, &err) && be.bandCount() == 0 && be.bandOf(7e6) == -1 && !be.pressBand(0) && !be.track(ts(7e6, "USB")));
    std::printf("band stacking: 7-band plan, inclusive edges, cycle 0->1->2->0, leave and return restores the last register, tracking, JSON by band name, 11 hostile documents leave the state unchanged\n");
}

void testVfo() {
    VfoPair v;
    CHECK(v.active() == VfoId::A && !v.split());
    CHECK(v.set(VfoId::A, ts(7.074e6, "USB", 2400)) && v.set(VfoId::B, ts(14.074e6, "USB", 2700)));
    CHECK(v.rx().hz == 7.074e6 && v.tx().hz == 7.074e6);                          // no split: both follow the active VFO
    v.select(VfoId::B);
    CHECK(v.rx().hz == 14.074e6 && v.tx().hz == 14.074e6 && v.activeState().hz == 14.074e6);
    v.setSplit(true);
    CHECK(v.rx().hz == 7.074e6 && v.tx().hz == 14.074e6);                         // split: receive on A, transmit on B, whichever is selected
    CHECK(v.splitOffsetHz() == 7.0e6);
    v.select(VfoId::A);
    CHECK(v.rx().hz == 7.074e6 && v.tx().hz == 14.074e6);
    v.setSplit(false);
    CHECK(v.splitOffsetHz() == 0.0);
    // swap: contents exchange, the selected letter stays
    v.select(VfoId::A);
    v.swap();
    CHECK(v.vfo(VfoId::A).hz == 14.074e6 && v.vfo(VfoId::B).hz == 7.074e6 && v.active() == VfoId::A && v.activeState().hz == 14.074e6);
    CHECK(v.vfo(VfoId::A).bandwidthHz == 2700 && v.vfo(VfoId::B).bandwidthHz == 2400);                   // mode and filter travel with the frequency
    v.swap();
    CHECK(v.vfo(VfoId::A).hz == 7.074e6);
    // copy
    v.copyAtoB(); CHECK(v.vfo(VfoId::B) == v.vfo(VfoId::A) && v.vfo(VfoId::B).hz == 7.074e6);
    v.set(VfoId::B, ts(3.5e6, "LSB")); v.copyBtoA(); CHECK(v.vfo(VfoId::A) == ts(3.5e6, "LSB"));
    v.set(VfoId::A, ts(1e6, "AM")); v.select(VfoId::A); v.copyActiveToOther(); CHECK(v.vfo(VfoId::B).hz == 1e6);
    v.set(VfoId::B, ts(2e6, "CW")); v.select(VfoId::B); v.copyActiveToOther(); CHECK(v.vfo(VfoId::A).hz == 2e6 && v.vfo(VfoId::A).mode == "CW");
    // edits go to the active VFO only
    v.select(VfoId::A); CHECK(v.setActiveHz(5e6) && v.vfo(VfoId::A).hz == 5e6 && v.vfo(VfoId::B).hz == 2e6);
    v.select(VfoId::B); CHECK(v.setActiveHz(6e6) && v.vfo(VfoId::B).hz == 6e6 && v.vfo(VfoId::A).hz == 5e6);
    // validation: nothing changes on a refusal
    const TuneState before = v.vfo(VfoId::B);
    CHECK(!v.setActiveHz(-1) && !v.setActiveHz(std::nan("")) && !v.setActiveHz(2e13) && v.vfo(VfoId::B) == before);
    CHECK(!v.set(VfoId::A, ts(std::nan(""), "USB")) && v.vfo(VfoId::A).hz == 5e6);
    // split up: B = A + offset, mode and filter copied, split on
    VfoPair s; s.set(VfoId::A, ts(7.074e6, "USB", 2400));
    CHECK(s.splitUp(5000) && s.split() && s.vfo(VfoId::B).hz == 7.079e6 && s.vfo(VfoId::B).mode == "USB" && s.splitOffsetHz() == 5000.0);
    CHECK(s.splitUp(-1000) && s.vfo(VfoId::B).hz == 7.073e6 && s.splitOffsetHz() == -1000.0);
    VfoPair low; low.set(VfoId::A, ts(500, "AM"));
    CHECK(!low.splitUp(-1000) && !low.split());                                  // would go below 0: refused, split stays off
    CHECK(!low.splitUp(std::nan("")) && !low.splitUp(static_cast<double>(INFINITY)));
    // JSON
    VfoPair r; r.set(VfoId::A, ts(7e6, "USB", 2400)); r.set(VfoId::B, ts(7.1e6, "LSB")); r.select(VfoId::B); r.setSplit(true);
    VfoPair q; CHECK(q.fromJson(r.toJson()));
    CHECK(q.vfo(VfoId::A) == r.vfo(VfoId::A) && q.vfo(VfoId::B) == r.vfo(VfoId::B) && q.active() == VfoId::B && q.split());
    VfoPair keep = q;
    CHECK(!q.fromJson(json::object()) && !q.fromJson(json{{"a", 1}, {"b", 2}}) && !q.fromJson(json{{"a", {{"hz", -1}, {"mode", "AM"}}}, {"b", {{"hz", 1}, {"mode", "AM"}}}}));
    json bad = r.toJson(); bad["active"] = "C"; CHECK(!q.fromJson(bad));
    bad = r.toJson(); bad["split"] = 1; CHECK(!q.fromJson(bad));
    bad = r.toJson(); bad["a"]["bw"] = "wide"; CHECK(!q.fromJson(bad));
    CHECK(q.vfo(VfoId::A) == keep.vfo(VfoId::A) && q.active() == keep.active());
    // random operation sequences against a plain reference model
    std::mt19937 g(11);
    VfoPair m; double ra = 0, rb = 0; VfoId act = VfoId::A; bool sp = false;
    m.set(VfoId::A, ts(0, "AM")); m.set(VfoId::B, ts(0, "AM"));
    int mism = 0;
    for (int i = 0; i < 20000; ++i) {
        switch (g() % 7) {
            case 0: { const double f = static_cast<double>(g() % 100000000); (act == VfoId::A ? ra : rb) = f; m.setActiveHz(f); break; }
            case 1: std::swap(ra, rb); m.swap(); break;
            case 2: rb = ra; m.copyAtoB(); break;
            case 3: ra = rb; m.copyBtoA(); break;
            case 4: act = (g() & 1) ? VfoId::A : VfoId::B; m.select(act); break;
            case 5: sp = !sp; m.setSplit(sp); break;
            default: if (act == VfoId::A) { rb = ra; } else { ra = rb; } m.copyActiveToOther(); break;
        }
        const double wantRx = sp ? ra : (act == VfoId::A ? ra : rb), wantTx = sp ? rb : (act == VfoId::A ? ra : rb);
        if (m.vfo(VfoId::A).hz != ra || m.vfo(VfoId::B).hz != rb || m.rx().hz != wantRx || m.tx().hz != wantTx) { ++mism; }
    }
    CHECK(mism == 0);
    std::printf("vfo: rx/tx with and without split, swap, copy both ways, split-up, validation, JSON, 20000 random operations equal a plain reference model\n");
}

void testModeMemory() {
    ModeMemory m;
    // built-in defaults for every mode name, and a fallback for an unknown one
    const char* names[] = {"NFM", "WFM", "AM", "DSB", "USB", "LSB", "CW", "RAW"};
    for (const char* n : names) { CHECK(validModeSettings(ModeMemory::builtinDefault(n))); }
    CHECK(ModeMemory::builtinDefault("USB") == ModeMemory::builtinDefault("LSB"));
    CHECK(!(ModeMemory::builtinDefault("CW") == ModeMemory::builtinDefault("AM")));
    CHECK(validModeSettings(ModeMemory::builtinDefault("???")));
    // the user's flow
    const ModeSettings usb{2400.0, 50.0, 3};
    ModeSettings now = usb;
    now = m.changeMode("USB", "CW", now);                                          // USB -> CW: USB remembered, CW starts at its default
    CHECK(m.recall("USB") && *m.recall("USB") == usb);
    CHECK(now == ModeMemory::builtinDefault("CW"));
    const ModeSettings cw{400.0, 5.0, 2};
    now = m.changeMode("CW", "USB", cw);                                           // the operator tuned CW (400 Hz, 5 Hz step, AGC 2), goes back to USB
    CHECK(now == usb);                                                              // USB came back exactly as it was left
    CHECK(*m.recall("CW") == cw);
    now = m.changeMode("USB", "CW", now);
    CHECK(now == cw);
    CHECK(m.size() == 2);
    // the same mode: nothing happens
    CHECK(m.changeMode("CW", "CW", ModeSettings{123, 4, 1}) == (ModeSettings{123, 4, 1}) && *m.recall("CW") == cw);
    // invalid current settings are not stored and are returned unchanged
    ModeSettings broken{std::nan(""), 5, 1};
    CHECK(m.changeMode("CW", "AM", broken).stepHz == 5 && *m.recall("CW") == cw);
    ModeSettings badAgc{100, 5, 9};
    CHECK(m.changeMode("CW", "AM", badAgc).agcMode == 9 && *m.recall("CW") == cw);
    // disabled: no memory, current returned
    ModeMemory off; off.setEnabled(false);
    CHECK(off.changeMode("USB", "CW", usb) == usb && off.size() == 0);
    off.setEnabled(true);
    CHECK(off.changeMode("USB", "CW", usb) == ModeMemory::builtinDefault("CW") && off.size() == 1);
    // remember on a change within the mode
    CHECK(m.remember("AM", ModeSettings{6000, 9000, 4}) && *m.recall("AM") == (ModeSettings{6000, 9000, 4}));
    CHECK(!m.remember("", ModeSettings{1, 1, 1}) && !m.remember(std::string(17, 'x'), ModeSettings{1, 1, 1}) && !m.remember("AM", ModeSettings{-1, 1, 1}) && !m.remember("AM", ModeSettings{1, 1e8, 1}) && !m.remember("AM", ModeSettings{1, 1, 5}) && !m.remember("AM", ModeSettings{1, 1, -1}));
    CHECK(*m.recall("AM") == (ModeSettings{6000, 9000, 4}));
    m.forget("AM"); CHECK(!m.recall("AM"));
    // capacity
    ModeMemory cap;
    for (int i = 0; i < 64; ++i) { CHECK(cap.remember("m" + std::to_string(i), ModeSettings{1, 1, 0})); }
    CHECK(!cap.remember("one-too-many", ModeSettings{1, 1, 0}) && cap.remember("m5", ModeSettings{2, 2, 0}) && cap.size() == 64);   // existing keys can still be updated
    cap.clear(); CHECK(cap.size() == 0);
    // JSON
    ModeMemory j1; j1.remember("USB", usb); j1.remember("CW", cw); j1.remember("WFM", ModeSettings{180000, 100000, 4});
    ModeMemory j2; CHECK(j2.fromJson(j1.toJson()));
    CHECK(*j2.recall("USB") == usb && *j2.recall("CW") == cw && j2.size() == 3);
    const json good = j1.toJson();
    auto refused = [&](json j) { ModeMemory x; x.remember("KEEP", ModeSettings{1, 1, 0}); const bool ok = x.fromJson(j); return !ok && x.size() == 1 && x.recall("KEEP"); };
    CHECK(refused(json::array()));
    CHECK(refused(json{{"USB", {{"bw", 1}, {"step", 1}}}}));                                    // missing agc
    CHECK(refused(json{{"USB", {{"bw", "x"}, {"step", 1}, {"agc", 0}}}}));
    CHECK(refused(json{{"USB", {{"bw", 1}, {"step", 1}, {"agc", 7}}}}));                        // AGC out of range
    CHECK(refused(json{{"USB", {{"bw", -1}, {"step", 1}, {"agc", 0}}}}));
    CHECK(refused(json{{"USB", {{"bw", 1}, {"step", std::nan("")}, {"agc", 0}}}}));
    CHECK(refused(json{{"USB", 5}}));
    CHECK(refused(json{{"", {{"bw", 1}, {"step", 1}, {"agc", 0}}}}));
    CHECK(refused(json{{std::string(17, 'm'), {{"bw", 1}, {"step", 1}, {"agc", 0}}}}));
    { json many = json::object(); for (int i = 0; i < 65; ++i) { many["m" + std::to_string(i)] = {{"bw", 1}, {"step", 1}, {"agc", 0}}; } CHECK(refused(many)); }
    // mutation fuzz over a valid document: never a crash, and an accepted document leaves only valid settings
    std::mt19937 g(2);
    int accepted = 0, rejected = 0;
    for (int i = 0; i < 20000; ++i) {
        json j = good;
        const int k = static_cast<int>(g() % 4);
        auto it = j.begin(); std::advance(it, g() % j.size());
        switch (k) {
            case 0: (*it)["bw"] = static_cast<double>(static_cast<int>(g() % 200000000) - 1000); break;
            case 1: (*it)["agc"] = static_cast<int>(g() % 12) - 2; break;
            case 2: (*it)["step"] = (g() & 1) ? json("x") : json(static_cast<double>(g() % 20000000)); break;
            default: (*it).erase("step"); break;
        }
        ModeMemory x;
        if (x.fromJson(j)) { ++accepted; for (const char* n : {"USB", "CW", "WFM"}) { const auto s = x.recall(n); CHECK(s && validModeSettings(*s)); } } else { ++rejected; }
    }
    CHECK(accepted > 1000 && rejected > 1000);
    std::printf("mode memory: save on leaving, restore on return, defaults, disabled, capacity 64, 10 hostile documents, 20000 mutated documents (%d accepted, all valid; %d refused)\n", accepted, rejected);
}

}  // namespace

int main() {
    testTuneLock();
    testBandStacking();
    testVfo();
    testModeMemory();
    return testSummary("test_operating_aids");
}
