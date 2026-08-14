// Tests for dsp/spsc_ring.hpp.
//
// Correctness is proven against a running expected-value counter (the ring
// carries the sequence 0,1,2,... and the consumer checks every element), not
// against constants derived from the implementation.
//
// SPDX-License-Identifier: MIT
#include "dsp/spsc_ring.hpp"

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
};

// Two-thread stress: the producer pushes `total` sequential float values in a
// fixed cycle of odd chunk sizes; the consumer (calling thread) drains with a
// different, co-prime-length cycle so the two schedules keep sliding against
// each other, exercising wrap, partial accept, full and empty continuously.
// Deterministic schedules — no randomness needed, seeded or otherwise.
StressResult runStress(std::size_t ringCapacity, std::size_t total) {
    SpscRing<float> ring(ringCapacity);
    StressResult res;

    static const std::size_t kProdChunks[] = {1, 3, 7, 13, 29, 61, 127, 251, 5, 97, 9};
    static const std::size_t kConsChunks[] = {2, 5, 11, 64, 1, 300, 17, 4, 128};

    std::thread producer([&ring, total] {
        std::vector<float> chunk(256);  // >= max producer chunk size
        std::size_t written = 0;
        std::size_t sched = 0;
        while (written < total) {
            std::size_t want = kProdChunks[sched % std::size(kProdChunks)];
            ++sched;
            if (want > total - written) { want = total - written; }
            for (std::size_t i = 0; i < want; ++i) {
                chunk[i] = static_cast<float>(written + i);
            }
            std::size_t done = 0;
            while (done < want) {
                const std::size_t accepted = ring.write(chunk.data() + done, want - done);
                if (accepted == 0) { std::this_thread::yield(); }
                done += accepted;
            }
            written += want;
        }
    });

    std::vector<float> out(512);  // >= max consumer chunk size
    std::size_t sched = 0;
    while (res.totalRead < total) {
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
        }
        res.totalRead += got;
    }

    producer.join();
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
        while (readCount < total) {
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
        const StressResult res = runStress(1024, 1000000);
        CHECK(res.totalRead == 1000000u);
        CHECK(res.mismatches == 0);
        if (res.mismatches != 0) {
            std::printf("stress: %lld mismatches, first at index %lld (got %.1f)\n",
                        res.mismatches, res.firstBadIndex,
                        static_cast<double>(res.firstBadValue));
        }
    }

    return testSummary("test_spsc_ring");
}
