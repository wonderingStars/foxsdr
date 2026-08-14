// SPDX-License-Identifier: MIT
#include "source/siggen.hpp"

#include <algorithm>
#include <cmath>

namespace cascade::source {

namespace {
// Per-tone scratch block size. Small enough to stay in L1 (2 KiB), and its
// value is behaviorally invisible: the Nco emits sample k identically no
// matter how the run is chunked, so results are bit-identical for any kChunk.
constexpr std::size_t kChunk = 256;
}  // namespace

SigGen::SigGen(double sampleRateHz)
    // A nonpositive rate would turn every tone frequency into inf/NaN via the
    // normalization divide and poison every downstream FFT; fall back to 1 Hz
    // so a misconfigured caller gets silence-shaped output instead of NaNs.
    : sampleRateHz_(sampleRateHz > 0.0 ? sampleRateHz : 1.0) {}

void SigGen::setTone(int slot, double freqHz, float amplitudeDb) {
    if (slot < 0 || slot >= static_cast<int>(tones_.size())) {
        return;  // contract is 0..3; ignore junk rather than corrupt memory
    }
    Tone& t = tones_[static_cast<std::size_t>(slot)];
    // Nco::setFrequency folds any out-of-range normalized frequency back into
    // [-0.5, 0.5) itself (a sampled tone aliases there anyway), so no
    // additional range handling is needed here.
    t.nco.setFrequency(freqHz / sampleRateHz_);
    t.amp = std::pow(10.0f, amplitudeDb * (1.0f / 20.0f));
    t.active = true;
}

void SigGen::clearTone(int slot) {
    if (slot < 0 || slot >= static_cast<int>(tones_.size())) {
        return;
    }
    // The phase accumulator is deliberately left alone: an inactive slot's
    // Nco is simply not stepped, so a later re-enable resumes deterministically.
    tones_[static_cast<std::size_t>(slot)].active = false;
}

void SigGen::setNoiseFloorDb(float db) {
    if (db <= -300.0f) {
        // Exact zeros, not a ~1e-15 residue: the "noise off" state must be
        // provably silent so tests (and the spectrum display) see a true floor.
        noiseSigma_ = 0.0f;
        return;
    }
    // db is TOTAL complex power 10^(db/10); it splits evenly between the I
    // and Q components, hence the 0.5 under the square root.
    noiseSigma_ = std::sqrt(std::pow(10.0f, db * (1.0f / 10.0f)) * 0.5f);
}

float SigGen::gaussian() noexcept {
    // Irwin-Hall: the sum of 12 U(0,1) draws has variance 12 * (1/12) = 1 and
    // mean 6, so (sum - 6) is zero-mean, unit-variance, and close to Gaussian
    // through the +/-3 sigma body — plenty for a noise floor, with no
    // transcendental calls (Box-Muller's log/sqrt per sample would dominate
    // the generator's cost). Tails truncate at +/-6 sigma, which only matters
    // below the -280 dB regime this source never claims to reproduce.
    float sum = 0.0f;
    for (int i = 0; i < 12; ++i) {
        // Numerical Recipes 32-bit LCG; the top 24 bits become the uniform
        // (low LCG bits have short periods and must not be used).
        rng_ = rng_ * 1664525u + 1013904223u;
        sum += static_cast<float>(rng_ >> 8) * (1.0f / 16777216.0f);
    }
    return sum - 6.0f;
}

void SigGen::generate(std::complex<float>* dst, std::size_t n) {
    if (dst == nullptr || n == 0) {
        return;
    }
    // Noise (or zero) fill happens first, in one pass, and the tone loop never
    // touches rng_. That ordering is what makes the output invariant under
    // chunking: sample k always consumes the same LCG draws and the same Nco
    // phase step regardless of how calls are split.
    if (noiseSigma_ > 0.0f) {
        for (std::size_t i = 0; i < n; ++i) {
            dst[i] = std::complex<float>(noiseSigma_ * gaussian(),
                                         noiseSigma_ * gaussian());
        }
    } else {
        for (std::size_t i = 0; i < n; ++i) {
            dst[i] = std::complex<float>(0.0f, 0.0f);
        }
    }
    std::complex<float> tmp[kChunk];
    for (Tone& t : tones_) {
        if (!t.active) {
            continue;
        }
        std::size_t done = 0;
        while (done < n) {
            const std::size_t m = std::min(kChunk, n - done);
            t.nco.generate(tmp, m);
            for (std::size_t i = 0; i < m; ++i) {
                dst[done + i] += t.amp * tmp[i];
            }
            done += m;
        }
    }
}

}  // namespace cascade::source
