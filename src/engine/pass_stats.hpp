// pass_stats.hpp - A LOG-LINEAR HISTOGRAM OF MICROSECONDS, for the figures the pump move's measurement run reports about the engine's pass
// (the survey's section 4): how long a pass takes, and how long from a submit's wake to the publish of the pass that served it.
//
// Four buckets an octave (about 19 % wide), 120 of them, so 0 us up to ~17 minutes in a fixed array of atomics: one writer (the control
// thread) and any reader, no allocation, no lock, nothing on a path that waits. A percentile is the UPPER EDGE of the bucket that holds
// it - an honest "at most", never a made-up precision.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <atomic>
#include <cstdint>

namespace cascade::engine {

struct PassHist {
    static constexpr int kBuckets = 120;

    // 0-3 us get a bucket each; from 4 on a value has octave e = floor(log2 us) and two mantissa bits m: bucket 4 * (e - 1) + m.
    static int bucketOf(std::uint64_t us) {
        if (us < 4) { return static_cast<int>(us); }
        int e = 0;
        for (std::uint64_t v = us; v > 1; v >>= 1) { ++e; }
        const int m = static_cast<int>((us >> (e - 2)) & 3u);
        const int b = 4 * (e - 1) + m;
        return b < kBuckets ? b : kBuckets - 1;
    }
    // The largest value the bucket holds.
    static std::uint64_t upperEdge(int b) {
        if (b < 4) { return static_cast<std::uint64_t>(b); }
        const int e = b / 4 + 1;
        const std::uint64_t m = static_cast<std::uint64_t>(b % 4);
        const std::uint64_t width = std::uint64_t{1} << (e - 2);
        return ((4 + m) << (e - 2)) + width - 1;
    }
    // The upper edge of the bucket holding the p-th percentile (0 < p <= 100) of `counts`; 0 for an empty histogram.
    static std::uint64_t percentile(const std::uint64_t (&counts)[kBuckets], double p) {
        std::uint64_t total = 0;
        for (int i = 0; i < kBuckets; ++i) { total += counts[i]; }
        if (total == 0) { return 0; }
        // nearest rank: the smallest value with at least p % of the sample at or below it
        std::uint64_t rank = static_cast<std::uint64_t>(p / 100.0 * static_cast<double>(total));
        if (static_cast<double>(rank) < p / 100.0 * static_cast<double>(total)) { ++rank; }
        if (rank < 1) { rank = 1; }
        std::uint64_t seen = 0;
        for (int i = 0; i < kBuckets; ++i) {
            seen += counts[i];
            if (seen >= rank) { return upperEdge(i); }
        }
        return upperEdge(kBuckets - 1);
    }

    void add(std::uint64_t us) { n[bucketOf(us)].fetch_add(1, std::memory_order_relaxed); }
    void copyTo(std::uint64_t (&out)[kBuckets]) const {
        for (int i = 0; i < kBuckets; ++i) { out[i] = n[i].load(std::memory_order_relaxed); }
    }

    std::atomic<std::uint64_t> n[kBuckets] = {};
};

}  // namespace cascade::engine
