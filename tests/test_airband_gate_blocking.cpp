// The airband per-channel WAV output must never make the thread that destroys it wait for the disk (engine fixes 6, item 1).
//
// WHAT WENT WRONG. AsyncWavOutput (core/airband_gate.hpp) joined its worker and closed its file in its destructor. The gate that owns it lives in a
// patch plan, and a retired plan is destroyed on the control thread (core/patch_runner.hpp), so changing an airband channel's settings, or closing
// down, waited for the disk on the thread that applies the person's commands: on a stalled share, a pulled drive or a synchronised folder, for as
// long as the disk took. The inventory row for the thread said there was no such wait.
//
// WHAT IS HELD HERE. A stand-in writer (the output is a template on its writer) whose write() and close() block until the test lets them go:
//   A  DESTROYING THE OUTPUT WHILE write() IS BLOCKED returns within a bound (the measurement is printed), the blocked worker owns what it touches,
//      and when the disk answers every block that was queued is written, in order, and the file is closed once
//   B  the same while close() is blocked
//   C  QUIT: the finish is counted with the recorders' (RecordFinisher::inFlight), drain(deadline) is bounded while the disk is stuck and collects it
//      when it answers
//   D  A HEALTHY DISK: a real take (the product's ChannelWavWriter) is a complete, valid 16-bit mono WAV holding every sample that was accepted,
//      in order, even though the output was destroyed straight after the last push
//   E  THE AUDIO THREAD ALLOCATES NOTHING in push (operator new counted on the calling thread), accepted blocks and dropped ones alike
//   F  DROP AND COUNT: with the writer stuck, the pushes beyond the queue are dropped and counted, nothing is lost silently
//      (written + dropped == pushed), a block larger than the hand-over can hold is dropped and counted, and nothing blocks
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <thread>
#include <vector>

#include "core/airband_gate.hpp"
#include "core/record_finish.hpp"
#include "dspmodels_alloc.hpp"
#include "test_check.hpp"

using namespace cascade::core::airband;
using cascade::core::RecordFinisher;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

constexpr int kBlock = 64;   // samples per stand-in block; the first sample of block k is the number k

struct Ctl {
    std::atomic<bool> blockWrite{false};
    std::atomic<bool> blockClose{false};
    std::atomic<bool> closed{false};
    std::atomic<int> inWrite{0};         // write() calls that have begun
    std::atomic<int> closeCalls{0};
    std::atomic<long> written{0};        // samples
    std::atomic<int> blocks{0};
    std::atomic<int> outOfOrder{0};
    std::atomic<long> last{-1};
    void reset() {
        blockWrite = false;
        blockClose = false;
        closed = false;
        inWrite = 0;
        closeCalls = 0;
        written = 0;
        blocks = 0;
        outOfOrder = 0;
        last = -1;
    }
} g;

void nap(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// A disk that answers when the test says so.
struct StuckWriter {
    explicit StuckWriter(const WriterConfig&) {}
    bool write(const float* a, std::size_t n, bool, const UtcStamp&) {
        g.inWrite.fetch_add(1);
        while (g.blockWrite.load()) { nap(1); }
        const long seq = static_cast<long>(a[0]);
        if (seq <= g.last.load()) { g.outOfOrder.fetch_add(1); }
        g.last.store(seq);
        g.written.fetch_add(static_cast<long>(n));
        g.blocks.fetch_add(1);
        return true;
    }
    void close() {
        g.closeCalls.fetch_add(1);
        while (g.blockClose.load()) { nap(1); }
        g.closed.store(true);
    }
};
using StuckOut = BasicAsyncWavOutput<StuckWriter>;

void pushNumbered(StuckOut& o, int k) {
    std::vector<float> b(kBlock, 0.0f);
    b[0] = static_cast<float>(k);
    o.push(b.data(), b.size(), true);
}

template <class F>
bool waitFor(F f, int ms) {
    const auto end = Clock::now() + std::chrono::milliseconds(ms);
    while (Clock::now() < end) {
        if (f()) { return true; }
        nap(2);
    }
    return f();
}

long msSince(Clock::time_point t) { return static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t).count()); }

