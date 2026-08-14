// Tests for dsp/spsc_ring.hpp.
//
// Correctness is proven against a running expected-value counter (the ring
// carries the sequence 0,1,2,... and the consumer checks every element), not
// against constants derived from the implementation.
//
// SPDX-License-Identifier: MIT
#include "dsp/spsc_ring.hpp"

#include <atomic>
#include <chrono>
#include <complex>
#include <cstddef>
#include <cstdio>
#include <iterator>
#include <stdexcept>
#include <thread>
#include <vector>

#include "test_check.hpp"

using cascade::dsp::SpscRing;

namespace {

bool ctorThrows(std::size_t capacity) {
    try {
        SpscRing<float> r(capacity);
    } catch (const std::invalid_argument&) {
        return true;
    }
    return false;
}

struct StressResult {
    long long mismatches = 0;
    long long firstBadIndex = -1;
    float firstBadValue = 0.0f;
    std::size_t totalRead = 0;
    std::size_t totalWritten = 0;  // elements the ring actually accepted
    double readSum = 0.0;          // sum of every value the consumer read
    double writtenSum = 0.0;       // sum of every value write() accepted
    bool completed = false;        // consumer finished by progress, not deadline
};

// Two-thread stress: the producer pushes `total` sequential float values in a
// fixed cycle of odd chunk sizes; the consumer (calling thread) drains with a
// different, co-prime-length cycle so the two schedules keep sliding against
// each other, exercising wrap, partial accept, full and empty continuously.
// Deterministic schedules — no randomness needed, seeded or otherwise.
//
// Liveness: every spin loop is bounded by a shared 30 s steady_clock deadline
// (a normal run finishes in well under a second). Whichever side hits the
// deadline first raises `abortFlag`, the other side polls it, so both threads
// always exit and join; runStress then reports completed == false instead of
// hanging the test.
StressResult runStress(std::size_t ringCapacity, std::size_t total) {
    SpscRing<float> ring(ringCapacity);
    StressResult res;
    std::atomic<bool> abortFlag{false};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);

    static const std::size_t kProdChunks[] = {1, 3, 7, 13, 29, 61, 127, 251, 5, 97, 9};
    static const std::size_t kConsChunks[] = {2, 5, 11, 64, 1, 300, 17, 4, 128};

    double prodSum = 0.0;         // written by producer thread, read after join()
    std::size_t prodWritten = 0;  // ditto

    std::thread producer([&ring, &abortFlag, &prodSum, &prodWritten, total, deadline] {
        std::vector<float> chunk(256);  // >= max producer chunk size
        std::size_t written = 0;
        std::size_t sched = 0;
        double sum = 0.0;
        bool timedOut = false;
        while (written < total && !timedOut && !abortFlag.load(std::memory_order_relaxed)) {
            std::size_t want = kProdChunks[sched % std::size(kProdChunks)];
            ++sched;
            if (want > total - written) { want = total - written; }
            for (std::size_t i = 0; i < want; ++i) {
                chunk[i] = static_cast<float>(written + i);
            }
            std::size_t done = 0;
            while (done < want) {
                const std::size_t accepted = ring.write(chunk.data() + done, want - done);
                if (accepted == 0) {
                    if (abortFlag.load(std::memory_order_relaxed) ||
                        std::chrono::steady_clock::now() >= deadline) {
                        timedOut = true;
                        break;
                    }
                    std::this_thread::yield();
                    continue;
                }
                // Values accepted this call are written+done .. written+done+accepted-1.
                for (std::size_t i = 0; i < accepted; ++i) {
                    sum += static_cast<double>(written + done + i);
                }
                done += accepted;
            }
            written += done;  // done == want unless the deadline expired mid-chunk
        }
        if (timedOut) { abortFlag.store(true, std::memory_order_relaxed); }
        prodSum = sum;
        prodWritten = written;
    });

    std::vector<float> out(512);  // >= max consumer chunk size
    std::size_t sched = 0;
    bool timedOut = false;
    while (res.totalRead < total) {
        if (abortFlag.load(std::memory_order_relaxed) ||
            std::chrono::steady_clock::now() >= deadline) {
            timedOut = true;
            break;
        }
        std::size_t want = kConsChunks[sched % std::size(kConsChunks)];
        ++sched;
        if (want > total - res.totalRead) { want = total - res.totalRead; }
        const std::size_t got = ring.read(out.data(), want);
        if (got == 0) {
            std::this_thread::yield();
            continue;
        }
        for (std::size_t i = 0; i < got; ++i) {
            const float expect = static_cast<float>(res.totalRead + i);
            if (out[i] != expect) {
                if (res.mismatches == 0) {
                    res.firstBadIndex = static_cast<long long>(res.totalRead + i);
                    res.firstBadValue = out[i];
                }
                ++res.mismatches;
            }
            res.readSum += static_cast<double>(out[i]);
        }
        res.totalRead += got;
    }
    res.completed = !timedOut && res.totalRead == total;
    if (timedOut) { abortFlag.store(true, std::memory_order_relaxed); }

    producer.join();
    res.writtenSum = prodSum;
    res.totalWritten = prodWritten;
    return res;
}

}  // namespace

