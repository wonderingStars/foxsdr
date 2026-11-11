// The scan models of the tuning lane: the scan trigger with a dynamic floor, the FFT scan, the scan plan (several ranges, direction, step back,
// hit to bookmark). Standalone models: the real Scanner is driven here with a plan's list to prove the two fit.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "core/fft_scan.hpp"
#include "core/scan_plan.hpp"
#include "core/scan_trigger.hpp"
#include "core/scanner.hpp"
#include "test_check.hpp"

namespace ct = cascade::core::tuning;
namespace cc = cascade::core;

namespace {

// A deterministic noise source in [-1, 1).
struct Lcg {
    std::uint64_t s;
    explicit Lcg(std::uint64_t seed) : s(seed) {}
    double next() {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<double>((s >> 33) & 0xFFFFF) / 524288.0 - 1.0;
    }
    std::uint32_t u32() { s = s * 6364136223846793005ULL + 1442695040888963407ULL; return static_cast<std::uint32_t>(s >> 32); }
};

void testTriggerFixed() {
    ct::ScanTrigger t;
    ct::ScanTriggerConfig c;
    c.mode = ct::TriggerMode::Fixed;
    c.fixedDb = -60.0;
    c.hysteresisDb = 2.0;
    CHECK(t.setConfig(c));
    CHECK(!t.update(0, -80.0));
    CHECK(!t.update(0, -60.5));
    CHECK(t.update(0, -60.0));          // the boundary triggers (>=)
    CHECK(t.update(0, -61.5));          // inside the hysteresis: still on
    CHECK(!t.update(0, -62.5));         // below fixed - hysteresis: released
    CHECK(t.update(0, -50.0));
    // a Fixed trigger on the very first reading of a slot
    CHECK(t.update(5, -40.0));
    CHECK(t.triggerLevelDb(7) == -60.0);
}

void testTriggerRelativeFloor() {
    ct::ScanTrigger t;
    ct::ScanTriggerConfig c;  // relative, margin 8, hysteresis 2, rise 0.1
    CHECK(t.setConfig(c));
    // the first reading is the seed, never a trigger
    CHECK(!t.update(0, -90.0));
    CHECK_NEAR(t.floorDb(0), -90.0, 1e-12);
    CHECK_NEAR(t.triggerLevelDb(0), -82.0, 1e-12);
    CHECK(!t.update(0, -82.5));         // under the trigger; the quiet reading lets the floor rise by 0.1 dB
    CHECK_NEAR(t.floorDb(0), -89.9, 1e-12);
    CHECK(!t.update(2, -90.0));
    CHECK(t.update(2, -82.0));          // exactly floor + margin triggers
    CHECK(t.update(0, -80.0));
    // a carrier of 300 readings does not teach the floor
    for (int i = 0; i < 300; ++i) { CHECK(t.update(0, -50.0)); }
    CHECK_NEAR(t.floorDb(0), -89.9, 1e-12);
    // gone again: released and quiet noise reads as itself
    CHECK(!t.update(0, -90.0));
    // the floor follows DOWN at once
    CHECK(!t.update(0, -99.0));
    CHECK_NEAR(t.floorDb(0), -99.0, 1e-12);
    // and UP slowly: a rise to -95 is followed at 0.1 dB a reading, so after 40 readings it is there
    for (int i = 0; i < 40; ++i) { t.update(0, -95.0); }
    CHECK_NEAR(t.floorDb(0), -95.0, 1e-9);
    // slots are independent
    CHECK(!t.update(1, -110.0));
    CHECK_NEAR(t.floorDb(1), -110.0, 1e-12);
    CHECK(t.update(1, -101.0));
    CHECK(!t.triggered(0));
    CHECK(t.triggered(1));
    CHECK(t.floorDb(99) == ct::kNoDataDb);
    CHECK(t.triggerLevelDb(99) == INFINITY);
    t.reset();
    CHECK(t.slotCount() == 0);
}

void testTriggerDynamicNoise() {
    // Noise at -100 dB drifts up to -88 dB over 4000 readings with +-1.5 dB of jitter. A fixed trigger at -92 would fire for the second half of
    // the drift; the floor-relative one (margin 8) never fires, then fires on a +20 dB carrier at the end.
    ct::ScanTrigger rel, fixed;
    ct::ScanTriggerConfig cf;
    cf.mode = ct::TriggerMode::Fixed;
    cf.fixedDb = -92.0;
    cf.hysteresisDb = 0.0;
    CHECK(fixed.setConfig(cf));
    ct::ScanTriggerConfig cr;
    cr.riseDbPerReading = 0.02;  // as fast as the drift (12 dB over 4000 readings = 0.003 dB each): comfortably quicker than the drift
    CHECK(rel.setConfig(cr));
    Lcg rng(1);
    int relFalse = 0, fixedFalse = 0;
    for (int i = 0; i < 4000; ++i) {
        const double level = -100.0 + 12.0 * i / 4000.0 + 1.5 * rng.next();
        if (rel.update(0, level)) { ++relFalse; }
        if (fixed.update(0, level)) { ++fixedFalse; }
    }
    CHECK(relFalse == 0);
    CHECK(fixedFalse > 500);
    CHECK(rel.update(0, -88.0 + 20.0));
}

void testTriggerRefusals() {
    ct::ScanTrigger t;
    ct::ScanTriggerConfig c;
    c.marginDb = -1.0;
    CHECK(!t.setConfig(c));
    c = {};
    c.fixedDb = NAN;
    CHECK(!t.setConfig(c));
    c = {};
    c.hysteresisDb = 31.0;
    CHECK(!t.setConfig(c));
    CHECK(t.config().marginDb == 8.0);   // unchanged by the refusals
    // no data readings are ignored and do not teach the floor
    CHECK(!t.update(0, -95.0));
    CHECK(!t.update(0, NAN));
    CHECK(!t.update(0, -INFINITY));
    CHECK(!t.update(0, -250.0));
    CHECK_NEAR(t.floorDb(0), -95.0, 1e-12);
    CHECK(t.update(0, -80.0));
    CHECK(t.update(0, NAN));            // keeps the previous state
    CHECK(!t.update(ct::ScanTrigger::kMaxSlots, -50.0));
    CHECK(t.slotCount() == 1);
}

void testTriggerJson() {
    ct::ScanTrigger a, b;
    ct::ScanTriggerConfig c;
    c.mode = ct::TriggerMode::Fixed;
    c.fixedDb = -73.5;
    c.marginDb = 11.0;
    c.hysteresisDb = 3.0;
    c.riseDbPerReading = 0.25;
    CHECK(a.setConfig(c));
    std::string err;
    CHECK(b.configFromJson(a.configToJson(), &err));
    CHECK(b.config().mode == ct::TriggerMode::Fixed);
    CHECK_NEAR(b.config().fixedDb, -73.5, 0);
    CHECK_NEAR(b.config().marginDb, 11.0, 0);
    nlohmann::json j = a.configToJson();
    j["version"] = 2;
    CHECK(!b.configFromJson(j, &err));
    CHECK(err == "unknown version");
    j = a.configToJson();
    j["mode"] = "auto";
    CHECK(!b.configFromJson(j, &err));
    j = a.configToJson();
    j.erase("marginDb");
    CHECK(!b.configFromJson(j, &err));
    j = a.configToJson();
    j["hysteresisDb"] = "2";
    CHECK(!b.configFromJson(j, &err));
    j = a.configToJson();
    j["fixedDb"] = 500.0;
    CHECK(!b.configFromJson(j, &err));
    CHECK(!b.configFromJson(nlohmann::json::array(), &err));
    CHECK_NEAR(b.config().fixedDb, -73.5, 0);  // all refusals left it alone
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// FFT scan.
// ---------------------------------------------------------------------------------------------------------------------------------------------
struct Frame {
    std::vector<double> bins;
    double centre, span;
};

Frame makeFrame(std::size_t n, double centre, double span, double noiseDb, Lcg& rng) {
    Frame f{std::vector<double>(n), centre, span};
    for (double& v : f.bins) { v = noiseDb + 1.0 * rng.next(); }
    return f;
}
void addTone(Frame& f, double hz, double levelDb, double widthHz = 0.0) {
    const double binHz = f.span / static_cast<double>(f.bins.size());
    const double lo = f.centre - f.span / 2.0;
    for (std::size_t i = 0; i < f.bins.size(); ++i) {
        const double c = lo + (static_cast<double>(i) + 0.5) * binHz;
        if (std::fabs(c - hz) <= std::max(widthHz / 2.0, binHz / 2.0)) { f.bins[i] = std::max(f.bins[i], levelDb); }
    }
}

void testFftScan() {
    Lcg rng(7);
    Frame f = makeFrame(1024, 100e6, 2.048e6, -100.0, rng);  // 2 kHz bins
    addTone(f, 100.0375e6, -60.0);
    ct::FftScanGrid g{99.5e6, 100.5e6, 12500.0, 0.0};
    ct::FftScanOptions o;
    ct::FftScanResult r = ct::fftScan(f.bins.data(), f.bins.size(), f.centre, f.span, g, o);
    CHECK(r.ok);
    CHECK(r.channelsInSpan == 81);
    CHECK(r.channelsOutOfSpan == 0);
    CHECK(r.hits.size() == 1);
    if (r.hits.size() == 1) {
        CHECK_NEAR(r.hits[0].hz, 100.0375e6, 1.0);
        CHECK_NEAR(r.hits[0].levelDb, -60.0, 1e-9);
        CHECK(r.hits[0].aboveFloorDb > 35.0 && r.hits[0].aboveFloorDb < 42.0);
        CHECK(r.hits[0].channel == 43);
    }
    CHECK(r.floorReliable);
    CHECK(r.floorDb > -101.5 && r.floorDb < -98.5);

    // A signal 3.1 kHz off its channel centre (inside +-6.25 kHz) still lands in that channel and in no other.
    Frame f2 = makeFrame(1024, 100e6, 2.048e6, -100.0, rng);
    addTone(f2, 100.0375e6 + 3100.0, -55.0);
    r = ct::fftScan(f2.bins.data(), f2.bins.size(), f2.centre, f2.span, g, o);
    CHECK(r.hits.size() == 1);
    if (!r.hits.empty()) { CHECK_NEAR(r.hits[0].hz, 100.0375e6, 1.0); }

    // A span with a sample-rate error of 100 ppm and the same signal: still the same channel (the error is 200 Hz at the edge).
    r = ct::fftScan(f.bins.data(), f.bins.size(), f.centre, f.span * (1.0 + 100e-6), g, o);
    CHECK(r.hits.size() == 1);

    // A grid wider than the captured span: the channels outside are counted, not guessed.
    ct::FftScanGrid wide{98.0e6, 102.0e6, 12500.0, 0.0};
    r = ct::fftScan(f.bins.data(), f.bins.size(), f.centre, f.span, wide, o);
    CHECK(r.ok);
    CHECK(r.channelsInSpan + r.channelsOutOfSpan == 321);
    CHECK(r.channelsOutOfSpan > 150);
    CHECK(r.hits.size() == 1);

    // A wide signal over five channels: one merged hit at the strongest, or five when merging is off.
    Frame f3 = makeFrame(1024, 100e6, 2.048e6, -100.0, rng);
    addTone(f3, 100.0e6, -70.0, 50000.0);
    addTone(f3, 100.0125e6, -50.0);       // the strongest channel in the run
    r = ct::fftScan(f3.bins.data(), f3.bins.size(), f3.centre, f3.span, g, o);
    CHECK(r.hits.size() == 1);
    if (!r.hits.empty()) { CHECK_NEAR(r.hits[0].hz, 100.0125e6, 1.0); CHECK_NEAR(r.hits[0].levelDb, -50.0, 1e-9); }
    o.mergeAdjacent = false;
    r = ct::fftScan(f3.bins.data(), f3.bins.size(), f3.centre, f3.span, g, o);
    CHECK(r.hits.size() >= 4 && r.hits.size() <= 6);
    o.mergeAdjacent = true;

    // Two separated signals are two hits, in frequency order.
    Frame f4 = makeFrame(1024, 100e6, 2.048e6, -100.0, rng);
    addTone(f4, 99.6e6, -62.0);
    addTone(f4, 100.4e6, -64.0);
    r = ct::fftScan(f4.bins.data(), f4.bins.size(), f4.centre, f4.span, g, o);
    CHECK(r.hits.size() == 2);
    if (r.hits.size() == 2) { CHECK(r.hits[0].hz < r.hits[1].hz); }

    // Fixed mode: a -90 dB signal is under a -70 dB trigger and over a -95 dB one.
    Frame f5 = makeFrame(1024, 100e6, 2.048e6, -100.0, rng);
    addTone(f5, 100.2e6, -90.0);
    ct::FftScanOptions fx;
    fx.mode = ct::TriggerMode::Fixed;
    fx.fixedDb = -70.0;
    r = ct::fftScan(f5.bins.data(), f5.bins.size(), f5.centre, f5.span, g, fx);
    CHECK(r.ok && r.hits.empty());
    fx.fixedDb = -95.0;
    r = ct::fftScan(f5.bins.data(), f5.bins.size(), f5.centre, f5.span, g, fx);
    CHECK(r.hits.size() == 1);

    // More than half of the channels occupied: the median is signal; the floor is flagged unreliable.
    Frame f6 = makeFrame(1024, 100e6, 2.048e6, -60.0, rng);
    r = ct::fftScan(f6.bins.data(), f6.bins.size(), f6.centre, f6.span, g, fx);
    CHECK(r.ok);
    CHECK(!r.floorReliable);
    CHECK(r.hits.size() == 1);  // every channel over -95, all adjacent: merged
    CHECK(r.hits[0].aboveFloorDb == 0.0);

    // A grid finer than a bin: every in-span channel still takes a bin (nearest), so none is skipped.
    ct::FftScanGrid fine{99.9e6, 100.1e6, 500.0, 0.0};
    Frame f7 = makeFrame(256, 100e6, 2.048e6, -100.0, rng);   // 8 kHz bins
    addTone(f7, 100.0e6, -50.0);
    ct::FftScanOptions o7;
    o7.mergeAdjacent = false;
    r = ct::fftScan(f7.bins.data(), f7.bins.size(), f7.centre, f7.span, fine, o7);
    CHECK(r.ok);
    CHECK(r.channelsInSpan == 401);
    CHECK(r.hits.size() >= 8);   // the one 8 kHz bin spreads over 16 half-kHz channels (some on each side)

    // no data bins are never a hit nor the floor
    Frame f8 = makeFrame(1024, 100e6, 2.048e6, -100.0, rng);
    for (std::size_t i = 0; i < f8.bins.size(); i += 3) { f8.bins[i] = NAN; }
    r = ct::fftScan(f8.bins.data(), f8.bins.size(), f8.centre, f8.span, g, o);
    CHECK(r.ok && r.hits.empty());
    CHECK(r.floorDb > -102.0 && r.floorDb < -98.0);

    // failure paths
    CHECK(!ct::fftScan(nullptr, 0, 1e8, 1e6, g, o).ok);
    CHECK(!ct::fftScan(f.bins.data(), 1, 1e8, 1e6, g, o).ok);
    CHECK(!ct::fftScan(f.bins.data(), f.bins.size(), NAN, 1e6, g, o).ok);
    CHECK(!ct::fftScan(f.bins.data(), f.bins.size(), 1e8, 0.0, g, o).ok);
    CHECK(!ct::fftScan(f.bins.data(), f.bins.size(), 1e8, -5.0, g, o).ok);
    ct::FftScanGrid bad{99.5e6, 100.5e6, 0.0, 0.0};
    CHECK(!ct::fftScan(f.bins.data(), f.bins.size(), f.centre, f.span, bad, o).ok);
    bad = {99.5e6, 100.5e6, NAN, 0.0};
    CHECK(!ct::fftScan(f.bins.data(), f.bins.size(), f.centre, f.span, bad, o).ok);
    ct::FftScanGrid huge{0.0, 1e9, 1.0, 0.0};
    CHECK(!ct::fftScan(f.bins.data(), f.bins.size(), f.centre, f.span, huge, o).ok);
    // a swapped grid is put right; one channel (start == stop) works
    ct::FftScanGrid swapped{100.5e6, 99.5e6, 12500.0, 0.0};
    r = ct::fftScan(f.bins.data(), f.bins.size(), f.centre, f.span, swapped, o);
    CHECK(r.ok && r.channelsInSpan == 81);
    ct::FftScanGrid one{100.0375e6, 100.0375e6, 12500.0, 0.0};
    r = ct::fftScan(f.bins.data(), f.bins.size(), f.centre, f.span, one, o);
    CHECK(r.ok && r.channelsInSpan == 1);
    CHECK(r.hits.empty());   // one channel is its own median (floor = its level), so Relative cannot fire
    r = ct::fftScan(f.bins.data(), f.bins.size(), f.centre, f.span, one, fx);
    CHECK(r.hits.size() == 1);   // Fixed mode (-95 dB) does
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// Scan plan.
// ---------------------------------------------------------------------------------------------------------------------------------------------
ct::ScanRange rng(double a, double b, double step, const char* mode, double bw, const char* label = "") {
    ct::ScanRange r;
    r.startHz = a; r.stopHz = b; r.stepHz = step; r.mode = mode; r.bandwidthHz = bw; r.label = label;
    return r;
}

void testPlanRanges() {
    ct::ScanPlan p;
    std::string err;
    CHECK(p.addRange(rng(118e6, 118.1e6, 25e3, "AM", 8000, "air"), &err));
    CHECK(p.addRange(rng(144e6, 144.05e6, 12.5e3, "NFM", 12500, "2 m"), &err));
    auto pts = p.expand(&err);
    CHECK(pts.has_value());
    if (!pts) { return; }
    CHECK(pts->hz.size() == 5 + 5);
    CHECK_NEAR(pts->hz[0], 118e6, 0);
    CHECK_NEAR(pts->hz[4], 118.1e6, 0);
    CHECK_NEAR(pts->hz[5], 144e6, 0);
    CHECK(pts->range[0] == 0 && pts->range[9] == 1);
    // Backward reverses all of it
    p.setDirection(ct::ScanDirection::Backward);
    auto back = p.expand(&err);
    CHECK(back.has_value());
    CHECK_NEAR(back->hz[0], 144.05e6, 0);
    CHECK_NEAR(back->hz[9], 118e6, 0);
    CHECK(back->range[0] == 1 && back->range[9] == 0);
    // Overlapping ranges: a shared point appears once, at its first place
    ct::ScanPlan q;
    CHECK(q.addRange(rng(100e6, 100.05e6, 25e3, "NFM", 12500)));
    CHECK(q.addRange(rng(100.025e6, 100.1e6, 25e3, "AM", 8000)));
    auto o = q.expand();
    CHECK(o && o->hz.size() == 5);   // 100.000, 100.025, 100.050 + 100.075, 100.100
    if (o) { CHECK(o->range[1] == 0 && o->range[3] == 1); }
    // rangeOf: the first range that holds it
    CHECK(q.rangeOf(100.025e6)->mode == "NFM");
    CHECK(q.rangeOf(100.1e6)->mode == "AM");
    CHECK(q.rangeOf(99e6) == nullptr);
}

void testPlanLimits() {
    ct::ScanPlan p;
    std::string err;
    CHECK(!p.expand(&err).has_value());
    CHECK(err == "no range");
    // 16 ranges allowed, a 17th refused
    for (int i = 0; i < 16; ++i) { CHECK(p.addRange(rng(100e6 + i * 1e6, 100e6 + i * 1e6 + 1e3, 500, "NFM", 12500))); }
    CHECK(!p.addRange(rng(200e6, 201e6, 1e3, "NFM", 12500), &err));
    CHECK(err == "more than 16 ranges");
    CHECK(p.ranges().size() == 16);
    p.clear();
    // The point cap: exactly 100000 points allowed, 100001 refused (not truncated)
    CHECK(p.addRange(rng(10e6, 10e6 + 99999.0, 1.0, "AM", 8000)));
    auto a = p.expand(&err);
    CHECK(a && a->hz.size() == 100000);
    p.clear();
    CHECK(p.addRange(rng(10e6, 10e6 + 100000.0, 1.0, "AM", 8000)));
    CHECK(!p.expand(&err).has_value());
    CHECK(err == "more than 100000 points");
    // the cap counts the sum of the ranges too
    p.clear();
    CHECK(p.addRange(rng(10e6, 10e6 + 60000.0, 1.0, "AM", 8000)));
    CHECK(p.addRange(rng(20e6, 20e6 + 60000.0, 1.0, "AM", 8000)));
    CHECK(!p.expand(&err).has_value());
    // invalid ranges
    ct::ScanPlan v;
    CHECK(!v.addRange(rng(0, 100e6, 1e3, "AM", 8000)));
    CHECK(!v.addRange(rng(100e6, 99e6, 1e3, "AM", 8000)));
    CHECK(!v.addRange(rng(100e6, 101e6, 0.0, "AM", 8000)));
    CHECK(!v.addRange(rng(100e6, 101e6, -5.0, "AM", 8000)));
    CHECK(!v.addRange(rng(NAN, 101e6, 1e3, "AM", 8000)));
    CHECK(!v.addRange(rng(100e6, INFINITY, 1e3, "AM", 8000)));
    CHECK(!v.addRange(rng(100e6, 101e6, 1e3, "N FM", 8000)));
    CHECK(!v.addRange(rng(100e6, 101e6, 1e3, "", 8000)));
    CHECK(!v.addRange(rng(100e6, 101e6, 1e3, "VERYLONGMODE", 8000)));
    CHECK(!v.addRange(rng(100e6, 101e6, 1e3, "AM", 10.0)));
    CHECK(!v.addRange(rng(100e6, 101e6, 1e3, "AM", 8000, "bad\nlabel")));
    CHECK(!v.addRange(rng(100e6, 101e6, 1e3, "AM", 8000, std::string(65, 'x').c_str())));
    CHECK(v.ranges().empty());
    CHECK(v.addRange(rng(100e6, 100e6, 1e3, "AM", 8000)));   // start == stop: one point
    auto one = v.expand();
    CHECK(one && one->hz.size() == 1);
    CHECK(v.removeRange(0));
    CHECK(!v.removeRange(0));
}

void testPlanCursorAndScanner() {
    ct::ScanPlan p;
    CHECK(p.addRange(rng(118e6, 118.05e6, 25e3, "AM", 8000)));
    CHECK(p.addRange(rng(144e6, 144.0125e6, 12.5e3, "NFM", 12500)));
    ct::ScanCursor cur;
    cur.load(*p.expand());
    CHECK(cur.size() == 5);
    CHECK_NEAR(cur.current(), 118e6, 0);
    CHECK_NEAR(cur.next(), 118.025e6, 0);
    CHECK_NEAR(cur.back(), 118e6, 0);
    CHECK_NEAR(cur.back(), 144.0125e6, 0);     // back from the first wraps to the last
    CHECK_NEAR(cur.next(), 118e6, 0);          // and forward from the last wraps to the first
    cur.seek(144.0e6);
    CHECK(cur.index() == 3);
    // direction flips and the scan stays where it was
    p.setDirection(ct::ScanDirection::Backward);
    cur.load(*p.expand(), 144.0e6);
    CHECK_NEAR(cur.current(), 144.0e6, 0);
    CHECK_NEAR(cur.next(), 118.05e6, 0);       // backward order: ...144.0125, 144.0, 118.05, 118.025, 118.0
    ct::ScanCursor empty;
    CHECK(empty.empty() && empty.current() == 0.0 && empty.next() == 0.0 && empty.back() == 0.0);

    // The real Scanner walks the plan's list in the plan's order, then wraps
    cc::ScannerParams base;
    base.dwellMs = 10; base.holdMs = 0; base.resumeMs = 0;
    auto params = p.toParams(base);
    CHECK(params.has_value());
    if (!params) { return; }
    CHECK(params->list.size() == 5);
    CHECK_NEAR(params->startHz, 118e6, 0);
    CHECK_NEAR(params->stopHz, 144.0125e6, 0);
    cc::Scanner s;
    s.configure(*params);
    s.start(0.0);
    std::vector<double> got;
    for (double t = 0.0; t < 200.0 && got.size() < 7; t += 5.0) {
        if (auto f = s.tick(t, false)) { got.push_back(*f); }
    }
    const std::vector<double> want = {144.0125e6, 144.0e6, 118.05e6, 118.025e6, 118.0e6, 144.0125e6, 144.0e6};
    CHECK(got == want);
    // toParams of an empty plan is refused
    ct::ScanPlan none;
    CHECK(!none.toParams(base).has_value());
}

void testHitToBookmark() {
    ct::ScanPlan p;
    CHECK(p.addRange(rng(118e6, 137e6, 25e3, "AM", 8000)));
    CHECK(p.addRange(rng(144e6, 146e6, 12.5e3, "NFM", 12500)));
    std::vector<cc::Bookmark> have;
    auto b = ct::bookmarkFromHit(145.5e6, p, have);
    CHECK(b.has_value());
    if (b) {
        CHECK(b->name == "Scan 145.5000 MHz");
        CHECK(b->mode == "NFM");
        CHECK_NEAR(b->bandwidthHz, 12500, 0);
        CHECK(b->group == "Scan hits");
        CHECK(!b->favourite && !b->scan && b->id == 0);
        have.push_back(*b);
    }
    // the same hit again: refused; a different mode at the same frequency is not a duplicate
    CHECK(!ct::bookmarkFromHit(145.5e6, p, have).has_value());
    CHECK(!ct::bookmarkFromHit(145.5e6 + 0.9, p, have).has_value());
    CHECK(ct::bookmarkFromHit(145.5e6 + 5.0, p, have).has_value());
    cc::Bookmark am = have[0];
    am.mode = "AM";
    have.push_back(am);
    auto air = ct::bookmarkFromHit(125.1e6, p, have);
    CHECK(air && air->mode == "AM" && air->bandwidthHz == 8000);
    // outside every range: the first range's mode
    auto out = ct::bookmarkFromHit(300e6, p, {});
    CHECK(out && out->mode == "AM");
    // refusals
    CHECK(!ct::bookmarkFromHit(NAN, p, {}).has_value());
    CHECK(!ct::bookmarkFromHit(-1.0, p, {}).has_value());
    CHECK(!ct::bookmarkFromHit(0.0, p, {}).has_value());
    CHECK(!ct::bookmarkFromHit(1e14, p, {}).has_value());
    CHECK(!ct::bookmarkFromHit(145e6, ct::ScanPlan(), {}).has_value());
    CHECK(!ct::bookmarkFromHit(145e6, p, {}, "bad\ngroup").has_value());
    CHECK(ct::bookmarkFromHit(145e6, p, {}, "").has_value());   // an empty group is "ungrouped"
}

void testPlanJson() {
    ct::ScanPlan a;
    CHECK(a.addRange(rng(118e6, 137e6, 25e3, "AM", 8000, "Air \xC3\xA9")));
    CHECK(a.addRange(rng(144e6, 146e6, 12.5e3, "NFM", 12500)));
    a.setDirection(ct::ScanDirection::Backward);
    a.trigger().marginDb = 12.0;
    const nlohmann::json j = a.toJson();
    CHECK(j["version"] == 1);
    ct::ScanPlan b;
    std::string err;
    CHECK(b.fromJson(j, &err));
    CHECK(b.ranges().size() == 2);
    CHECK(b.direction() == ct::ScanDirection::Backward);
    CHECK(b.ranges()[0].label == "Air \xC3\xA9");
    CHECK_NEAR(b.ranges()[1].stepHz, 12500, 0);
    CHECK_NEAR(b.trigger().marginDb, 12.0, 0);   // the trigger is saved with the plan
    // text round trip
    ct::ScanPlan c;
    CHECK(c.fromJson(nlohmann::json::parse(j.dump())));
    CHECK(c.toJson() == j);
    // refusals leave the plan alone
    auto refuse = [&](nlohmann::json bad, const char* why) {
        ct::ScanPlan keep = b;
        std::string e;
        const bool ok = keep.fromJson(bad, &e);
        CHECK(!ok);
        CHECK(keep.toJson() == b.toJson());
        if (ok) { std::printf("  accepted: %s\n", why); }
    };
    nlohmann::json v = j; v["version"] = 2; refuse(v, "version 2");
    v = j; v["version"] = "1"; refuse(v, "version as text");
    v = j; v.erase("version"); refuse(v, "no version");
    v = j; v["direction"] = "sideways"; refuse(v, "direction");
    v = j; v.erase("trigger"); refuse(v, "no trigger");
    v = j; v["trigger"]["version"] = 9; refuse(v, "trigger version");
    v = j; v["ranges"][0]["stepHz"] = 0; refuse(v, "zero step");
    v = j; v["ranges"][0]["mode"] = 5; refuse(v, "mode as number");
    v = j; v["ranges"][1].erase("label"); refuse(v, "no label");
    v = j; v["ranges"] = "none"; refuse(v, "ranges text");
    v = j; v["ranges"] = nlohmann::json::array(); for (int i = 0; i < 17; ++i) { v["ranges"].push_back(j["ranges"][0]); } refuse(v, "17 ranges");
    refuse(nlohmann::json::array(), "array");
    refuse(nlohmann::json(), "null");
}

void fuzzPlanJson() {
    ct::ScanPlan a;
    CHECK(a.addRange(rng(118e6, 137e6, 25e3, "AM", 8000, "air")));
    CHECK(a.addRange(rng(144e6, 146e6, 12.5e3, "NFM", 12500)));
    const std::string base = a.toJson().dump();
    Lcg r(99);
    int accepted = 0, parsed = 0;
    const int kRuns = 30000;
    for (int i = 0; i < kRuns; ++i) {
        std::string s = base;
        const int edits = 1 + static_cast<int>(r.u32() % 4);
        for (int e = 0; e < edits; ++e) {
            switch (r.u32() % 4) {
                case 0: s[r.u32() % s.size()] = static_cast<char>(r.u32() & 0xFF); break;
                case 1: s.erase(r.u32() % s.size(), 1 + r.u32() % 8); break;
                case 2: s.insert(r.u32() % s.size(), 1, "{}[]\":,0123456789e-+."[r.u32() % 21]); break;
                default: s.resize(1 + r.u32() % s.size()); break;
            }
            if (s.empty()) { s = "x"; }
        }
        const nlohmann::json j = nlohmann::json::parse(s, nullptr, false);
        if (j.is_discarded()) { continue; }
        ++parsed;
        ct::ScanPlan p;
        if (p.fromJson(j)) {
            ++accepted;
            CHECK(p.ranges().size() <= ct::ScanPlan::kMaxRanges);
            for (const ct::ScanRange& rr : p.ranges()) { CHECK(ct::validRange(rr)); }
        }
    }
    std::printf("plan fuzz: %d runs, %d parsed, %d accepted\n", kRuns, parsed, accepted);
    CHECK(parsed > 1000);
}

}  // namespace

int main() {
    testTriggerFixed();
    testTriggerRelativeFloor();
    testTriggerDynamicNoise();
    testTriggerRefusals();
    testTriggerJson();
    testFftScan();
    testPlanRanges();
    testPlanLimits();
    testPlanCursorAndScanner();
    testHitToBookmark();
    testPlanJson();
    fuzzPlanJson();
    return testSummary("test_tuning_scan");
}
