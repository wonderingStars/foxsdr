// The scope node model (src/dsp/scope_model.hpp): trigger edge, level, hysteresis, hold-off and pre-trigger; Auto, Normal and Single; min/max
// decimation; the delivered frame outliving the ring. Generated signals with noise.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <limits>
#include <random>

#include "dsp/scope_model.hpp"
#include "dspmodels_alloc.hpp"
#include "dspmodels_util.hpp"
#include "test_check.hpp"

using namespace cascade::dsp;

namespace {

std::vector<float> sine(std::size_t n, double periodSamples, double amp = 1.0, double phase = 0.0, double noise = 0.0, std::uint64_t seed = 1) {
    std::mt19937_64 rng(seed);
    std::normal_distribution<double> g(0.0, 1.0);
    std::vector<float> v(n);
    for (std::size_t i = 0; i < n; ++i) { v[i] = static_cast<float>(amp * std::sin(dspm::kTwoPi * static_cast<double>(i) / periodSamples + phase) + noise * g(rng)); }
    return v;
}

ScopeSettings base() {
    ScopeSettings s;
    s.window = 400;
    s.preFraction = 0.25;
    s.level = 0.0;
    s.hysteresis = 0.05;
    return s;
}

void testRisingTrigger() {
    ScopeModel m(1u << 14);
    ScopeSettings s = base();
    s.mode = ScopeMode::Normal;
    m.setSettings(s);
    const std::vector<float> x = sine(5000, 100.0, 1.0, 0.7, 0.002);
    m.push(x.data(), x.size());
    CHECK(m.frameReady() && m.frameTriggered());
    CHECK(m.frameLength() == 400);
    CHECK(m.frameTriggerIndex() == 100);  // 25 % of 400
    float before, at;
    CHECK(m.frameSample(99, &before) && m.frameSample(100, &at));
    std::printf("MEAS rising trigger at level 0: sample before %.4f, at the trigger %.4f\n", before, at);
    CHECK(before < 0.0f && at >= 0.0f);  // the crossing sits exactly at the pre-trigger index
    // The frame is a straight copy of the stream around the crossing: the whole frame is one clean sine, 4 periods.
    float lo = 9, hi = -9;
    for (std::size_t j = 0; j < 400; ++j) {
        float v;
        m.frameSample(j, &v);
        lo = std::min(lo, v);
        hi = std::max(hi, v);
    }
    CHECK(lo < -0.99f && hi > 0.99f);
    // Falling edge: the sample at the trigger index is the first at or below the level, and the one before is above it.
    ScopeModel f(1u << 14);
    s.edge = ScopeEdge::Falling;
    f.setSettings(s);
    f.push(x.data(), x.size());
    CHECK(f.frameReady());
    f.frameSample(99, &before);
    f.frameSample(100, &at);
    CHECK(before > 0.0f && at <= 0.0f);
    // A level other than zero.
    ScopeModel l(1u << 14);
    s.edge = ScopeEdge::Rising;
    s.level = 0.5;
    l.setSettings(s);
    l.push(x.data(), x.size());
    l.frameSample(99, &before);
    l.frameSample(100, &at);
    CHECK(before < 0.5f && at >= 0.5f);
    // Pre-trigger 0 and 1: the trigger is the first / last sample (the last leaves no post-trigger samples to wait for).
    for (double pre : {0.0, 1.0}) {
        ScopeModel p(1u << 14);
        s.level = 0.0;
        s.preFraction = pre;
        p.setSettings(s);
        p.push(x.data(), x.size());
        CHECK(p.frameReady());
        CHECK(p.frameTriggerIndex() == (pre == 0.0 ? 0u : 399u));
        float a, b;
        const std::size_t ti = p.frameTriggerIndex();
        p.frameSample(ti, &b);
        CHECK(b >= 0.0f);
        if (ti > 0) { p.frameSample(ti - 1, &a); CHECK(a < 0.0f); }
    }
}

void testHysteresisAndHoldoff() {
    // A trace that chatters around the level: with no hysteresis it fires on every wobble; with 0.2 only on the real rising edges.
    std::mt19937_64 rng(5);
    std::normal_distribution<double> g(0.0, 0.03);
    std::vector<float> x;
    for (int cycle = 0; cycle < 40; ++cycle) {
        for (int i = 0; i < 100; ++i) { x.push_back(static_cast<float>((i < 50 ? -1.0 : 1.0) + g(rng))); }
    }
    // chatter right at the edge of every cycle
    std::vector<float> y;
    for (int cycle = 0; cycle < 40; ++cycle) {
        for (int i = 0; i < 100; ++i) {
            double v = (i < 50 ? -1.0 : 1.0);
            if (i >= 49 && i < 75) { v = 0.02 * ((i % 2) ? 1 : -1) + g(rng) * 0.2; }  // dithering around the level
            y.push_back(static_cast<float>(v));
        }
    }
    auto frames = [&](double hyst, std::size_t holdoff, std::size_t window = 60) {
        ScopeModel m(1u << 14);
        ScopeSettings s = base();
        s.mode = ScopeMode::Normal;
        s.window = window;
        s.hysteresis = hyst;
        s.holdoff = holdoff;
        m.setSettings(s);
        m.push(y.data(), y.size());
        return m.frameCount();
    };
    const auto none = frames(0.0, 0, 8), with = frames(0.2, 0, 8);
    std::printf("MEAS chattering edge, 40 cycles, window 8: frames with hysteresis 0 = %llu, with 0.2 = %llu\n", static_cast<unsigned long long>(none),
                static_cast<unsigned long long>(with));
    CHECK(none > 50);
    CHECK(with == 39 || with == 40);  // one per cycle (the first needs a full cycle to arm)
    // Hold-off: a long hold-off thins the triggers to one per `holdoff` samples.
    const auto h = frames(0.2, 1000);
    CHECK(h >= 3 && h <= 5);  // 4000 samples / 1000
    CHECK(frames(0.2, 100000) <= 1);
}

void testModes() {
    const std::vector<float> flat(20000, 0.3f);  // never crosses
    ScopeSettings s = base();
    ScopeModel normal(1u << 14), autoM(1u << 14), single(1u << 14);
    s.mode = ScopeMode::Normal;
    normal.setSettings(s);
    normal.push(flat.data(), flat.size());
    CHECK(!normal.frameReady() && normal.frameCount() == 0);  // Normal waits for a trigger
    float tmp[8];
    CHECK(!normal.frame(8, tmp, tmp));
    // Auto: after the time-out (2 x window) a free-running frame ending at the newest sample is delivered, flagged untriggered.
    s.mode = ScopeMode::Auto;
    autoM.setSettings(s);
    autoM.push(flat.data(), flat.size());
    CHECK(autoM.frameReady() && !autoM.frameTriggered());
    CHECK(autoM.frameCount() == 20000 / 800);
    float mn[4], mx[4];
    CHECK(autoM.frame(4, mn, mx));
    CHECK(mn[0] == 0.3f && mx[3] == 0.3f);
    // A custom time-out.
    ScopeModel quick(1u << 14);
    s.autoTimeout = 410;
    quick.setSettings(s);
    quick.push(flat.data(), 1000);
    CHECK(quick.frameCount() == 2);
    // Single: one frame, then nothing until arm().
    s.mode = ScopeMode::Single;
    s.autoTimeout = 0;
    single.setSettings(s);
    const std::vector<float> x = sine(20000, 100.0, 1.0, 0.7);
    single.push(x.data(), x.size());
    CHECK(single.frameCount() == 1);
    single.acknowledge();
    CHECK(!single.frameReady());
    single.push(x.data(), x.size());
    CHECK(single.frameCount() == 1);
    single.arm();
    CHECK(!single.frameReady());
    single.push(x.data(), 3000);
    CHECK(single.frameCount() == 2 && single.frameReady());
    // Normal keeps showing the last frame (reading does not consume it) and a newer one replaces it.
    ScopeModel keep(1u << 14);
    s.mode = ScopeMode::Normal;
    keep.setSettings(s);
    keep.push(x.data(), 1000);
    float a0, a1;
    keep.frameSample(5, &a0);
    keep.frameSample(5, &a1);
    CHECK(a0 == a1);
    CHECK(keep.frame(4, mn, mx) && keep.frame(4, mn, mx));
}

void testDecimation() {
    // A ramp in a window of 400: each of 8 columns holds 50 samples; min is the first of them, max the last.
    ScopeModel m(1u << 14);
    ScopeSettings s = base();
    s.mode = ScopeMode::Auto;
    s.autoTimeout = 400;
    s.window = 400;
    m.setSettings(s);
    std::vector<float> ramp(2000);
    for (std::size_t i = 0; i < ramp.size(); ++i) { ramp[i] = static_cast<float>(i); }
    m.push(ramp.data(), 400);  // exactly the time-out: a free-running frame of samples 0 .. 399
    CHECK(m.frameReady());
    float mn[8], mx[8];
    CHECK(m.frame(8, mn, mx));
    for (int c = 0; c < 8; ++c) {
        CHECK(mn[c] == static_cast<float>(c * 50));
        CHECK(mx[c] == static_cast<float>(c * 50 + 49));
    }
    // A one-sample spike survives any decimation: it is the max of its column.
    ScopeModel sp(1u << 14);
    sp.setSettings(s);
    std::vector<float> spike(400, 0.1f);
    spike[237] = 5.0f;
    sp.push(spike.data(), 400);
    for (std::size_t columns : {std::size_t{1}, std::size_t{7}, std::size_t{50}, std::size_t{399}, std::size_t{400}}) {
        std::vector<float> a(columns), b(columns);
        CHECK(sp.frame(columns, a.data(), b.data()));
        float top = 0;
        for (float v : b) { top = std::max(top, v); }
        CHECK(top == 5.0f);
        CHECK(*std::min_element(a.begin(), a.end()) == 0.1f);
        // and it is in the right place: the column whose range [c*400/columns, (c+1)*400/columns) holds sample 237
        const std::size_t col = static_cast<std::size_t>(std::max_element(b.begin(), b.end()) - b.begin());
        CHECK(col * 400 / columns <= 237 && 237 < std::max((col + 1) * 400 / columns, col * 400 / columns + 1));
    }
    // More columns than samples: every column repeats the nearest sample (min == max).
    std::vector<float> a(1000), b(1000);
    CHECK(sp.frame(1000, a.data(), b.data()));
    bool same = true;
    for (std::size_t i = 0; i < 1000; ++i) { same = same && a[i] == b[i]; }
    CHECK(same);
    const std::size_t at1000 = static_cast<std::size_t>(std::max_element(b.begin(), b.end()) - b.begin());
    CHECK(at1000 * 400 / 1000 == 237);  // the column that repeats sample 237
    // Bad calls.
    CHECK(!sp.frame(0, a.data(), b.data()));
    CHECK(!sp.frame(8, nullptr, b.data()));
    ScopeModel empty(1u << 14);
    CHECK(!empty.frame(8, a.data(), b.data()));
}

void testFrameSurvivesTheRing() {
    ScopeModel m(256);
    ScopeSettings s = base();
    s.mode = ScopeMode::Single;
    s.window = 100;
    m.setSettings(s);
    const std::vector<float> x = sine(400, 40.0, 1.0, 0.7);
    m.push(x.data(), x.size());
    CHECK(m.frameCount() == 1);
    std::vector<float> first(100);
    for (std::size_t j = 0; j < 100; ++j) { m.frameSample(j, &first[j]); }
    const std::vector<float> junk(100000, 123.0f);
    m.push(junk.data(), junk.size());  // the ring has turned many times over
    for (std::size_t j = 0; j < 100; ++j) {
        float v;
        m.frameSample(j, &v);
        CHECK(v == first[j]);
    }
    // The window is clamped to half the ring; the ring is rounded up to a power of two.
    ScopeSettings big = s;
    big.window = 100000;
    m.setSettings(big);
    CHECK(m.settings().window == 128);
    ScopeModel odd(1000);
    odd.setSettings(big);
    CHECK(odd.settings().window == 512);
}

void testInputKinds() {
    using Cf = std::complex<float>;
    std::vector<Cf> iq(2000);
    for (std::size_t i = 0; i < iq.size(); ++i) { iq[i] = std::polar(0.5f, static_cast<float>(dspm::kTwoPi * i / 100.0)); }
    ScopeSettings s = base();
    s.mode = ScopeMode::Normal;
    ScopeModel mag(1u << 12), ph(1u << 12), q(1u << 12);
    mag.setSettings(s);
    ph.setSettings(s);
    s.level = 0.2;
    q.setSettings(s);
    mag.pushIq(iq.data(), iq.size(), ScopeTrace::Magnitude);
    CHECK(!mag.frameReady());  // a constant magnitude never crosses level 0
    ph.pushIq(iq.data(), iq.size(), ScopeTrace::Phase);
    CHECK(ph.frameReady());    // the phase wraps from +pi to -pi once a cycle: a rising crossing of 0 exists
    q.pushIq(iq.data(), iq.size(), ScopeTrace::Q);
    CHECK(q.frameReady());
    float v;
    q.frameSample(q.frameTriggerIndex(), &v);
    CHECK(v >= 0.2f);
    // Non-finite samples read as 0; null is ignored.
    ScopeModel n(1u << 12);
    n.setSettings(base());
    std::vector<float> bad = {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), -1.0f, 1.0f};
    n.push(bad.data(), bad.size());
    n.push(nullptr, 5);
    n.pushIq(nullptr, 5, ScopeTrace::I);
    // Settings are repaired.
    ScopeSettings w;
    w.preFraction = std::numeric_limits<double>::quiet_NaN();
    w.hysteresis = -4.0;
    w.window = 0;
    n.setSettings(w);
    CHECK(n.settings().preFraction == 0.25 && n.settings().hysteresis == 0.0 && n.settings().window == 2);
}

