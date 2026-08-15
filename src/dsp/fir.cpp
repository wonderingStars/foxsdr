// FIR low-pass design (windowed sinc) and a streaming decimating FIR filter.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "dsp/fir.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace cascade::dsp {

std::vector<float> windowedSincLowpass(std::size_t numTaps, double cutoffNorm,
                                       WindowType type) {
    assert(numTaps % 2 == 1 && "even lengths break the symmetric-peak design");
    assert(cutoffNorm > 0.0 && cutoffNorm < 0.5);
    if (numTaps == 0) { return {}; }

    // Symmetric window from the periodic helper: an N-point symmetric window
    // uses divisor N-1, so it equals the (N-1)-point periodic window (divisor
    // N-1) with the first sample repeated at the end — at i = N-1 the cosine
    // arguments reach a full 2*pi and wrap to the i = 0 values. Reusing
    // fillWindow this way keeps the window coefficient tables in one place.
    std::vector<float> w(numTaps, 1.0f);
    if (numTaps > 1) {
        fillWindow(type, numTaps - 1, w.data());
        w[numTaps - 1] = w[0];
    }

    constexpr double kPi = 3.14159265358979323846264338327950288;
    const std::ptrdiff_t mid = static_cast<std::ptrdiff_t>(numTaps / 2);

    // Ideal low-pass impulse response 2*fc*sinc(2*fc*k), windowed. Everything
    // stays in double until the final cast: the taps are later summed for
    // normalization and small errors there show up directly as DC gain error.
    std::vector<double> h(numTaps);
    double sum = 0.0;
    for (std::size_t i = 0; i < numTaps; ++i) {
        const double k =
            static_cast<double>(static_cast<std::ptrdiff_t>(i) - mid);
        const double ideal = (k == 0.0)
                                 ? 2.0 * cutoffNorm
                                 : std::sin(2.0 * kPi * cutoffNorm * k) / (kPi * k);
        h[i] = ideal * static_cast<double>(w[i]);
        sum += h[i];
    }

    // Normalize to exactly unity DC gain: sum(h) is H(0), so dividing by it
    // makes the passband gain 1 whatever the window's coherent gain was.
    std::vector<float> taps(numTaps);
    for (std::size_t i = 0; i < numTaps; ++i) {
        taps[i] = static_cast<float>(h[i] / sum);
    }
    return taps;
}

FirDecimator::FirDecimator(std::vector<float> taps, std::size_t decimation)
    : taps_(std::move(taps)),
      decim_(decimation),
      // Zero-filled history means the first outputs see x[n<0] = 0, matching
      // causal offline convolution with no special-casing of stream start.
      hist_(taps_.empty() ? 0 : taps_.size() - 1) {
    assert(!taps_.empty());
    assert(decim_ >= 1);
}

std::size_t FirDecimator::process(const std::complex<float>* in,
                                  std::size_t inCount, std::complex<float>* out) {
    const std::size_t histLen = hist_.size();
    const std::size_t numTaps = taps_.size();

    // Concatenate history + block so every dot product below indexes one
    // contiguous buffer — no wrap-around cases at the block boundary, which is
    // exactly where continuity bugs live.
    work_.resize(histLen + inCount);
    std::copy(hist_.begin(), hist_.end(), work_.begin());
    if (inCount != 0) {
        std::copy(in, in + inCount, work_.begin() + static_cast<std::ptrdiff_t>(histLen));
    }

    std::size_t produced = 0;
    for (std::size_t i = 0; i < inCount; ++i) {
        if (phase_ == 0) {
            // y = sum_j h[j] * x[n-j]; newest sample is work_[histLen + i] and
            // the oldest one touched is work_[histLen + i - (numTaps-1)] >= 0.
            // Accumulate in double: tap-by-tap float rounding would otherwise
            // put block-size-dependent noise on the output.
            const std::size_t newest = histLen + i;
            double re = 0.0;
            double im = 0.0;
            for (std::size_t j = 0; j < numTaps; ++j) {
                const std::complex<float>& s = work_[newest - j];
                const double hj = static_cast<double>(taps_[j]);
                re += hj * static_cast<double>(s.real());
                im += hj * static_cast<double>(s.imag());
            }
            out[produced++] = {static_cast<float>(re), static_cast<float>(im)};
        }
        ++phase_;
        if (phase_ == decim_) { phase_ = 0; }
    }

    // Keep the newest histLen samples for the next call. When the block was
    // shorter than the history, the tail of work_ still ends with the right
    // mix of old history and new samples, so one copy covers both cases.
    if (histLen != 0) {
        std::copy(work_.end() - static_cast<std::ptrdiff_t>(histLen), work_.end(),
                  hist_.begin());
    }
    return produced;
}

void FirDecimator::reset() {
    std::fill(hist_.begin(), hist_.end(), std::complex<float>(0.0f, 0.0f));
    phase_ = 0;
}

}  // namespace cascade::dsp
