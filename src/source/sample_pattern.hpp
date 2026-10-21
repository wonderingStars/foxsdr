// A STREAM THAT REPEATS ITSELF EXACTLY IS NOT A RADIO.
//
// On 2026-10-06 the RTL-SDR on the development bench stopped receiving and
// started delivering the same 1880 complex samples over and over, whatever it
// was tuned to: thirteen raw captures at thirteen frequencies had one hash. It
// kept its sample rate to the sample, reported no timeout, overflow or error,
// read a plausible mean magnitude and a plausible peak, and passed every live
// check this product had, twice a day, for a day. Those checks counted
// samples and looked at their level; neither says whether the samples came
// from the air. Thermal noise alone makes two samples of a real stream differ
// somewhere in any run of a few hundred, so a block that matches itself bit
// for bit at some lag is a digital pattern - a test mode, a stuck converter,
// a stalled tuner feeding the ADC its own reference - and never RF.
//
// exactRepeatPeriod() asks that one question of a block and nothing more. It
// is exact on purpose: an approximate measure (autocorrelation above a
// threshold) would need a threshold, and a threshold is a number that a
// strong carrier on a working radio can cross. Equality cannot be crossed by
// a signal; it is crossed only by the absence of one.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <complex>
#include <cstddef>

namespace cascade::source {

// The smallest period P in [1, maxPeriod] at which the block repeats itself
// EXACTLY - x[i] == x[i + P] for every i with i + P < n - having seen at least
// two full repeats of it (n >= 2 * P). 0 when there is none, which is what
// every block from a working radio returns. A constant block is period 1.
//
// Cost: for a block that does not repeat, the inner loop leaves at the first
// mismatch, so the whole call is about maxPeriod comparisons; for one that
// does, one pass over the block at the period found. A dropped sample inside
// the block breaks the equality, so a pattern can hide behind a USB overflow:
// callers hand in a block from a stream whose health line reports none.
inline std::size_t exactRepeatPeriod(const std::complex<float>* x, std::size_t n,
                                     std::size_t maxPeriod) {
    if (x == nullptr || n < 2) { return 0; }
    for (std::size_t p = 1; p <= maxPeriod && 2 * p <= n; ++p) {
        bool same = true;
        for (std::size_t i = 0; i + p < n; ++i) {
            if (x[i] != x[i + p]) {
                same = false;
                break;
            }
        }
        if (same) { return p; }
    }
    return 0;
}

}  // namespace cascade::source
