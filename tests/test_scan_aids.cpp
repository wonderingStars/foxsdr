// Tests for the scanner additions (core/scan_aids.hpp): priority channels, lockouts, the scan log and mute while scanning. The integration tests drive the
// REAL core::Scanner (core/scanner.hpp) with a simulated radio and squelch, so the models are shown working beside the scanner they will sit next to.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/scan_aids.hpp"
#include "test_check.hpp"

#include <chrono>
#include <cstdio>
#include <random>
#include <string>

namespace {
using namespace cascade::core::scanaids;
using cascade::core::Scanner;
using nlohmann::json;
using Kind = PriorityAction::Kind;

PriorityScanner makePs(bool interruptHold = true) {
    PriorityScanner p;
    std::string err;
    p.setChannels({100e6, 200e6}, &err);
    PriorityConfig c; c.enabled = true; c.intervalS = 2.0; c.dwellMs = 200; c.hangMs = 2000; c.interruptHold = interruptHold;
    p.setConfig(c);
    return p;
}

void testPriorityTiming() {
    PriorityScanner p = makePs();
    // the first update only sets the baseline
    CHECK(p.update(0, false, false).kind == Kind::None);
    CHECK(p.update(1999, false, false).kind == Kind::None);
    PriorityAction a = p.update(2000, false, false);                           // 2.0 s: the check starts at the first priority channel
    CHECK(a.kind == Kind::GoPriority && a.index == 0 && a.hz == 100e6 && p.state() == PriorityScanner::State::Checking);
    CHECK(p.update(2100, false, false).kind == Kind::None);                    // dwell (200 ms) not over
    a = p.update(2200, false, false);                                          // no signal on the first: the second
    CHECK(a.kind == Kind::GoPriority && a.index == 1 && a.hz == 200e6);
    CHECK(p.update(2300, false, false).kind == Kind::None);
    a = p.update(2400, false, false);                                          // none on the second either: back to scanning
    CHECK(a.kind == Kind::ReturnToScan && p.state() == PriorityScanner::State::Idle);
    CHECK(p.update(4399, false, false).kind == Kind::None);                    // the next check is a full interval after the cycle ended
    CHECK(p.update(4400, false, false).kind == Kind::GoPriority);
    CHECK(p.hits() == 0);
    // a signal on the second channel: stay, hold while it lasts, return after the hang time
    p.update(4600, false, false);                                              // first channel done (idx 1)
    CHECK(p.currentIndex() == 1);
    CHECK(p.update(4800, true, false).kind == Kind::None && p.state() == PriorityScanner::State::Holding && p.hits() == 1);
    CHECK(p.update(7000, true, false).kind == Kind::None && p.state() == PriorityScanner::State::Holding);   // still talking after 2.2 s: no return
    CHECK(p.update(8999, false, false).kind == Kind::None);                    // quiet 1999 ms: within the hang time
    CHECK(p.update(9000, true, false).kind == Kind::None);                     // talks again: the hang restarts
    CHECK(p.update(10999, false, false).kind == Kind::None);
    a = p.update(11000, false, false);                                         // 2000 ms of unbroken quiet
    CHECK(a.kind == Kind::ReturnToScan && p.state() == PriorityScanner::State::Idle);
    // a signal on the FIRST channel holds there without checking the second
    PriorityScanner q = makePs();
    q.update(0, false, false); q.update(2000, false, false);
    CHECK(q.update(2200, true, false).kind == Kind::None && q.state() == PriorityScanner::State::Holding && q.currentIndex() == 0);
    std::printf("priority timing: check every 2.0 s, 200 ms dwell per channel, hold while the signal lasts, 2000 ms hang restarts on new activity, a hit on the first channel skips the second\n");
}

void testPriorityRules() {
    // interrupt a hold on an ordinary signal, or not
    PriorityScanner on = makePs(true), off = makePs(false);
    on.update(0, false, true); off.update(0, false, true);
    CHECK(on.update(2000, false, true).kind == Kind::GoPriority);
    CHECK(off.update(2000, false, true).kind == Kind::None);                   // the scanner is holding on a conversation: not interrupted
    CHECK(off.update(3000, false, true).kind == Kind::None);
    CHECK(off.update(3100, false, false).kind == Kind::GoPriority);            // the hold ended: the overdue check runs at once (once, not several)
    CHECK(off.update(3200, false, false).kind == Kind::None);
    // a long stall (the UI hangs for an hour) causes ONE check, not a burst
    PriorityScanner s = makePs();
    s.update(0, false, false);
    int actions = 0;
    PriorityAction a = s.update(3600000, false, false); if (a.kind != Kind::None) { ++actions; }
    a = s.update(3600001, false, false); if (a.kind != Kind::None) { ++actions; }
    CHECK(actions == 1);
    // the clock jumps backward: counts as zero elapsed, no burst, and time accumulates from the new value
    PriorityScanner b = makePs();
    b.update(10000, false, false);
    b.update(11500, false, false);                                             // 1.5 s elapsed
    CHECK(b.update(1000, false, false).kind == Kind::None);                    // jumped back 10.5 s: zero elapsed
    CHECK(b.update(1400, false, false).kind == Kind::None);                    // 1.9 s in total
    CHECK(b.update(1500, false, false).kind == Kind::GoPriority);              // 2.0 s in total (1500 + 400 + 100)
    // disabled / no channels / bad times
    PriorityScanner d = makePs();
    d.update(0, false, false); d.update(2000, false, false);                   // checking
    PriorityConfig c = d.config(); c.enabled = false; d.setConfig(c);
    CHECK(d.state() == PriorityScanner::State::Idle);
    CHECK(d.update(5000, false, false).kind == Kind::None && d.update(50000, false, false).kind == Kind::None);   // never fires while disabled
    PriorityScanner e;
    PriorityConfig ce; ce.enabled = true; e.setConfig(ce);
    CHECK(e.update(0, false, false).kind == Kind::None && e.update(100000, false, false).kind == Kind::None);   // enabled but no channels
    CHECK(makePs().update(std::nan(""), false, false).kind == Kind::None);
    // a disable while checking says "return to scan" once
    PriorityScanner f = makePs(); f.update(0, false, false); f.update(2000, false, false);
    c = f.config(); c.enabled = true; f.setConfig(c);                          // reconfigure resets quietly
    CHECK(f.state() == PriorityScanner::State::Idle);
    // config sanitising
    PriorityScanner g; PriorityConfig w; w.enabled = true; w.intervalS = -5; w.dwellMs = -1; w.hangMs = -9; g.setConfig(w);
    CHECK(g.config().intervalS == 2.0 && g.config().dwellMs == 10 && g.config().hangMs == 0);
    w.intervalS = std::nan(""); w.dwellMs = 1000000; w.hangMs = 100000000; g.setConfig(w);
    CHECK(g.config().intervalS == 2.0 && g.config().dwellMs == 10000 && g.config().hangMs == 600000);
    w.intervalS = 1e9; g.setConfig(w); CHECK(g.config().intervalS == 3600.0);
    // channel list validation
    PriorityScanner h; std::string err;
    CHECK(h.setChannels({100e6, 100e6 + 0.4, 200e6}, &err) && h.channels().size() == 2);       // duplicates within 1 Hz merge
    CHECK(!h.setChannels({100e6, std::nan("")}, &err) && !err.empty() && h.channels().size() == 2);
    CHECK(!h.setChannels({-1.0}, &err) && !h.setChannels({2e13}, &err));
    std::vector<double> many; for (int i = 0; i < 17; ++i) { many.push_back(1e6 * (i + 1)); }
    CHECK(!h.setChannels(many, &err) && h.channels().size() == 2);
    many.pop_back(); CHECK(h.setChannels(many, &err) && h.channels().size() == 16);
    CHECK(h.setChannels({}, &err) && h.channels().empty());
    std::printf("priority rules: interrupt or not, one check after a stall, backward clock, disabled, config clamps, channel list validation\n");
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// The scanner + priority model + mute + log driven together against a simulated radio.
// ---------------------------------------------------------------------------------------------------------------------------------------------
void testScannerIntegration() {
    // Ordinary channels 150.000, 150.100, 150.200 MHz (list scan); a chatty signal on 150.100 from t = 3 s to t = 30 s; a priority channel 100 MHz that is
    // active from t = 10 s to t = 11 s.
    Scanner sc;
    Scanner::Params sp;
    sp.list = {150.0e6, 150.1e6, 150.2e6}; sp.dwellMs = 50; sp.holdMs = 1000; sp.resumeMs = 100; sp.listenMs = 0;
    sc.configure(sp);
    PriorityScanner pri;
    std::string err;
    pri.setChannels({100e6}, &err);
    PriorityConfig pc; pc.enabled = true; pc.intervalS = 2.0; pc.dwellMs = 200; pc.hangMs = 500; pc.interruptHold = true;
    pri.setConfig(pc);
    ScanMute mute; mute.setEnabled(true); mute.setTailMs(150);
    ScanLog log;

    double radio = 0.0;           // where the radio is tuned
    double scanHz = 0.0;          // where the scanner thinks it is (restored after a priority visit)
    bool onPriority = false;
    auto signalAt = [&](double hz, double tMs) {
        if (hz == 150.1e6 && tMs >= 3000 && tMs <= 30000) { return true; }
        if (hz == 100e6 && tMs >= 10000 && tMs <= 11000) { return true; }
        return false;
    };
    sc.start(0.0);
    int mutedWhileStepping = 0, unmutedWhileStepping = 0, mutedOnSignal = 0, priorityVisits = 0, returns = 0;
    double firstPriorityHit = -1, lastPriorityEnd = -1;
    bool logOpen = false; double logHz = 0;
    for (double t = 0.0; t <= 40000.0; t += 10.0) {
        const bool sq = signalAt(radio, t);
        const PriorityAction a = pri.update(t, sq, sc.state() == Scanner::State::Paused || sc.state() == Scanner::State::Holding);
        if (a.kind == Kind::GoPriority) { if (!onPriority) { scanHz = radio; ++priorityVisits; } onPriority = true; radio = a.hz; }
        else if (a.kind == Kind::ReturnToScan) { onPriority = false; radio = scanHz; ++returns; lastPriorityEnd = t; }
        if (!onPriority) {
            const auto retune = sc.tick(t, signalAt(radio, t));
            if (retune) { radio = *retune; scanHz = radio; }
        }
        if (onPriority && pri.state() == PriorityScanner::State::Holding && firstPriorityHit < 0) { firstPriorityHit = t; }
        // mute: only while the scanner is stepping and the radio is on the scanner's own channel
        const bool m = mute.update(onPriority ? Scanner::State::Paused : sc.state(), t);
        if (sc.state() == Scanner::State::Scanning && !onPriority) { if (m) { ++mutedWhileStepping; } else { ++unmutedWhileStepping; } }
        if (m && signalAt(radio, t)) { ++mutedOnSignal; }
        // log: a stop on an ordinary signal
        const bool stopped = !onPriority && (sc.state() == Scanner::State::Paused || sc.state() == Scanner::State::Holding);
        if (stopped && !logOpen) { log.begin(1000000 + static_cast<std::int64_t>(t), t, radio, "NFM", StopReason::Signal, -50.0); logOpen = true; logHz = radio; }
        else if (stopped && logOpen) { log.sample(t, -50.0); }
        else if (!stopped && logOpen) { log.end(t, EndReason::SignalGone); logOpen = false; }
    }
    (void)logHz;
    CHECK(priorityVisits >= 15);                                  // a visit every ~2 s over 40 s, minus the time spent on the priority signal
    CHECK(returns == priorityVisits || returns == priorityVisits - 1);
    CHECK(firstPriorityHit >= 10000 && firstPriorityHit <= 12300); // the priority signal at 10-11 s was found within one check interval and its dwell
    CHECK(lastPriorityEnd > 0);
    CHECK(pri.hits() >= 1);
    // while the scanner only stepped the audio was muted (after the tail); it was never muted on a signal
    CHECK(mutedWhileStepping > unmutedWhileStepping);
    CHECK(mutedOnSignal == 0);
    CHECK(log.size() >= 1);
    const auto ev = log.events();
    bool allAt150100 = true;
    for (const ScanEvent& e : ev) { if (e.hz != 150.1e6) { allAt150100 = false; } }
    CHECK(allAt150100);
    std::printf("integration with the real Scanner: %d priority visits (%d returns), the priority signal at 10-11 s found at t = %.0f ms, audio muted %d of %d stepping ticks, never on a signal, %zu scan-log stops (all on 150.100 MHz)\n",
                priorityVisits, returns, firstPriorityHit, mutedWhileStepping, mutedWhileStepping + unmutedWhileStepping, log.size());
}

void testLockouts() {
    LockoutList l;
    CHECK(l.lockFrequency(145.5e6, 6250, false));
    CHECK(l.isLocked(145.5e6) && l.isLocked(145.5e6 - 6250) && l.isLocked(145.5e6 + 6250) && !l.isLocked(145.5e6 + 6251) && !l.isLocked(145.5e6 - 6251));   // inclusive edges
    CHECK(l.temporaryCount() == 1 && l.permanentCount() == 0);
    // permanent
    CHECK(l.lockFrequency(7.074e6, 0.0, true));
    CHECK(l.isLocked(7.074e6) && !l.isLocked(7.074e6 + 1));
    // a range
    CHECK(l.lock(118e6, 137e6, true));
    CHECK(l.isLocked(121.5e6) && !l.isLocked(117.999e6));
    // merging within a kind: overlapping and touching ranges become one
    CHECK(l.lock(130e6, 140e6, true));
    CHECK(l.permanentCount() == 2 && l.isLocked(139e6) && l.isLocked(118e6));
    CHECK(l.lock(200e6, 201e6, true) && l.lock(201e6, 202e6, true) && l.permanentCount() == 3 && l.isLocked(201.5e6));   // 200-201 and 201-202 touch at 201 MHz: one entry
    // temporary entries with a lifetime
    LockoutList t;
    CHECK(t.lock(100e6, 100e6 + 10, false, 1000.0, 5000.0));
    CHECK(t.isLocked(100e6, 1000.0) && t.isLocked(100e6, 5999.0) && !t.isLocked(100e6, 6000.0) && !t.isLocked(100e6, 7000.0));   // expires exactly at now + ttl
    CHECK(t.temporaryCount() == 1);                                           // still stored until expire()
    CHECK(t.expire(5999.0) == 0 && t.expire(6000.0) == 1 && t.temporaryCount() == 0);
    // no lifetime: until cleared
    CHECK(t.lock(100e6, 100e6 + 10, false, 1000.0, 0.0));
    CHECK(t.isLocked(100e6, 1e12));
    t.clearTemporary(); CHECK(!t.isLocked(100e6, 0) && t.temporaryCount() == 0);
    // temporary entries never stretch another entry's lifetime: different expiries stay separate entries, a request inside a longer-lived entry changes nothing
    CHECK(t.lock(1e6, 2e6, false, 0.0, 1000.0) && t.lock(1.5e6, 3e6, false, 0.0, 5000.0));
    CHECK(t.temporaryCount() == 2);
    CHECK(t.isLocked(1.2e6, 500.0) && !t.isLocked(1.2e6, 1000.0));            // [1, 2] MHz expires at 1000 ms although the other entry reaches into it above 1.5 MHz
    CHECK(t.isLocked(1.7e6, 4999.0) && !t.isLocked(1.7e6, 5000.0) && t.isLocked(2.5e6, 4999.0));
    CHECK(t.lock(2.0e6, 2.5e6, false, 0.0, 3000.0) && t.temporaryCount() == 2);       // inside the 5 s entry: already covered
    CHECK(t.lock(1.2e6, 1.3e6, false, 0.0, 0.0) && t.temporaryCount() == 3 && t.isLocked(1.25e6, 1e12));   // no expiry asked: its own entry
    CHECK(t.lock(1.22e6, 1.28e6, false, 0.0, 700.0) && t.temporaryCount() == 3);        // inside the no-expiry entry: covered
    t.clearTemporary();
    CHECK(t.lock(1e6, 2e6, false, 0.0, 1000.0) && t.lock(1.5e6, 3e6, false, 0.0, 1000.0) && t.temporaryCount() == 1);   // same expiry and overlapping: merged
    // a permanent entry outlives clearTemporary and is not removed by expiry
    LockoutList k; k.lockFrequency(5e6, 100, true); k.lockFrequency(6e6, 100, false, 0.0, 10.0);
    k.clearTemporary(); CHECK(k.isLocked(5e6) && !k.isLocked(6e6));
    k.expire(1e12); CHECK(k.isLocked(5e6));
    // unlocking
    LockoutList u; u.lock(100, 200, true); u.lock(150, 175, false); u.lock(300, 400, true); u.lock(500, 600, false);
    CHECK(u.unlockAt(160) == 2 && !u.isLocked(160) && u.isLocked(350));       // removes the permanent AND the temporary entry containing it
    CHECK(u.unlockAt(160) == 0);
    CHECK(u.unlockRange(350, 550) == 2 && u.permanentCount() == 0 && u.temporaryCount() == 0);
    u.lock(1, 2, true); u.lock(3, 4, false); u.clearAll(); CHECK(u.permanentCount() == 0 && u.temporaryCount() == 0);
    // validation
    LockoutList v;
    CHECK(!v.lock(std::nan(""), 5, true) && !v.lock(5, std::nan(""), true) && !v.lock(10, 5, true) && !v.lock(-1, 5, true) && !v.lock(1, 2e13, true));
    CHECK(!v.lock(1, 2, false, 0.0, -5.0) && !v.lock(1, 2, false, 0.0, std::nan("")) && !v.lock(1, 2, false, std::nan(""), 5.0));
    CHECK(!v.lockFrequency(1e6, -1.0, true) && !v.lockFrequency(std::nan(""), 1.0, true));
    CHECK(v.lockFrequency(100.0, 1000.0, true) && v.isLocked(0.0) && v.isLocked(1100.0));   // the low edge clamps at 0 Hz
    CHECK(!v.isLocked(std::nan("")));
    CHECK(v.permanentCount() == 1 && v.temporaryCount() == 0);
    // capacity: 1024 per kind
    LockoutList cap;
    for (int i = 0; i < 1024; ++i) { CHECK(cap.lock(i * 10.0, i * 10.0 + 1, true)); }
    CHECK(!cap.lock(1e9, 1e9 + 1, true) && cap.permanentCount() == 1024);
    CHECK(cap.lock(5.5, 12.5, true) && cap.permanentCount() == 1024);   // an overlapping request merges even when the list is full
    CHECK(cap.lock(1e9, 1e9 + 1, false));                                      // the temporary list has its own room
    // speed: 1024 + 1024 entries, a million queries
    LockoutList big;
    for (int i = 0; i < 1024; ++i) { big.lock(i * 1000.0, i * 1000.0 + 10, true); big.lock(2e6 + i * 1000.0, 2e6 + i * 1000.0 + 10, false); }
    const auto t0 = std::chrono::steady_clock::now();
    int hits = 0;
    for (int i = 0; i < 1000000; ++i) { if (big.isLocked(static_cast<double>(i % 4000000), 0.0)) { ++hits; } }
    const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / 1e6;
    CHECK(hits > 0);
    CHECK(ns < 20000.0);   // a scanner step is at best a millisecond apart
    // JSON: only permanent entries are saved
    LockoutList j; j.lock(7.0e6, 7.1e6, true); j.lock(14e6, 14e6, true); j.lock(21e6, 21.1e6, false);
    LockoutList j2; CHECK(j2.fromJson(j.toJson()));
    CHECK(j2.permanentCount() == 2 && j2.temporaryCount() == 0 && j2.isLocked(7.05e6) && j2.isLocked(14e6) && !j2.isLocked(21.05e6));
    const json bad[] = {json::object(), json::array({json::array({1, 2, 3})}), json::array({json::array({"a", 2})}), json::array({json::array({5, 1})}), json::array({json::array({-1, 2})}), json::array({1})};
    for (const json& b : bad) { LockoutList x; x.lock(1, 2, true); CHECK(!x.fromJson(b) && x.permanentCount() == 1); }
    { json many = json::array(); for (int i = 0; i < 1025; ++i) { many.push_back(json::array({i, i + 1})); } LockoutList x; CHECK(!x.fromJson(many)); }
    // a mutation fuzz over the loader
    std::mt19937 g(4);
    int acc = 0;
    for (int i = 0; i < 20000; ++i) {
        json m = j.toJson();
        const std::size_t e = g() % m.size();
        switch (g() % 4) { case 0: m[e][0] = static_cast<double>(g() % 100000000); break; case 1: m[e][1] = -static_cast<double>(g() % 100); break; case 2: m[e] = json::array({1.0}); break; default: m[e][0] = "x"; break; }
        LockoutList x;
        if (x.fromJson(m)) { ++acc; for (const LockEntry& le : x.permanent()) { CHECK(le.hiHz >= le.loHz && le.loHz >= 0.0); } }
    }
    std::printf("lockouts: inclusive edges, merge within a kind, ttl expiry exactly at now + ttl, permanent vs temporary, unlock at / by range, validation, cap 1024 per kind, %.0f ns per query with 2048 entries, JSON (permanent only), 20000 mutated documents (%d accepted, all valid)\n", ns, acc);
}

void testScanLog() {
    ScanLog log(5);
    CHECK(!log.open());
    log.end(100.0, EndReason::Stopped);                                       // end without begin: ignored
    CHECK(log.size() == 0);
    // time-weighted mean: -80 dB for 1 s then -60 dB for 3 s -> (-80 + 3 x -60) / 4 = -65
    log.begin(1794415314123LL, 1000.0, 145.5e6, "NFM", StopReason::Signal, -80.0, "Repeater");
    CHECK(log.open());
    log.sample(2000.0, -60.0);
    log.sample(4000.0, -55.0);
    log.sample(5000.0, -70.0);
    log.end(5000.0, EndReason::SignalGone);
    CHECK(!log.open() && log.size() == 1);
    ScanEvent e = log.events()[0];
    CHECK(e.hz == 145.5e6 && e.mode == "NFM" && e.label == "Repeater" && e.startUnixMs == 1794415314123LL && e.stop == StopReason::Signal && e.end == EndReason::SignalGone);
    CHECK(e.durationMs == 4000.0 && e.peakDb == -55.0);
    // weights: [1000,2000) at -80, [2000,4000) at -60, [4000,5000) at -55 -> (-80 x 1000 - 60 x 2000 - 55 x 1000) / 4000
    CHECK(std::fabs(e.meanDb - ((-80.0 * 1000 - 60.0 * 2000 - 55.0 * 1000) / 4000.0)) < 1e-9);
    // a begin while open ends the open one as superseded
    log.begin(1000, 0.0, 100e6, "AM", StopReason::Priority, -40.0);
    log.begin(2000, 500.0, 101e6, "AM", StopReason::Signal, -40.0);
    CHECK(log.size() == 2 && log.events()[1].end == EndReason::Superseded && log.events()[1].durationMs == 500.0 && log.events()[1].stop == StopReason::Priority);
    log.end(900.0, EndReason::Skipped);
    // a backward clock gives duration 0, not negative
    log.begin(3000, 10000.0, 102e6, "AM", StopReason::Manual, -30.0);
    log.sample(5000.0, -20.0);
    log.end(2000.0, EndReason::Stopped);
    CHECK(log.events().back().durationMs == 0.0 && log.events().back().peakDb == -20.0);
    // non-finite readings are ignored; a non-finite frequency logs as 0
    log.begin(4000, 0.0, std::nan(""), "AM", StopReason::Signal, std::nan(""));
    log.sample(10.0, std::nan("")); log.sample(20.0, -33.0);
    log.end(30.0, EndReason::ListenLimit);
    CHECK(log.events().back().hz == 0.0 && log.events().back().peakDb == -33.0 && log.events().back().end == EndReason::ListenLimit);
    // ring: capacity 5 keeps the newest, oldest first, counts what it dropped
    ScanLog ring(5);
    for (int i = 0; i < 12; ++i) { ring.begin(i * 1000, i * 100.0, 100e6 + i, "NFM", StopReason::Signal, -50.0); ring.end(i * 100.0 + 50.0, EndReason::SignalGone); }
    CHECK(ring.size() == 5 && ring.dropped() == 7);
    const auto evs = ring.events();
    bool inOrder = true;
    for (std::size_t i = 0; i < evs.size(); ++i) { if (evs[i].hz != 100e6 + 7 + static_cast<double>(i)) { inOrder = false; } }
    CHECK(inOrder);
    ring.setCapacity(2);
    CHECK(ring.size() == 2 && ring.events()[0].hz == 100e6 + 10 && ring.events()[1].hz == 100e6 + 11);
    ring.setCapacity(0); ring.begin(0, 0, 1, "A", StopReason::Signal, 0); ring.end(1, EndReason::Stopped); ring.begin(0, 0, 2, "A", StopReason::Signal, 0); ring.end(1, EndReason::Stopped);
    CHECK(ring.size() == 1 && ring.events()[0].hz == 2.0);                       // capacity 0 is raised to 1
    ring.clear(); CHECK(ring.size() == 0 && ring.dropped() == 0 && !ring.open());
    // per-frequency statistics: grouped by whole Hz, ordered by total time
    ScanLog st;
    auto add = [&](double hz, double dur, std::int64_t at, double level) { st.begin(at, 0.0, hz, "NFM", StopReason::Signal, level); st.end(dur, EndReason::SignalGone); };
    add(145.5e6, 3000, 100, -60); add(146.0e6, 10000, 200, -70); add(145.5e6, 4000, 300, -50); add(145.5e6 + 0.2, 1000, 400, -65); add(147.0e6, 10000, 50, -80);
    const auto stats = st.stats();
    CHECK(stats.size() == 3);
    CHECK(stats[0].hz == 146.0e6 && stats[1].hz == 147.0e6);                       // 10 s each: equal totals go by frequency
    CHECK(stats[2].hz == 145.5e6 && stats[2].stops == 3 && stats[2].totalMs == 8000.0 && stats[2].longestMs == 4000.0 && stats[2].lastUnixMs == 400 && stats[2].peakDb == -50.0);   // 145.5 MHz and 145.5 MHz + 0.2 Hz are one frequency
    // CSV golden
    ScanLog csv;
    csv.begin(1794415314123LL, 0.0, 145500000.0, "NFM", StopReason::Signal, -60.0, "Repeater");
    csv.sample(2500.0, -50.0); csv.end(2500.0, EndReason::SignalGone);
    csv.begin(1794415320000LL, 3000.0, 7074000.4, "USB", StopReason::Priority, -70.0, "a,b \"q\"\nline");
    csv.end(4000.0, EndReason::Skipped);
    csv.begin(1794415330000LL, 5000.0, 100e6, "AM", StopReason::Manual, -40.0, "=cmd|' /C calc'!A0");
    csv.end(5500.0, EndReason::Locked);
    const std::string want =
        "time_utc,freq_hz,mode,label,duration_s,peak_db,mean_db,stop_reason,end_reason\r\n"
        "2026-11-11T16:41:54.123Z,145500000,NFM,Repeater,2.500,-50.0,-60.0,signal,signal_gone\r\n"
        "2026-11-11T16:42:00.000Z,7074000,USB,\"a,b \"\"q\"\"\nline\",1.000,-70.0,-70.0,priority,skipped\r\n"
        "2026-11-11T16:42:10.000Z,100000000,AM,'=cmd|' /C calc'!A0,0.500,-40.0,-40.0,manual,locked\r\n";
    if (csv.toCsv() != want) { std::printf("--- got ---\n%s--- want ---\n%s", csv.toCsv().c_str(), want.c_str()); }
    CHECK(csv.toCsv() == want);
    CHECK(ScanLog::csvField("plain") == "plain" && ScanLog::csvField("") == "" && ScanLog::csvField("a\"b") == "\"a\"\"b\"" && ScanLog::csvField("+1") == "'+1" && ScanLog::csvField("@x") == "'@x" && ScanLog::csvField("-1") == "'-1" && ScanLog::csvField("\tx") == "'\tx");
    // speed: 10000 events to CSV
    ScanLog bigLog(10000);
    for (int i = 0; i < 10000; ++i) { bigLog.begin(1794415314123LL + i, i * 10.0, 100e6 + i, "NFM", StopReason::Signal, -60.0, "x"); bigLog.end(i * 10.0 + 5.0, EndReason::SignalGone); }
    const auto t0 = std::chrono::steady_clock::now();
    const std::string big = bigLog.toCsv();
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    CHECK(big.size() > 10000 * 50);
    CHECK(ms < 500.0);
    std::printf("scan log: time-weighted mean and peak, superseded begin, backward clock, non-finite input, ring of 5 keeps the newest 5 and counts 7 dropped, per-frequency stats, golden CSV with quoting and formula-injection guard, 10000 events to CSV in %.1f ms\n", ms);
}

void testMute() {
    ScanMute m;
    CHECK(!m.update(Scanner::State::Scanning, 0));                                  // disabled: never muted
    m.setEnabled(true); m.setTailMs(150);
    m.notifyIdle();
    CHECK(!m.update(Scanner::State::Idle, 0) && !m.muted());
    CHECK(m.update(Scanner::State::Scanning, 10));                                  // a fresh scan from Idle: muted at once
    CHECK(m.update(Scanner::State::Scanning, 60));
    CHECK(!m.update(Scanner::State::Paused, 100));                                  // a signal: audio up
    CHECK(!m.update(Scanner::State::Holding, 200));
    CHECK(!m.update(Scanner::State::Scanning, 1300));                               // the hold ended: the tail keeps the audio open ...
    CHECK(!m.update(Scanner::State::Scanning, 1449));
    CHECK(m.update(Scanner::State::Scanning, 1450));                                // ... for exactly 150 ms, then muted
    CHECK(m.update(Scanner::State::Scanning, 1500));
    CHECK(!m.update(Scanner::State::Idle, 1600));                                   // idle: never muted
    CHECK(m.update(Scanner::State::Scanning, 1700));                                // a new scan from Idle: no tail, muted at once
    // tail 0
    ScanMute z; z.setEnabled(true); z.setTailMs(0);
    CHECK(z.update(Scanner::State::Scanning, 0) && !z.update(Scanner::State::Paused, 1) && z.update(Scanner::State::Scanning, 2));
    // disabling mid-scan unmutes
    z.setEnabled(false); CHECK(!z.update(Scanner::State::Scanning, 3) && !z.muted());
    // bad inputs
    z.setEnabled(true); z.setTailMs(-5); CHECK(z.update(Scanner::State::Scanning, 10));
    z.setTailMs(100000000);
    CHECK(!z.update(Scanner::State::Paused, 20));
    CHECK(!z.update(Scanner::State::Scanning, 30) && !z.update(Scanner::State::Scanning, 9000) && z.update(Scanner::State::Scanning, 10030));   // capped at 10 s
    const bool before = z.muted();
    CHECK(z.update(Scanner::State::Scanning, std::nan("")) == before);              // a NaN clock changes nothing
    // a backward clock inside the tail cannot unmute early or crash
    ScanMute b; b.setEnabled(true); b.setTailMs(150);
    b.update(Scanner::State::Paused, 1000); b.update(Scanner::State::Scanning, 2000);
    CHECK(!b.update(Scanner::State::Scanning, 1500));                               // jumped back: still inside the tail
    CHECK(b.update(Scanner::State::Scanning, 2150));
    std::printf("mute while scanning: muted only while stepping, up at once on a signal, 150 ms tail after a stop, no tail from Idle, disabled, clamps, NaN and backward clocks\n");
}

}  // namespace

int main() {
    testPriorityTiming();
    testPriorityRules();
    testScannerIntegration();
    testLockouts();
    testScanLog();
    testMute();
    return testSummary("test_scan_aids");
}
