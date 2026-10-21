// THE FIXED-PATTERN DETECTOR behind test_rtlsdr_live and --rtlsdr-check
// (source/sample_pattern.hpp), proved on blocks built here so that the live
// test's one new assertion is known to fire on the thing it is named after and
// to stay quiet on everything else, with no dongle in the room.
//
// The blocks are shaped like the bench's own failure: the dongle repeated 1880
// complex samples (3760 bytes at 2.4 MS/s) for a day while every count and
// level check passed. The noise here is a linear congruential generator, which
// is nothing like RF and exactly like RF in the one way that matters - it does
// not come round again inside the block.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "source/sample_pattern.hpp"
#include "test_check.hpp"

using cascade::source::exactRepeatPeriod;

namespace {

constexpr std::size_t kMaxPeriod = 4096;  // what the live test and the check search up to
constexpr std::size_t kDonglePeriod = 1880;  // the bench's pattern, 2026-10-06

// Samples that never repeat within any block this file builds: an LCG's 32-bit
// state has period 2^32, and every sample takes 16 bits of it.
std::vector<std::complex<float>> noise(std::size_t n, std::uint32_t seed) {
    std::vector<std::complex<float>> out(n);
    std::uint32_t s = seed;
    for (std::complex<float>& v : out) {
        s = s * 1664525u + 1013904223u;
        const float re = static_cast<float>((s >> 24) & 0xff) / 128.0f - 1.0f;
        const float im = static_cast<float>((s >> 16) & 0xff) / 128.0f - 1.0f;
        v = {re, im};
    }
    return out;
}

// One cycle of noise, tiled: the dongle's failure, in miniature.
std::vector<std::complex<float>> repeating(std::size_t period, std::size_t n, std::uint32_t seed) {
    const std::vector<std::complex<float>> cycle = noise(period, seed);
    std::vector<std::complex<float>> out(n);
    for (std::size_t i = 0; i < n; ++i) { out[i] = cycle[i % period]; }
    return out;
}

}  // namespace

int main() {
    // THE BENCH'S PATTERN is found, and named by its true period, from a block
    // the size the live test captures.
    {
        const auto block = repeating(kDonglePeriod, 32768, 7u);
        const std::size_t p = exactRepeatPeriod(block.data(), block.size(), kMaxPeriod);
        std::printf("1880-sample pattern over 32768: period %zu\n", p);
        CHECK(p == kDonglePeriod);
    }

    // NOISE IS NOT A PATTERN, at any lag up to the search limit.
    {
        const auto block = noise(32768, 7u);
        CHECK(exactRepeatPeriod(block.data(), block.size(), kMaxPeriod) == 0);
        // ...and a weak signal on top of noise is still not one: the smallest
        // block the live test accepts, with a different seed.
        const auto small = noise(2 * kMaxPeriod, 99u);
        CHECK(exactRepeatPeriod(small.data(), small.size(), kMaxPeriod) == 0);
    }

    // A CONSTANT is period 1 (a dead ADC reads a constant; the level checks
    // catch that first, and this agrees with them rather than contradicting).
    {
        const std::vector<std::complex<float>> flat(8192, std::complex<float>{0.25f, -0.5f});
        CHECK(exactRepeatPeriod(flat.data(), flat.size(), kMaxPeriod) == 1);
    }

    // THE SMALLEST PERIOD IS THE ANSWER: a block of period 4 also repeats every
    // 8, 12, ... and the caller is told 4.
    {
        const auto block = repeating(4, 1024, 3u);
        CHECK(exactRepeatPeriod(block.data(), block.size(), kMaxPeriod) == 4);
    }

    // THE SEARCH LIMIT IS INCLUSIVE: a period of exactly maxPeriod is found, one
    // sample longer is not looked for.
    {
        const auto at = repeating(kMaxPeriod, 4 * kMaxPeriod, 11u);
        CHECK(exactRepeatPeriod(at.data(), at.size(), kMaxPeriod) == kMaxPeriod);
        const auto past = repeating(kMaxPeriod + 1, 4 * kMaxPeriod, 11u);
        CHECK(exactRepeatPeriod(past.data(), past.size(), kMaxPeriod) == 0);
    }

    // TWO FULL REPEATS ARE REQUIRED: n == 2P is enough, n == 2P - 1 is not, so a
    // short block can never be called a pattern on one and a bit repeats.
    {
        const auto two = repeating(100, 200, 5u);
        CHECK(exactRepeatPeriod(two.data(), two.size(), kMaxPeriod) == 100);
        const auto less = repeating(100, 199, 5u);
        CHECK(exactRepeatPeriod(less.data(), less.size(), kMaxPeriod) == 0);
    }

    // THE CHECK IS EXACT: one sample dropped from a repeating stream breaks the
    // equality, which is why callers use a block from a stream with no
    // overflows. Stated here so the limitation is a tested fact, not a surprise.
    {
        auto block = repeating(kDonglePeriod, 32768, 7u);
        block.erase(block.begin() + 9000);
        CHECK(exactRepeatPeriod(block.data(), block.size(), kMaxPeriod) == 0);
    }

    // DEGENERATE INPUT answers 0 rather than reading past the end.
    {
        const auto one = noise(1, 1u);
        CHECK(exactRepeatPeriod(one.data(), one.size(), kMaxPeriod) == 0);
        CHECK(exactRepeatPeriod(nullptr, 0, kMaxPeriod) == 0);
    }

    return testSummary("test_sample_pattern");
}
