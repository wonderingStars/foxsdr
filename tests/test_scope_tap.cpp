/*
 * THE DISPLAY TAP GIVES WAY; THE DSP THREAD NEVER DOES.
 *
 * core/scope_tap.hpp exists because the two taps this product already had were
 * the wrong shape for a scope - one is a queue whose writer refuses to
 * overwrite, the other is 85 ms long and lives under the mutex the DSP thread
 * holds across a whole block. What replaces them has exactly one hazard worth
 * a test file: a reader that is copying a span the writer then laps must be
 * told, rather than handed half of one sweep and half of another.
 *
 * SO THE LAST CHECK IN THIS FILE IS THE ONE THAT EARNS IT. A writer thread
 * pushes a strictly increasing sequence while a reader snapshots as fast as it
 * can; every snapshot that comes back must be contiguous and increasing, which
 * a torn copy cannot be. The rest of the file pins the ordering, the
 * overwrite policy and the capacity contract that make that possible.
 *
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
#include <atomic>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <thread>
#include <vector>

#include "core/scope_tap.hpp"
#include "test_check.hpp"

namespace {

using cascade::core::ScopeTap;

// --- the capacity contract ---------------------------------------------------
void testCapacity() {
    bool threw = false;
    try {
        ScopeTap<float> bad(1000);  // not a power of two
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);

    threw = false;
    try {
        ScopeTap<float> zero(0);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);

    ScopeTap<float> ok(1024);
    CHECK(ok.capacity() == 1024);
    CHECK(ok.written() == 0);
}

// --- the newest samples, oldest first ----------------------------------------
//
// The ORDER is a contract the trace drawing leans on completely: dst[count-1]
// is the most recent instant, so a sweep runs left to right in time. A tap
// that handed its window back reversed would draw every waveform backwards and
// nothing else in the application would notice.
void testOrdering() {
    ScopeTap<float> tap(64);
    std::vector<float> in(10);
    for (std::size_t i = 0; i < in.size(); ++i) { in[i] = static_cast<float>(i); }
    tap.push(in.data(), in.size());
    CHECK(tap.written() == 10);

    std::vector<float> out(10, -1.0f);
    CHECK(tap.snapshot(out.data(), out.size()) == 10);
    for (std::size_t i = 0; i < 10; ++i) { CHECK(out[i] == static_cast<float>(i)); }

    // Fewer than are there: the NEWEST four, still oldest first.
    std::vector<float> four(4, -1.0f);
    CHECK(tap.snapshot(four.data(), four.size()) == 4);
    CHECK(four[0] == 6.0f);
    CHECK(four[3] == 9.0f);

    // More than are there: what exists, and no reading past it.
    std::vector<float> many(32, -1.0f);
    CHECK(tap.snapshot(many.data(), many.size()) == 10);
    CHECK(many[9] == 9.0f);
    CHECK(many[10] == -1.0f);  // untouched
}

// --- the writer overwrites, and never fails ----------------------------------
//
// This is the whole difference from dsp/spsc_ring.hpp. A scope nobody is
// looking at must not be able to make the DSP thread's writes fail; the oldest
// samples are simply gone, which for a display is the intended behaviour and
// not data loss.
void testOverwrite() {
    ScopeTap<float> tap(16);
    std::vector<float> in(100);
    for (std::size_t i = 0; i < in.size(); ++i) { in[i] = static_cast<float>(i); }
    // In pieces, so the wrap is crossed several times rather than once.
    for (std::size_t i = 0; i < in.size(); i += 7) {
        const std::size_t n = (i + 7 <= in.size()) ? 7u : (in.size() - i);
        tap.push(in.data() + i, n);
    }
    CHECK(tap.written() == 100);

    std::vector<float> out(16, -1.0f);
    CHECK(tap.snapshot(out.data(), out.size()) == 16);
    for (std::size_t i = 0; i < 16; ++i) {
        CHECK(out[i] == static_cast<float>(84 + i));
    }

    // A single push LONGER than the buffer keeps its own tail, not its head.
    ScopeTap<float> big(16);
    big.push(in.data(), in.size());
    CHECK(big.written() == 16);
    CHECK(big.snapshot(out.data(), out.size()) == 16);
    CHECK(out[0] == 84.0f);
    CHECK(out[15] == 99.0f);
}

// --- a live tap is tellable from a stalled one -------------------------------
//
// written() is monotonic and is how the page tells "the samples are all zero"
// from "nothing has arrived" - two statements this product has a standing rule
// about never conflating.
void testWrittenCounter() {
    ScopeTap<float> tap(32);
    CHECK(tap.written() == 0);
    const float silence[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    tap.push(silence, 8);
    CHECK(tap.written() == 8);
    tap.push(silence, 8);
    CHECK(tap.written() == 16);
    // A no-op push moves nothing.
    tap.push(nullptr, 8);
    tap.push(silence, 0);
    CHECK(tap.written() == 16);
}

// --- THE RACE ----------------------------------------------------------------
//
// One writer, one reader, no lock. Every snapshot that returns a count must be
// a contiguous increasing run of the sequence the writer is pushing: a copy
// that was lapped mid-way would show a jump backwards at the seam, which is
// precisely the tear the validation in snapshot() exists to refuse.
// A 32-bit COUNTER rather than the float the application instantiates, and
// deliberately: at float precision the sequence stops being exactly
// contiguous above 2^24 and the check would start failing on arithmetic rather
// than on tearing. The template does not care which type it carries, and the
// hazard being tested is in the counters, not in the elements.
void testConcurrentReads() {
    constexpr std::size_t kCap = 4096;
    ScopeTap<std::uint32_t> tap(kCap);
    std::atomic<bool> stop{false};
    std::atomic<int> torn{0};
    std::atomic<int> reads{0};

    // A WHOLE BUFFER PER PUSH, AND A QUARTER OF ONE PER READ, and those two
    // sizes are the entire point of the fixture. With a 512-element push and a
    // 1024-element read the writer had to advance more than three thousand
    // elements inside a one-microsecond memcpy to reach back into the span
    // being copied - which never once happened in twenty thousand attempts, so
    // the test passed just as happily with the tear check DELETED as with it
    // present. It was measuring nothing. A push the size of the buffer means a
    // SINGLE concurrent push overwrites everything the reader is holding, which
    // is the hazard snapshot() exists to refuse, and it now happens often
    // enough to be caught.
    std::thread writer([&] {
        std::vector<std::uint32_t> block(kCap);
        std::uint32_t next = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            for (std::size_t i = 0; i < block.size(); ++i) { block[i] = next++; }
            tap.push(block.data(), block.size());
        }
    });

    // WAIT FOR THE WRITER TO BE REALLY RUNNING FIRST. Spawning a thread costs
    // more than twenty thousand early-returning snapshots do, so without this
    // the reader finishes its whole loop against an empty tap and the test
    // passes while having measured nothing at all - which is how it first came
    // out (0 reads, 0 tears) and is exactly the vacuous green the checks below
    // are written to refuse.
    while (tap.written() == 0) { std::this_thread::yield(); }

    std::vector<std::uint32_t> out(kCap / 4);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    int i = 0;
    while (reads.load(std::memory_order_relaxed) < 2000 &&
           std::chrono::steady_clock::now() < deadline) {
        ++i;
        const std::size_t n = tap.snapshot(out.data(), out.size());
        if (n == 0) { continue; }
        reads.fetch_add(1, std::memory_order_relaxed);
        for (std::size_t k = 1; k < n; ++k) {
            if (out[k] != out[k - 1] + 1u) {
                torn.fetch_add(1, std::memory_order_relaxed);
                break;
            }
        }
    }
    stop.store(true, std::memory_order_relaxed);
    writer.join();

    // The reader got real windows (this is not a vacuous pass), and not one of
    // them was torn.
    CHECK(i > 0);
    CHECK(reads.load() >= 2000);
    CHECK(torn.load() == 0);
    // And the writer was never held up: it is still pushing hard enough that
    // the counter has run far past the buffer.
    CHECK(tap.written() > kCap);
}

}  // namespace

int main() {
    testCapacity();
    testOrdering();
    testOverwrite();
    testWrittenCounter();
    testConcurrentReads();
    return testSummary("test_scope_tap");
}
