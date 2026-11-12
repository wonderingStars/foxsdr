// Shared helpers for the test_bits_* tests (bit-view library, src/core/bits): a small
// deterministic generator and a frame generator. Not a test itself (no test_ prefix).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cmath>
#include <cstdint>
#include <vector>

#include "core/bits/bits.hpp"
#include "core/bits/linecode.hpp"

namespace bitstest {

struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed ? seed : 0x9E3779B97F4A7C15ull) {}
    uint64_t next() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return s;
    }
    double uni() { return static_cast<double>(next() >> 11) / 9007199254740992.0; }  // [0,1)
    double sym() { return 2.0 * uni() - 1.0; }                                        // [-1,1)
    uint8_t byte() { return static_cast<uint8_t>(next() >> 24); }
    uint32_t below(uint32_t n) { return static_cast<uint32_t>((next() >> 20) % n); }
};

inline cascade::bits::Bits randomBits(Rng& r, size_t n) {
    cascade::bits::Bits b(n);
    for (auto& x : b) x = static_cast<uint8_t>(r.next() >> 33 & 1u);
    return b;
}

inline std::vector<uint8_t> randomBytes(Rng& r, size_t n) {
    std::vector<uint8_t> v(n);
    for (auto& x : v) x = r.byte();
    return v;
}

// Chips -> run list, as a slicer would produce it: every level run lasts n chips of period
// T * (1 + clockError); each EDGE moves by a uniform amount in +-edgeJitter * T (so a run's length
// moves by the difference of two edges).
inline std::vector<cascade::bits::LevelRun> chipsToRuns(const cascade::bits::Bits& chips, double T, double clockError,
                                                        double edgeJitter, Rng& r) {
    std::vector<cascade::bits::LevelRun> runs;
    if (chips.empty()) return runs;
    const double Tc = T * (1.0 + clockError);
    double edgePrev = 0.0;  // jittered position of the previous edge (the start counts as exact)
    size_t i = 0;
    while (i < chips.size()) {
        size_t j = i;
        while (j < chips.size() && chips[j] == chips[i]) ++j;
        double edge = static_cast<double>(j) * Tc;
        if (j < chips.size()) edge += r.sym() * edgeJitter * T;
        runs.push_back({chips[i], edge - edgePrev});
        edgePrev = edge;
        i = j;
    }
    return runs;
}

}  // namespace bitstest