constexpr long kBoundMs = 250;       // what the control thread may spend destroying an output, however stuck the disk
constexpr int kStuckForMs = 1500;    // how long the stand-in disk is stuck in the cases that let it go again by itself

void testDestroyWhileWriteBlocked() {
    g.reset();
    g.blockWrite = true;
    auto out = std::make_unique<StuckOut>(WriterConfig{});
    pushNumbered(*out, 1);
    CHECK(waitFor([] { return g.inWrite.load() >= 1; }, 5000));   // the worker is inside the disk
    for (int k = 2; k <= 21; ++k) { pushNumbered(*out, k); }       // twenty more are queued behind it
    std::thread release([] { nap(kStuckForMs); g.blockWrite = false; });
    const auto t0 = Clock::now();
    out.reset();                                                    // THE CONTROL THREAD'S CALL
    const long ms = msSince(t0);
    std::printf("A: destroying the output with write() blocked returned after %ld ms (bound %ld, the disk answers after %d ms)\n", ms, kBoundMs, kStuckForMs);
    CHECK(ms < kBoundMs);
    release.join();
    // The disk answered: every queued block is written, in order, and the file is closed once.
    CHECK(waitFor([] { return g.closed.load(); }, 10000));
    CHECK(g.blocks.load() == 21);
    CHECK(g.written.load() == 21L * kBlock);
    CHECK(g.outOfOrder.load() == 0);
    CHECK(g.closeCalls.load() == 1);
    CHECK(waitFor([] { return RecordFinisher::inFlight() == 0; }, 5000));
}

void testDestroyWhileCloseBlocked() {
    g.reset();
    g.blockClose = true;
    auto out = std::make_unique<StuckOut>(WriterConfig{});
    for (int k = 1; k <= 5; ++k) { pushNumbered(*out, k); nap(2); }
    CHECK(waitFor([] { return g.blocks.load() == 5; }, 5000));
    std::thread release([] { nap(kStuckForMs); g.blockClose = false; });
    const auto t0 = Clock::now();
    out.reset();
    const long ms = msSince(t0);
    std::printf("B: destroying the output with close() blocked returned after %ld ms (bound %ld, the disk answers after %d ms)\n", ms, kBoundMs, kStuckForMs);
    CHECK(ms < kBoundMs);
    release.join();
    CHECK(waitFor([] { return g.closed.load(); }, 10000));
    CHECK(g.closeCalls.load() == 1 && g.written.load() == 5L * kBlock);
    CHECK(waitFor([] { return RecordFinisher::inFlight() == 0; }, 5000));
}

void testQuitDrainIsBoundedAndCollects() {
    g.reset();
    g.blockWrite = true;
    auto out = std::make_unique<StuckOut>(WriterConfig{});
    pushNumbered(*out, 1);
    CHECK(waitFor([] { return g.inWrite.load() >= 1; }, 5000));
    const std::size_t before = RecordFinisher::inFlight();
    out.reset();
    CHECK(RecordFinisher::inFlight() == before + 1);                       // counted with the recorders' finishes
    const auto t0 = Clock::now();
    const bool early = RecordFinisher::drain(Clock::now() + std::chrono::milliseconds(150));
    const long ms = msSince(t0);
    std::printf("C: quit drain against a stuck disk: returned %s after %ld ms\n", early ? "true" : "false", ms);
    CHECK(!early);                                                         // the disk is stuck, so the drain gives up at its deadline
    CHECK(ms >= 100 && ms < 1500);                                         // ...and not before it, and not long after
    g.blockWrite = false;
    CHECK(RecordFinisher::drain(Clock::now() + std::chrono::seconds(10))); // the disk answered: collected
    CHECK(g.closed.load() && RecordFinisher::inFlight() == before);
}