int main() {
    // --- Power-of-two capacity is enforced at construction ------------------
    CHECK(ctorThrows(0));
    CHECK(ctorThrows(3));
    CHECK(ctorThrows(12));
    CHECK(ctorThrows(1000));
    CHECK(!ctorThrows(1));  // 2^0 is a legal (if tiny) ring
    CHECK(!ctorThrows(8));
    CHECK(!ctorThrows(1024));

    // --- Full ring refuses further writes; the full capacity is usable ------
    {
        SpscRing<float> r(8);
        CHECK(r.capacity() == 8u);
        CHECK(r.size() == 0u);
        CHECK(r.freeSpace() == 8u);

        const float in[8] = {10, 11, 12, 13, 14, 15, 16, 17};
        CHECK(r.write(in, 8) == 8u);  // no keep-one-slot-empty tax
        CHECK(r.size() == 8u);
        CHECK(r.freeSpace() == 0u);

        const float extra = 99;
        CHECK(r.write(&extra, 1) == 0u);  // full: refuses, never overwrites

        float out[8] = {};
        CHECK(r.read(out, 8) == 8u);
        for (int i = 0; i < 8; ++i) { CHECK(out[i] == in[i]); }  // 99 never landed
        CHECK(r.read(out, 1) == 0u);  // and empty yields nothing
    }

    // --- Partial writes and reads clamp to what fits / what exists ----------
    {
        SpscRing<float> r(8);
        const float in[6] = {0, 1, 2, 3, 4, 5};
        CHECK(r.write(in, 5) == 5u);
        CHECK(r.write(in, 6) == 3u);  // only 3 slots free: partial accept
        CHECK(r.size() == 8u);

        float out[16] = {};
        CHECK(r.read(out, 16) == 8u);  // asked for 16, ring holds 8
        const float expect[8] = {0, 1, 2, 3, 4, 0, 1, 2};
        for (int i = 0; i < 8; ++i) { CHECK(out[i] == expect[i]); }
    }

    // --- Wraparound preserves exact data -------------------------------------
    // Push 100 sequential values through a capacity-8 ring in chunks of 5 and
    // drain in chunks of 3: the head/tail counters cross the wrap boundary
    // dozens of times and the sequence must survive intact.
    {
        SpscRing<float> r(8);
        std::size_t written = 0;
        std::size_t readCount = 0;
        const std::size_t total = 100;
        float buf[8] = {};
        // Liveness bound: ~40 productive iterations finish the job, so 100000
        // is a generous retry cap. A ring that stops making progress (write or
        // read pinned at 0) breaks out and fails below instead of spinning
        // forever.
        bool madeProgress = true;
        std::size_t iters = 0;
        while (readCount < total) {
            if (++iters > 100000u) {
                madeProgress = false;
                break;
            }
            if (written < total) {
                float chunk[5];
                std::size_t want = 5;
                if (want > total - written) { want = total - written; }
                for (std::size_t i = 0; i < want; ++i) {
                    chunk[i] = static_cast<float>(written + i);
                }
                written += r.write(chunk, want);
            }
            const std::size_t got = r.read(buf, 3);
            for (std::size_t i = 0; i < got; ++i) {
                CHECK(buf[i] == static_cast<float>(readCount + i));
            }
            readCount += got;
        }
        CHECK(madeProgress);  // wraparound loop finished by progress, not by cap
        CHECK(readCount == total);
        CHECK(r.size() == written - readCount);
    }

    // --- Degenerate n == 0 calls are safe no-ops (even with null pointers) ---
    {
        SpscRing<float> r(4);
        CHECK(r.write(nullptr, 0) == 0u);
        CHECK(r.read(nullptr, 0) == 0u);
        const float v = 7;
        CHECK(r.write(&v, 1) == 1u);
        CHECK(r.write(nullptr, 0) == 0u);  // n==0 on a non-empty ring
        CHECK(r.size() == 1u);
        float out = 0;
        CHECK(r.read(&out, 1) == 1u);
        CHECK(out == 7.0f);
    }

    // --- std::complex<float> element type (the IQ-sample case) ---------------
    {
        SpscRing<std::complex<float>> r(4);
        const std::complex<float> a[3] = {{1, -1}, {2, -2}, {3, -3}};
        CHECK(r.write(a, 3) == 3u);
        std::complex<float> out[3] = {};
        CHECK(r.read(out, 2) == 2u);
        CHECK(out[0] == a[0]);
        CHECK(out[1] == a[1]);
        const std::complex<float> b[3] = {{4, -4}, {5, -5}, {6, -6}};
        CHECK(r.write(b, 3) == 3u);  // crosses the wrap boundary
        std::complex<float> out2[4] = {};
        CHECK(r.read(out2, 4) == 4u);
        CHECK(out2[0] == a[2]);
        CHECK(out2[1] == b[0]);
        CHECK(out2[2] == b[1]);
        CHECK(out2[3] == b[2]);
    }

    // --- Two-thread stress: 1,000,000 floats, zero loss or duplication -------
    {
        const std::size_t kTotal = 1000000;
        const StressResult res = runStress(1024, kTotal);
        // The consumer must finish by reading every element, not by hitting the
        // 30 s liveness deadline: a ring that stops accepting writes or stops
        // yielding reads now fails here instead of hanging the test.
        CHECK(res.completed);
        if (!res.completed) {
            std::printf("stress: liveness deadline expired — read %zu of %zu, ring accepted %zu\n",
                        res.totalRead, kTotal, res.totalWritten);
        }
        CHECK(res.totalRead == kTotal);
        CHECK(res.totalWritten == kTotal);
        CHECK(res.mismatches == 0);
        // Conservation checksum, independent of the loop exit conditions: what
        // the producer pushed and what the consumer read must both equal the
        // analytic sum 0+1+...+999999. Every value and every partial sum is an
        // integer below 2^53, so the double additions are exact in any order
        // and the comparison can be exact too.
        const double expectSum = 499999500000.0;  // kTotal*(kTotal-1)/2
        CHECK(res.writtenSum == expectSum);
        CHECK(res.readSum == expectSum);
        if (res.mismatches != 0) {
            std::printf("stress: %lld mismatches, first at index %lld (got %.1f)\n",
                        res.mismatches, res.firstBadIndex,
                        static_cast<double>(res.firstBadValue));
        }
    }

    return testSummary("test_spsc_ring");
}