void testChunkingAndAlloc() {
    const std::vector<float> x = sine(30000, 137.0, 0.8, 0.3, 0.01);
    auto run = [&](std::size_t block) {
        ScopeModel m(1u << 14);
        ScopeSettings s = base();
        s.mode = ScopeMode::Normal;
        s.level = 0.1;
        s.holdoff = 20;
        m.setSettings(s);
        for (std::size_t p = 0; p < x.size(); p += block) { m.push(x.data() + p, std::min(block, x.size() - p)); }
        std::vector<float> out(400);
        for (std::size_t j = 0; j < 400; ++j) { m.frameSample(j, &out[j]); }
        return std::make_pair(m.frameCount(), out);
    };
    const auto whole = run(30000);
    CHECK(run(1) == whole && run(7) == whole && run(1000) == whole);
    ScopeModel m(1u << 14);
    m.setSettings(base());
    DspmAllocProbe probe;
    for (int i = 0; i < 30; ++i) { m.push(x.data(), 4096); }
    float a[16], b[16];
    m.frame(16, a, b);
    CHECK(probe.news() == 0);
}

}  // namespace

int main() {
    testRisingTrigger();
    testHysteresisAndHoldoff();
    testModes();
    testDecimation();
    testFrameSurvivesTheRing();
    testInputKinds();
    testChunkingAndAlloc();
    return testSummary("test_scope_model");
}
