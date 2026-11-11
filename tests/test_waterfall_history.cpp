// Live waterfall history (core/waterfall_history.hpp): ring capacity and memory cap, view window (live, paused, scrolled back, forward, seek by time),
// eviction while held, quantisation error, retune metadata, reset, no allocation in push/copyView, a producer/reader concurrency test that looks for torn
// lines, and a random-operation comparison with a plain reference model.
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <new>
#include <random>
#include <thread>
#include <vector>

#include "core/waterfall_history.hpp"
#include "test_check.hpp"

using namespace cascade::core::wfhistory;

static std::atomic<long> g_allocs{0};
static std::atomic<bool> g_armed{false};
void* operator new(std::size_t n) {
    if (g_armed.load(std::memory_order_relaxed)) { g_allocs.fetch_add(1, std::memory_order_relaxed); }
    void* p = std::malloc(n == 0 ? 1 : n);
    if (p == nullptr) { throw std::bad_alloc(); }
    return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

namespace {

Config smallCfg(std::size_t bins, std::size_t lines) {
    Config c; c.bins = bins; c.maxLines = lines; c.maxBytes = (bins + kLineOverheadBytes) * lines; return c;
}

// line i: every bin is a value that quantises to (i % 250); time i*100 ms
float valueOf(std::uint64_t i, const WaterfallHistory& h) { return -150.0f + static_cast<float>(i % 250) * h.dbStep(); }
void pushLine(WaterfallHistory& h, std::uint64_t i, std::vector<float>& buf) {
    std::fill(buf.begin(), buf.end(), valueOf(i, h));
    h.push(buf.data(), static_cast<std::int64_t>(i) * 100, 7.1e6, 2.4e6);
}
std::uint64_t idOf(const LineMeta& m) { return static_cast<std::uint64_t>(m.timeMs / 100); }

void testCapacityAndLive() {
    WaterfallHistory h(smallCfg(64, 100));
    CHECK(h.valid() && h.capacityLines() == 100 && h.bytesUsed() == 100 * (64 + 24));
    std::vector<float> buf(64);
    std::vector<std::uint8_t> out(64 * 20);
    std::vector<LineMeta> meta(20);
    CHECK(h.copyViewU8(20, out.data(), meta.data()) == 0);                          // empty
    for (std::uint64_t i = 0; i < 7; ++i) { pushLine(h, i, buf); }
    CHECK(h.copyViewU8(20, out.data(), meta.data()) == 7);                           // short history: fewer lines, oldest first from index 0
    for (std::size_t r = 0; r < 7; ++r) { CHECK(idOf(meta[r]) == r && out[r * 64] == r && out[r * 64 + 63] == r); }
    for (std::uint64_t i = 7; i < 55; ++i) { pushLine(h, i, buf); }
    CHECK(h.copyViewU8(20, out.data(), meta.data()) == 20);
    for (std::size_t r = 0; r < 20; ++r) { CHECK(idOf(meta[r]) == 35 + r && out[r * 64] == (35 + r) % 250); }
    // overflow: 3 x capacity
    for (std::uint64_t i = 55; i < 300; ++i) { pushLine(h, i, buf); }
    State s = h.state();
    CHECK(s.totalPushed == 300 && s.stored == 100 && s.oldestLine == 200 && s.newestLine == 299 && !s.held && s.behindLines == 0);
    std::vector<std::uint8_t> all(64 * 100);
    std::vector<LineMeta> am(100);
    CHECK(h.copyViewU8(100, all.data(), am.data()) == 100 && idOf(am[0]) == 200 && idOf(am[99]) == 299);
    CHECK(h.copyViewU8(1000, all.data(), am.data()) == 100);                          // asking for more than stored
    CHECK(h.copyViewU8(0, all.data(), am.data()) == 0 && h.copyViewU8(5, nullptr, nullptr) == 0);
    CHECK(h.copyViewU8(5, all.data(), nullptr) == 5);                                 // metadata is optional
    // floats are the quantised values
    std::vector<float> f(64 * 3);
    CHECK(h.copyView(3, f.data(), meta.data()) == 3);
    for (std::size_t r = 0; r < 3; ++r) { CHECK_NEAR(f[r * 64], valueOf(297 + r, h), 1e-3); }
}

void testPauseScroll() {
    WaterfallHistory h(smallCfg(8, 200));
    std::vector<float> buf(8);
    for (std::uint64_t i = 0; i < 100; ++i) { pushLine(h, i, buf); }
    std::vector<std::uint8_t> out(8 * 10);
    std::vector<LineMeta> meta(10);
    h.pause();
    State s = h.state();
    CHECK(s.held && s.viewNewest == 99 && s.behindLines == 0);
    for (std::uint64_t i = 100; i < 130; ++i) { pushLine(h, i, buf); }              // the view does not move while lines arrive
    CHECK(h.copyViewU8(10, out.data(), meta.data()) == 10 && idOf(meta[9]) == 99 && idOf(meta[0]) == 90);
    s = h.state();
    CHECK(s.held && s.behindLines == 30 && s.newestLine == 129);
    h.scrollBack(25, 10);
    CHECK(h.copyViewU8(10, out.data(), meta.data()) == 10 && idOf(meta[9]) == 74);
    CHECK(h.state().behindLines == 55);
    h.scrollForward(5);
    CHECK(h.copyViewU8(10, out.data(), meta.data()) == 10 && idOf(meta[9]) == 79);
    h.scrollForward(1000);                                                           // past the newest: back to live
    CHECK(!h.state().held);
    CHECK(h.copyViewU8(10, out.data(), meta.data()) == 10 && idOf(meta[9]) == 129);
    // scrollBack from live holds the view
    h.scrollBack(1, 10);
    CHECK(h.state().held && h.state().viewNewest == 128);
    // scrolling back past the oldest clamps to a full window at the oldest line
    h.scrollBack(100000, 10);
    CHECK(h.state().viewNewest == 9 && h.copyViewU8(10, out.data(), meta.data()) == 10 && idOf(meta[0]) == 0);
    h.jumpToLive();
    CHECK(!h.state().held && h.copyViewU8(10, out.data(), meta.data()) == 10 && idOf(meta[9]) == 129);
    // scrollForward / pause with nothing held or stored do nothing harmful
    h.scrollForward(3); CHECK(!h.state().held);
    WaterfallHistory e(smallCfg(8, 50));
    e.pause(); e.scrollBack(5, 10); e.scrollForward(5); e.jumpToLive();
    CHECK(!e.state().held && e.state().stored == 0 && e.copyViewU8(5, out.data()) == 0);
    // scroll back by zero lines holds without moving
    h.scrollBack(0, 10);
    CHECK(h.state().held && h.state().viewNewest == 129);
}

void testEvictionWhileHeld() {
    WaterfallHistory h(smallCfg(4, 50));
    std::vector<float> buf(4);
    for (std::uint64_t i = 0; i < 50; ++i) { pushLine(h, i, buf); }
    h.scrollBack(40, 10);                                                            // anchor at line 9 (the oldest full window)
    CHECK(h.state().viewNewest == 9 && !h.state().viewSlid);
    for (std::uint64_t i = 50; i < 55; ++i) { pushLine(h, i, buf); }                // lines 0..4 are overwritten: the window [0..9] is no longer whole
    std::vector<std::uint8_t> out(4 * 10);
    std::vector<LineMeta> meta(10);
    State s = h.state();
    CHECK(s.oldestLine == 5 && s.viewNewest == 9 && !s.viewSlid);
    CHECK(h.copyViewU8(10, out.data(), meta.data()) == 5 && idOf(meta[0]) == 5);   // a short window, never garbage
    for (std::uint64_t i = 55; i < 70; ++i) { pushLine(h, i, buf); }                // now the anchor line itself is gone
    s = h.state();
    CHECK(s.viewSlid && s.viewNewest == s.oldestLine && s.oldestLine == 20);
    CHECK(h.copyViewU8(10, out.data(), meta.data()) == 1 && idOf(meta[0]) == 20);
    h.scrollBack(1, 10);
    CHECK(!h.state().viewSlid || h.state().viewSlid);                                // the flag is only informative
    h.jumpToLive();
    CHECK(!h.state().viewSlid);
}

void testSeekByTime() {
    WaterfallHistory h(smallCfg(4, 100));
    std::vector<float> buf(4);
    for (std::uint64_t i = 0; i < 80; ++i) { pushLine(h, i, buf); }                  // times 0, 100, ..., 7900
    std::vector<std::uint8_t> out(4 * 5);
    std::vector<LineMeta> meta(5);
    CHECK(h.seekToTime(3000, 5) && h.state().held && h.state().viewNewest == 30);
    CHECK(h.copyViewU8(5, out.data(), meta.data()) == 5 && idOf(meta[4]) == 30);
    CHECK(h.seekToTime(3049, 5) && h.state().viewNewest == 30);                      // nearest
    CHECK(h.seekToTime(3051, 5) && h.state().viewNewest == 31);
    CHECK(h.seekToTime(-5000, 5) && h.state().viewNewest == 4);                      // before the oldest: the oldest full window
    CHECK(h.seekToTime(999999, 5) && !h.state().held);                               // after the newest: live
    for (std::uint64_t i = 80; i < 250; ++i) { pushLine(h, i, buf); }                // history now starts at line 150
    CHECK(h.seekToTime(3000, 5) && h.state().viewNewest == 154);                     // the line for t=3000 is gone: the oldest window
    WaterfallHistory e(smallCfg(4, 50));
    CHECK(!e.seekToTime(5, 5));
    // timestamps that go back are held at the previous time (so the search stays valid)
    WaterfallHistory b(smallCfg(4, 50));
    b.push(buf.data(), 1000, 1e6, 1e5); b.push(buf.data(), 500, 1e6, 1e5); b.push(buf.data(), 2000, 1e6, 1e5);
    std::vector<LineMeta> m3(3); std::vector<std::uint8_t> o3(12);
    CHECK(b.copyViewU8(3, o3.data(), m3.data()) == 3 && m3[0].timeMs == 1000 && m3[1].timeMs == 1000 && m3[2].timeMs == 2000);
}

void testQuantisation() {
    Config c = smallCfg(1000, 20);
    WaterfallHistory h(c);
    const float step = h.dbStep();
    CHECK_NEAR(step, 150.0 / 255.0, 1e-6);
    std::mt19937 g(3);
    std::vector<float> line(1000);
    std::uniform_real_distribution<float> d(-150.0f, 0.0f);
    for (float& v : line) { v = d(g); }
    h.push(line.data(), 0, 0, 0);
    std::vector<float> back(1000);
    CHECK(h.copyView(1, back.data()) == 1);
    double worst = 0;
    for (std::size_t i = 0; i < 1000; ++i) { worst = std::max(worst, static_cast<double>(std::fabs(back[i] - line[i]))); }
    std::printf("quantisation: worst error %.4f dB, half step %.4f dB\n", worst, step / 2);
    CHECK(worst <= step / 2 + 1e-4);
    // clamping, NaN and infinity
    const float odd[8] = {-200.0f, 20.0f, std::nanf(""), HUGE_VALF, -HUGE_VALF, -150.0f, 0.0f, -75.0f};
    WaterfallHistory h2(smallCfg(8, 20));
    h2.push(odd, 0, 0, 0);
    std::uint8_t q[8];
    CHECK(h2.copyViewU8(1, q) == 1);
    CHECK(q[0] == 0 && q[1] == 255 && q[2] == 0 && q[3] == 255 && q[4] == 0 && q[5] == 0 && q[6] == 255);
    CHECK(q[7] == 127 || q[7] == 128);
    // a custom range
    Config c3 = smallCfg(2, 20); c3.dbMin = -100.0f; c3.dbMax = -20.0f;
    WaterfallHistory h3(c3);
    const float two[2] = {-100.0f, -20.0f};
    h3.push(two, 0, 0, 0);
    std::uint8_t q3[2];
    h3.copyViewU8(1, q3);
    CHECK(q3[0] == 0 && q3[1] == 255);
}

void testMetaAndReset() {
    WaterfallHistory h(smallCfg(4, 30));
    std::vector<float> buf(4, -50.0f);
    h.push(buf.data(), 10, 7.1e6, 2.4e6);
    h.push(buf.data(), 20, 14.2e6, 1.0e6);                                            // a retune
    std::vector<std::uint8_t> out(8);
    std::vector<LineMeta> m(2);
    CHECK(h.copyViewU8(2, out.data(), m.data()) == 2);
    CHECK_NEAR(m[0].centerMhz, 7.1, 1e-4);
    CHECK_NEAR(m[1].centerMhz, 14.2, 1e-4);
    CHECK_NEAR(m[0].spanKhz, 2400.0, 1e-3);
    CHECK_NEAR(m[1].spanKhz, 1000.0, 1e-3);
    CHECK(m[0].epoch == m[1].epoch);
    const std::uint32_t e0 = h.state().epoch;
    h.pause();
    Config c2 = smallCfg(16, 30);
    CHECK(h.reset(c2));
    State s = h.state();
    CHECK(s.totalPushed == 0 && s.stored == 0 && !s.held && s.epoch == e0 + 1 && h.bins() == 16);
    std::vector<float> b16(16, -60.0f);
    h.push(b16.data(), 5, 1e6, 1e5);
    std::vector<std::uint8_t> o16(16);
    LineMeta lm;
    CHECK(h.copyViewU8(1, o16.data(), &lm) == 1 && lm.epoch == e0 + 1);
    // a reset to something invalid
    Config bad = smallCfg(0, 30);
    CHECK(!h.reset(bad) && !h.valid() && !h.push(b16.data(), 6, 0, 0) && h.copyViewU8(1, o16.data()) == 0);
}

void testInvalidAndCap() {
    Config c; c.bins = 0;
    CHECK(!WaterfallHistory(c).valid());
    c.bins = 4096; c.maxBytes = 1000;                                                 // cannot hold 16 lines
    CHECK(!WaterfallHistory(c).valid());
    c.maxBytes = (4096 + kLineOverheadBytes) * 16; CHECK(WaterfallHistory(c).valid());
    c.maxBytes = (4096 + kLineOverheadBytes) * 16 - 1; CHECK(!WaterfallHistory(c).valid());
    c = Config(); c.bins = (1u << 20) + 1; CHECK(!WaterfallHistory(c).valid());
    c = Config(); c.bins = 64; c.dbMin = 0; c.dbMax = 0; CHECK(!WaterfallHistory(c).valid());
    c = Config(); c.bins = 64; c.dbMin = std::nanf(""); CHECK(!WaterfallHistory(c).valid());
    WaterfallHistory ok(smallCfg(8, 20));
    CHECK(!ok.push(nullptr, 0, 0, 0));
    // the memory cap, with the numbers the header quotes
    for (std::size_t bins : {1024u, 4096u, 16384u}) {
        Config d; d.bins = bins;
        WaterfallHistory h(d);
        std::printf("memory cap 192 MiB: %zu bins -> %zu lines, %zu bytes used\n", bins, h.capacityLines(), h.bytesUsed());
        CHECK(h.valid() && h.bytesUsed() <= d.maxBytes && h.bytesUsed() + bins + kLineOverheadBytes > d.maxBytes - 0);
    }
    Config d; d.bins = 4096; WaterfallHistory h(d);
    CHECK(h.capacityLines() == 48000 || h.capacityLines() == (192u * 1024 * 1024) / (4096 + 24));
}

void testNoAllocation() {
    WaterfallHistory h(smallCfg(4096, 64));
    std::vector<float> line(4096, -70.0f);
    std::vector<std::uint8_t> out(4096 * 32);
    std::vector<float> fo(4096 * 32);
    std::vector<LineMeta> m(32);
    for (int i = 0; i < 70; ++i) { h.push(line.data(), i, 7e6, 2e6); }
    g_allocs = 0; g_armed = true;
    for (int i = 0; i < 500; ++i) {
        h.push(line.data(), 100 + i, 7e6, 2e6);
        h.copyViewU8(32, out.data(), m.data());
        h.copyView(32, fo.data(), m.data());
        if (i == 100) { h.pause(); }
        if (i == 200) { h.scrollBack(20, 32); }
        if (i == 300) { h.seekToTime(300, 32); }
        if (i == 400) { h.jumpToLive(); }
        (void)h.state();
    }
    g_armed = false;
    std::printf("allocations in 500 x (push, copyViewU8, copyView, state, view control): %ld\n", g_allocs.load());
    CHECK(g_allocs.load() == 0);
}

void testConcurrentTearing() {
    // the producer writes lines whose every bin and whose timestamp carry the same number; a window copied while it runs must hold contiguous lines
    // (numbers rising by one) with every bin equal inside a line
    WaterfallHistory h(smallCfg(256, 500));
    std::atomic<bool> stop{false};
    std::thread prod([&] {
        std::vector<float> buf(256);
        std::uint64_t i = 0;
        while (!stop.load()) { pushLine(h, i, buf); ++i; }
    });
    std::vector<std::uint8_t> out(256 * 40);
    std::vector<LineMeta> m(40);
    long windows = 0, torn = 0, nonContiguous = 0, heldWindows = 0;
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
    std::mt19937 g(1);
    while (std::chrono::steady_clock::now() < until) {
        const unsigned op = g() % 10;
        if (op == 0) { h.pause(); } else if (op == 1) { h.scrollBack(g() % 50, 40); } else if (op == 2) { h.jumpToLive(); } else if (op == 3) { h.scrollForward(g() % 30); }
        const std::size_t n = h.copyViewU8(40, out.data(), m.data());
        ++windows;
        if (h.state().held) { ++heldWindows; }
        for (std::size_t r = 0; r < n; ++r) {
            const std::uint8_t v0 = out[r * 256];
            for (std::size_t k = 1; k < 256; ++k) { if (out[r * 256 + k] != v0) { ++torn; break; } }
            if (v0 != idOf(m[r]) % 250) { ++torn; }
            if (r > 0 && idOf(m[r]) != idOf(m[r - 1]) + 1) { ++nonContiguous; }
        }
    }
    stop = true;
    prod.join();
    std::printf("concurrency: %ld windows (%ld while held), %ld torn lines, %ld gaps, %llu lines produced\n", windows, heldWindows, torn, nonContiguous,
                static_cast<unsigned long long>(h.state().totalPushed));
    CHECK(torn == 0 && nonContiguous == 0);
    CHECK(windows > 1000 && h.state().totalPushed > 1000);
}

void testRandomAgainstReference() {
    // a plain model: a deque of line ids, an optional anchor
    std::mt19937 g(555);
    const std::size_t cap = 60;
    WaterfallHistory h(smallCfg(4, cap));
    std::vector<float> buf(4);
    std::deque<std::uint64_t> ring;
    std::uint64_t total = 0;
    bool held = false;
    std::uint64_t anchor = 0;
    std::vector<std::uint8_t> out(4 * 25);
    std::vector<LineMeta> m(25);
    long compared = 0;
    bool ok = true;
    for (int step = 0; step < 60000; ++step) {
        switch (g() % 8) {
            case 0: case 1: case 2: case 3: {
                pushLine(h, total, buf);
                ring.push_back(total); if (ring.size() > cap) { ring.pop_front(); }
                ++total;
                if (held && anchor < ring.front()) { anchor = ring.front(); }
                break;
            }
            case 4: { h.pause(); if (!held && total > 0) { held = true; anchor = total - 1; } break; }
            case 5: {
                const std::size_t n = g() % 40, rows = 1 + g() % 25;
                h.scrollBack(n, rows);
                if (total > 0) {
                    if (!held) { held = true; anchor = total - 1; }
                    const std::uint64_t lowest = std::min<std::uint64_t>(ring.front() + rows - 1, total - 1);
                    anchor = anchor >= lowest + n ? anchor - n : lowest;
                }
                break;
            }
            case 6: {
                const std::size_t n = g() % 40;
                h.scrollForward(n);
                if (held) { if (anchor + n >= total - 1) { held = false; } else { anchor += n; } }
                break;
            }
            default: {
                if (g() % 4 == 0) { h.jumpToLive(); held = false; }
                const std::size_t rows = 1 + g() % 25;
                const std::size_t got = h.copyViewU8(rows, out.data(), m.data());
                std::size_t want = 0;
                std::uint64_t newest = 0;
                if (total > 0) { newest = held ? anchor : total - 1; want = std::min<std::uint64_t>(rows, newest + 1 - ring.front()); }
                ok = ok && got == want;
                for (std::size_t r = 0; r < got && r < want; ++r) { ok = ok && idOf(m[r]) == newest + 1 - want + r; }
                ++compared;
            }
        }
    }
    std::printf("randomised: %ld windows compared with the reference model\n", compared);
    CHECK(ok);
}

}  // namespace

int main() {
    testCapacityAndLive();
    testPauseScroll();
    testEvictionWhileHeld();
    testSeekByTime();
    testQuantisation();
    testMetaAndReset();
    testInvalidAndCap();
    testNoAllocation();
    testConcurrentTearing();
    testRandomAgainstReference();
    return testSummary("test_waterfall_history");
}