// --- D: a real take on a healthy disk -------------------------------------------------------------------------------------------------------
std::vector<unsigned char> readAll(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<unsigned char>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
std::uint32_t le32(const std::vector<unsigned char>& v, std::size_t at) {
    return static_cast<std::uint32_t>(v[at]) | (static_cast<std::uint32_t>(v[at + 1]) << 8) | (static_cast<std::uint32_t>(v[at + 2]) << 16) |
           (static_cast<std::uint32_t>(v[at + 3]) << 24);
}
std::int16_t quant(float v) { return static_cast<std::int16_t>(std::lrintf(std::min(std::max(v, -1.0f), 1.0f) * 32767.0f)); }

void testHealthyTakeIsValidAndComplete(const fs::path& scratch) {
    const fs::path root = scratch / "healthy";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    WriterConfig c;
    c.recordingsRoot = root.string();
    c.channelIndex = 4;
    c.freqHz = 121500000.0;
    c.preRollMs = 0.0;
    c.tailMs = 0.0;
    auto out = std::make_unique<AsyncWavOutput>(c);
    std::vector<float> expect;
    constexpr int kBlocks = 200, kN = 480;
    std::uint64_t dropBefore = 0;
    for (int k = 0; k < kBlocks; ++k) {
        std::vector<float> b(kN);
        for (int i = 0; i < kN; ++i) { b[i] = static_cast<float>(((k * kN + i) % 20000) * 4e-5 - 0.4); }
        out->push(b.data(), b.size(), true);
        if (out->droppedFrames() == dropBefore) { expect.insert(expect.end(), b.begin(), b.end()); }   // accepted (a busy lock drops: counted)
        dropBefore = out->droppedFrames();
        if (k % 20 == 19) { nap(1); }
    }
    out.reset();   // straight after the last push: the queue is still full of audio
    CHECK(waitFor([] { return RecordFinisher::inFlight() == 0; }, 10000));
    fs::path wav;
    for (const auto& e : fs::recursive_directory_iterator(root, ec)) {
        if (e.is_regular_file() && e.path().extension() == ".wav") { wav = e.path(); }
    }
    CHECK(!wav.empty());
    const std::vector<unsigned char> f = readAll(wav);
    CHECK(f.size() >= 44);
    if (f.size() < 44) { return; }
    const std::size_t dataBytes = f.size() - 44;
    std::printf("D: take of %zu accepted samples -> %s, %zu bytes\n", expect.size(), wav.filename().string().c_str(), f.size());
    CHECK(std::memcmp(&f[0], "RIFF", 4) == 0 && std::memcmp(&f[8], "WAVEfmt ", 8) == 0 && std::memcmp(&f[36], "data", 4) == 0);
    CHECK(le32(f, 4) == 36 + dataBytes && le32(f, 40) == dataBytes);           // the header says what the file holds
    CHECK(f[20] == 1 && f[22] == 1 && le32(f, 24) == 48000 && f[34] == 16);    // PCM, mono, 48 kHz, 16 bit
    CHECK(dataBytes == expect.size() * 2);                                     // every accepted sample is there
    std::size_t bad = 0;
    for (std::size_t i = 0; i < expect.size() && 44 + 2 * i + 1 < f.size(); ++i) {
        const std::int16_t s = static_cast<std::int16_t>(f[44 + 2 * i] | (f[45 + 2 * i] << 8));
        if (s != quant(expect[i])) { ++bad; }
    }
    CHECK(bad == 0);
}

// --- E: the audio thread allocates nothing --------------------------------------------------------------------------------------------------
void testPushAllocatesNothing(const fs::path& scratch) {
    const fs::path root = scratch / "alloc";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    WriterConfig c;
    c.recordingsRoot = root.string();
    c.channelIndex = 5;
    c.freqHz = 118700000.0;
    auto out = std::make_unique<AsyncWavOutput>(c);
    std::vector<float> a(960, 0.1f);
    long news = -1;
    {
        DspmAllocProbe probe;
        for (int i = 0; i < 1000; ++i) { out->push(a.data(), a.size(), (i / 10) % 2 == 0); }
        news = probe.news();
    }
    std::printf("E: 1000 pushes on a healthy disk: %ld allocations on the audio thread (dropped %llu)\n", news,
                static_cast<unsigned long long>(out->droppedFrames()));
    CHECK(news == 0);
    out.reset();
    CHECK(waitFor([] { return RecordFinisher::inFlight() == 0; }, 10000));
    // ...and with the writer stuck and the hand-over full (the drop path).
    g.reset();
    g.blockWrite = true;
    auto stuck = std::make_unique<StuckOut>(WriterConfig{});
    pushNumbered(*stuck, 1);
    CHECK(waitFor([] { return g.inWrite.load() >= 1; }, 5000));
    std::vector<float> b(kBlock, 0.5f);
    long news2 = -1;
    {
        DspmAllocProbe probe;
        for (int i = 0; i < 1000; ++i) { stuck->push(b.data(), b.size(), true); }
        news2 = probe.news();
    }
    std::printf("E: 1000 pushes with the writer stuck (the drop path): %ld allocations\n", news2);
    CHECK(news2 == 0);
    g.blockWrite = false;
    stuck.reset();
    CHECK(waitFor([] { return g.closed.load(); }, 10000));
    CHECK(waitFor([] { return RecordFinisher::inFlight() == 0; }, 5000));
}

// --- F: drop and count ----------------------------------------------------------------------------------------------------------------------
void testDropAndCount() {
    g.reset();
    g.blockWrite = true;
    auto out = std::make_unique<StuckOut>(WriterConfig{});
    pushNumbered(*out, 1);
    CHECK(waitFor([] { return g.inWrite.load() >= 1; }, 5000));
    constexpr int kPushes = 400;
    const auto t0 = Clock::now();
    for (int k = 2; k <= kPushes; ++k) { pushNumbered(*out, k); }
    const long ms = msSince(t0);
    CHECK(ms < 1000);                                                  // a stuck disk never holds the audio thread
    const std::uint64_t dropped = out->droppedFrames();
    std::printf("F: %d pushes into a stuck writer: %llu samples dropped and counted, %ld ms\n", kPushes, static_cast<unsigned long long>(dropped), ms);
    CHECK(dropped > 0);
    // A block no hand-over can hold is dropped and counted as one.
    std::vector<float> huge(1u << 20, 0.25f);
    out->push(huge.data(), huge.size(), true);
    CHECK(out->droppedFrames() == dropped + huge.size());
    g.blockWrite = false;
    out.reset();
    CHECK(waitFor([] { return g.closed.load(); }, 10000));
    // Nothing is lost silently: every sample pushed was either written or counted as dropped.
    const long pushedSamples = static_cast<long>(kPushes) * kBlock + static_cast<long>(huge.size());
    // (the final drop count is read before the worker's drain, which drops nothing)
    CHECK(g.written.load() + static_cast<long>(dropped + huge.size()) == pushedSamples);
    CHECK(g.outOfOrder.load() == 0);
    CHECK(waitFor([] { return RecordFinisher::inFlight() == 0; }, 5000));
}

}  // namespace

int main() {
    const fs::path scratch = fs::temp_directory_path() / "foxsdr_test_airband_gate_blocking";
    std::error_code ec;
    fs::remove_all(scratch, ec);
    fs::create_directories(scratch, ec);
    testDestroyWhileWriteBlocked();
    testDestroyWhileCloseBlocked();
    testQuitDrainIsBoundedAndCollects();
    testHealthyTakeIsValidAndComplete(scratch);
    testPushAllocatesNothing(scratch);
    testDropAndCount();
    fs::remove_all(scratch, ec);
    return testSummary("test_airband_gate_blocking");
}
