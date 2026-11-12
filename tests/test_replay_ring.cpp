// Tests for the replay ring (core/replay_ring.hpp): wrap-around, shorter-than-N, exact-N, resize,
// the gate bits and "skip silence" copies, quantisation, a concurrent writer and reader, and
// "no allocation in write()".
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/replay_ring.hpp"

#include "alloc_probe.hpp"
#include "test_check.hpp"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <limits>
#include <thread>
#include <vector>

namespace {

using cascade::core::ReplayRing;
constexpr double kRate = 48000.0;

// A ramp whose sample number n is the value (n mod 20000) / 32768: any sample's value says which sample it is.
float rampValue(std::uint64_t n) { return static_cast<float>(n % 20000) / 32768.0f; }
std::int16_t rampInt(std::uint64_t n) { return static_cast<std::int16_t>(n % 20000); }

void writeRamp(ReplayRing& r, std::uint64_t from, std::size_t n, bool gate = true) {
    std::vector<float> v(n);
    for (std::size_t i = 0; i < n; ++i) { v[i] = rampValue(from + i); }
    r.write(v.data(), n, gate);
}

void testBasics() {
    ReplayRing r;
    CHECK(r.capacity() == 0);
    CHECK(r.head() == 0 && r.oldest() == 0);
    // Writes to a ring with no capacity are ignored; copies return nothing.
    float x[4] = {0.1f, 0.2f, 0.3f, 0.4f};
    r.write(x, 4, true);
    CHECK(r.head() == 0);
    std::int16_t out[8] = {};
    CHECK(r.copyLast(4, out) == 0);
    CHECK(r.copyRange(0, 4, out) == 0);
    CHECK(!r.gateOpenAt(0));
    // resize: refused inputs change nothing.
    CHECK(!r.resize(0.0, 1.0));
    CHECK(!r.resize(-1.0, 1.0));
    CHECK(!r.resize(kRate, std::numeric_limits<double>::quiet_NaN()));
    CHECK(!r.resize(kRate, ReplayRing::kMaxSeconds + 1));
    CHECK(!r.resize(std::numeric_limits<double>::infinity(), 1.0));
    CHECK(r.capacity() == 0);
    CHECK(r.resize(kRate, 1.0));
    CHECK(r.capacity() == 48128);  // 48000 rounded up to a whole 1024 block
    CHECK_NEAR(r.seconds(), 48128.0 / 48000.0, 1e-9);
    CHECK(r.rateHz() == kRate);
}

void testShorterExactAndWrap() {
    ReplayRing r;
    r.resize(kRate, 1.0);
    const std::size_t cap = r.capacity();
    std::vector<std::int16_t> out(cap + 10);
    // SHORTER than N: 1000 samples written, ask for 5000: get 1000.
    writeRamp(r, 0, 1000);
    std::uint64_t first = 99;
    CHECK(r.copyLast(5000, out.data(), &first) == 1000);
    CHECK(first == 0);
    for (std::size_t i = 0; i < 1000; ++i) { CHECK(out[i] == rampInt(i)); }
    // EXACT: ask for exactly what is held.
    CHECK(r.copyLast(1000, out.data(), &first) == 1000 && first == 0);
    // A part: the newest 300.
    CHECK(r.copyLast(300, out.data(), &first) == 300 && first == 700);
    CHECK(out[0] == rampInt(700) && out[299] == rampInt(999));
    // Fill to exactly capacity.
    writeRamp(r, 1000, cap - 1000);
    CHECK(r.head() == cap && r.oldest() == 0);
    CHECK(r.copyLast(cap, out.data(), &first) == cap && first == 0);
    CHECK(out[0] == rampInt(0) && out[cap - 1] == rampInt(cap - 1));
    // WRAP: write 5000 more; the oldest 5000 are gone, the rest is intact and in order.
    writeRamp(r, cap, 5000);
    CHECK(r.head() == cap + 5000);
    CHECK(r.oldest() == 5000);
    const std::size_t got = r.copyLast(cap + 100, out.data(), &first);  // asks for more than held
    CHECK(got == cap && first == 5000);
    bool ok = true;
    for (std::size_t i = 0; i < got; ++i) { ok = ok && (out[i] == rampInt(first + i)); }
    CHECK(ok);
    // copyRange of something wholly overwritten returns nothing; one that overlaps starts at the oldest held sample.
    CHECK(r.copyRange(0, 100, out.data(), &first) == 0);
    CHECK(r.copyRange(4950, 100, out.data(), &first) == 50 && first == 5000);
    CHECK(out[0] == rampInt(5000) && out[49] == rampInt(5049));
    // A range wholly in the future or wholly gone: zero.
    CHECK(r.copyRange(r.head(), 10, out.data()) == 0);
    CHECK(r.copyRange(r.head() + 100, 10, out.data()) == 0);
    // Many wraps with odd write sizes keep every sample right.
    ReplayRing q;
    q.resize(kRate, 0.5);
    std::uint64_t pos = 0;
    for (int i = 0; i < 200; ++i) {
        const std::size_t n = 1 + static_cast<std::size_t>((i * 7919) % 4097);
        writeRamp(q, pos, n);
        pos += n;
    }
    std::vector<std::int16_t> o2(q.capacity());
    const std::size_t g2 = q.copyLast(q.capacity(), o2.data(), &first);
    CHECK(g2 == q.capacity() && first == pos - q.capacity());
    ok = true;
    for (std::size_t i = 0; i < g2; ++i) { ok = ok && (o2[i] == rampInt(first + i)); }
    CHECK(ok);
    // A single write bigger than kMaxWriteChunk and bigger than the capacity.
    ReplayRing big;
    big.resize(kRate, 0.5);  // 24576 samples
    writeRamp(big, 0, 70000);
    CHECK(big.head() == 70000);
    std::vector<std::int16_t> o3(big.capacity());
    CHECK(big.copyLast(big.capacity(), o3.data(), &first) == big.capacity() && first == 70000 - big.capacity());
    CHECK(o3[0] == rampInt(first) && o3.back() == rampInt(69999));
    // n == 0 and null out.
    CHECK(r.copyLast(0, out.data()) == 0);
    CHECK(r.copyRange(0, 5, nullptr) == 0);
    r.write(nullptr, 5, true);
    CHECK(r.head() == cap + 5000);
}

void testResize() {
    ReplayRing r;
    r.resize(kRate, 1.0);
    writeRamp(r, 0, 100000);  // wrapped: holds the newest 48128
    const std::uint64_t head = r.head();
    // Grow: nothing lost, head unchanged, everything still readable and correct.
    CHECK(r.resize(kRate, 3.0));
    CHECK(r.head() == head);
    CHECK(r.oldest() == head - 48128);
    std::vector<std::int16_t> out(200000);
    std::uint64_t first = 0;
    std::size_t got = r.copyLast(200000, out.data(), &first);
    CHECK(got == 48128 && first == head - 48128);
    bool ok = true;
    for (std::size_t i = 0; i < got; ++i) { ok = ok && (out[i] == rampInt(first + i)); }
    CHECK(ok);
    // Keep writing after the grow; the ring fills to its new size.
    writeRamp(r, head, 200000);
    CHECK(r.oldest() == r.head() - r.capacity());
    // Shrink: the NEWEST audio is what stays.
    const std::uint64_t h2 = r.head();
    CHECK(r.resize(kRate, 0.25));
    CHECK(r.head() == h2);
    CHECK(r.capacity() == 12288);
    got = r.copyLast(r.capacity(), out.data(), &first);
    CHECK(got == 12288 && first == h2 - 12288);
    ok = true;
    for (std::size_t i = 0; i < got; ++i) { ok = ok && (out[i] == rampInt(first + i)); }
    CHECK(ok);
    // Off: memory freed, writes ignored, head kept.
    CHECK(r.resize(kRate, 0.0));
    CHECK(r.capacity() == 0 && r.head() == h2);
    writeRamp(r, h2, 100);
    CHECK(r.head() == h2);
    CHECK(r.copyLast(10, out.data()) == 0);
    // On again: starts empty at the old head (nothing older exists).
    CHECK(r.resize(kRate, 1.0));
    CHECK(r.oldest() == r.head());
    writeRamp(r, h2, 500);
    CHECK(r.copyLast(2000, out.data(), &first) == 500 && first == h2);
    // Same length again keeps everything; a RATE change discards the audio but keeps the head.
    CHECK(r.resize(kRate, 1.0));
    CHECK(r.copyLast(2000, out.data(), &first) == 500);
    CHECK(r.resize(44100.0, 1.0));
    CHECK(r.oldest() == r.head());
    CHECK(r.copyLast(2000, out.data()) == 0);
    // Resize of an empty ring (nothing written yet) is fine.
    ReplayRing e;
    CHECK(e.resize(kRate, 2.0) && e.head() == 0 && e.oldest() == 0);
    CHECK(e.resize(kRate, 1.0) && e.resize(kRate, 4.0));
}

void testGateBitsAndSkipSilence() {
    ReplayRing r;
    r.resize(kRate, 1.0);
    const std::size_t B = ReplayRing::kGateBlock;
    // 8 blocks: open, closed, closed, open, closed, open(partial: only the second half of the block), closed, open.
    const bool gate[8] = {true, false, false, true, false, false, false, true};
    for (int b = 0; b < 8; ++b) {
        if (b == 5) {
            writeRamp(r, b * B, B / 2, false);
            writeRamp(r, b * B + B / 2, B / 2, true);  // gate opens mid-block: the block counts as open
        } else {
            writeRamp(r, b * B, B, gate[b]);
        }
    }
    const bool expectOpen[8] = {true, false, false, true, false, true, false, true};
    for (int b = 0; b < 8; ++b) {
        CHECK(r.gateOpenAt(b * B) == expectOpen[b]);
        CHECK(r.gateOpenAt(b * B + B - 1) == expectOpen[b]);
    }
    CHECK(!r.gateOpenAt(8 * B));      // not written yet
    CHECK(!r.gateOpenAt(1000000));
    // Skip silence: only the open blocks, packed, in order.
    std::vector<std::int16_t> out(8 * B);
    const std::size_t got = r.copyGated(0, 8 * B, out.data(), out.size());
    CHECK(got == 4 * B);
    bool ok = true;
    const int openBlocks[4] = {0, 3, 5, 7};
    for (int k = 0; k < 4; ++k) {
        for (std::size_t i = 0; i < B; ++i) { ok = ok && (out[k * B + i] == rampInt(openBlocks[k] * B + i)); }
    }
    CHECK(ok);
    // A range that starts mid-block and stops mid-block: partial first/last pieces, by their block's bit.
    const std::size_t got2 = r.copyGated(3 * B + 100, 300, out.data(), out.size());
    CHECK(got2 == 300 && out[0] == rampInt(3 * B + 100));
    CHECK(r.copyGated(1 * B, 2 * B, out.data(), out.size()) == 0);  // all closed
    CHECK(r.copyGated(0, 8 * B, out.data(), B) == B);               // output capacity limits the copy
    CHECK(r.copyGated(0, 0, out.data(), out.size()) == 0);
    // A block that has wrapped out of the ring is not reported open.
    writeRamp(r, 8 * B, r.capacity(), false);
    CHECK(!r.gateOpenAt(0));
    // The gate bit is reset when the ring reuses the block: all-closed writes leave all blocks closed.
    CHECK(!r.gateOpenAt(r.head() - 1));
    // Gate bits survive a resize for the samples kept.
    ReplayRing s;
    s.resize(kRate, 1.0);
    writeRamp(s, 0, B, true);
    writeRamp(s, B, B, false);
    s.resize(kRate, 2.0);
    CHECK(s.gateOpenAt(10) && !s.gateOpenAt(B + 10));
}

void testQuantisation() {
    ReplayRing r;
    r.resize(kRate, 0.1);
    const float in[9] = {0.0f, 1.0f, -1.0f, 2.5f, -7.0f, 0.5f, std::numeric_limits<float>::quiet_NaN(), 1.0f / 32768.0f, -0.5f / 32768.0f};
    r.write(in, 9, true);
    std::int16_t out[9] = {};
    CHECK(r.copyLast(9, out) == 9);
    CHECK(out[0] == 0);
    CHECK(out[1] == 32767);    // +1.0 saturates, as core::Recorder does
    CHECK(out[2] == -32768);   // -1.0
    CHECK(out[3] == 32767);    // clamped
    CHECK(out[4] == -32768);
    CHECK(out[5] == 16384);
    CHECK(out[6] == 0);        // NaN
    CHECK(out[7] == 1);
    CHECK(out[8] == -1 || out[8] == 0);  // -0.5 LSB rounds away from zero (lround) -> -1
}

void testConcurrentReadWhileWriting() {
    // A writer thread writes the ramp in odd chunks as fast as it can; a reader repeatedly copies the newest
    // 20000 samples and checks that EVERY sample returned is the right value for its number (no torn or
    // overwritten data returned as good), and that the returned run is contiguous and ends near the head.
    ReplayRing r;
    r.resize(kRate, 1.0);
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> written{0};
    std::thread w([&] {
        std::uint64_t pos = 0;
        std::vector<float> v(3001);
        int k = 0;
        while (!stop.load()) {
            const std::size_t n = 1 + static_cast<std::size_t>((k++ * 977) % 3000);
            for (std::size_t i = 0; i < n; ++i) { v[i] = rampValue(pos + i); }
            r.write(v.data(), n, (k & 1) != 0);
            pos += n;
            written.store(pos);
            // Pace the writer at about 3000 samples per 40 us so that a copy of the WHOLE ring (which takes longer
            // than that) is overtaken by the writer partway through: that is the case the overwrite check exists for.
            const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(40);
            while (std::chrono::steady_clock::now() < until) {}
        }
    });
    long bad = 0, reads = 0, samplesChecked = 0, goodReads = 0;
    std::vector<std::int16_t> out(r.capacity());
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(1500)) {
        std::uint64_t first = 0;
        const std::size_t ask = (reads & 1) ? 30000 : out.size();  // every other read asks for the whole ring
        const std::size_t got = r.copyLast(ask, out.data(), &first);
        ++reads;
        if (got > 0) { ++goodReads; }
        for (std::size_t i = 0; i < got; ++i) {
            if (out[i] != rampInt(first + i)) { ++bad; break; }
        }
        samplesChecked += static_cast<long>(got);
    }
    stop = true;
    w.join();
    std::printf("  concurrent: %ld reads, %ld samples checked, %ld bad reads, %llu samples written\n", reads, samplesChecked, bad, static_cast<unsigned long long>(written.load()));
    CHECK(reads > 100);
    CHECK(goodReads > 10);  // the check does not simply refuse everything
    CHECK(bad == 0);
}

void testNoAllocationInWrite() {
    ReplayRing r;
    r.resize(kRate, 2.0);
    std::vector<float> v(4800, 0.25f);
    AllocProbe probe;
    for (int i = 0; i < 500; ++i) { r.write(v.data(), v.size(), (i & 1) != 0); }
    CHECK(probe.news() == 0);
    CHECK(r.head() == 500 * 4800);
}

}  // namespace

int main() {
    testBasics();
    testShorterExactAndWrap();
    testResize();
    testGateBitsAndSkipSilence();
    testQuantisation();
    testConcurrentReadWhileWriting();
    testNoAllocationInWrite();
    return testSummary("test_replay_ring");
}
