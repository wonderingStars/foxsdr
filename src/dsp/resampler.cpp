// Rational L/M polyphase resampler.
//
// SPDX-License-Identifier: MIT
#include "dsp/resampler.hpp"

#include <algorithm>
#include <cassert>
#include <numeric>

#include "dsp/fir.hpp"

namespace cascade::dsp {

RationalResampler::RationalResampler(unsigned interp, unsigned decim,
                                     unsigned tapsPerPhase) {
    assert(interp >= 1 && decim >= 1 && tapsPerPhase >= 1);
    // Reduce the ratio here rather than requiring the caller to: the filter
    // length, the branch count and the stream phase arithmetic below all key
    // off the REDUCED L and M, and an unreduced 12/50 would otherwise buy a
    // longer filter and more branches for exactly the same resampling job.
    const unsigned g = std::gcd(interp, decim);
    interp_ = interp / g;
    decim_ = decim / g;

    if (interp_ == 1 && decim_ == 1) {
        // 1/1: min(0.5/L, 0.5/M) would be the full Nyquist band, i.e. the
        // ideal filter is an allpass — so pass samples through untouched
        // (single unit tap, zero delay) instead of paying for a do-nothing
        // FIR that windowedSincLowpass could not design anyway (it requires
        // cutoff < 0.5).
        phaseLen_ = 1;
        taps_.assign(1, 1.0f);
    } else {
        // Total prototype length ~ L*tapsPerPhase, bumped to odd because the
        // windowed-sinc design needs its symmetric peak on an integer tap.
        std::size_t numTaps =
            interp_ * static_cast<std::size_t>(tapsPerPhase);
        if (numTaps % 2 == 0) { ++numTaps; }
        const double cutoff =
            0.5 / static_cast<double>(std::max(interp_, decim_));
        const std::vector<float> proto =
            windowedSincLowpass(numTaps, cutoff, WindowType::BlackmanHarris);

        // Scatter the prototype into L branches of K taps: branch p holds
        // h[p], h[p+L], h[p+2L], ... — exactly the taps that meet nonzero
        // (real) input samples when computing an output at upsampled phase p.
        phaseLen_ = (numTaps + interp_ - 1) / interp_;
        taps_.assign(interp_ * phaseLen_, 0.0f);
        for (std::size_t p = 0; p < interp_; ++p) {
            double sum = 0.0;
            for (std::size_t t = 0; p + t * interp_ < numTaps; ++t) {
                sum += static_cast<double>(proto[p + t * interp_]);
            }
            // Normalize each branch to sum exactly 1. The prototype's branch
            // sums are only ~1/L each — they deviate by the prototype's
            // stopband leakage sampled at multiples of the input rate — so
            // merely scaling the prototype by L would leave every output
            // phase with a slightly different DC gain, i.e. a periodic
            // ripple at the output rate riding on a constant input.
            // Per-branch normalization pins DC gain to exactly 1 for every
            // output phase, which is what "unity passband gain" must mean
            // for a resampler.
            assert(sum > 0.0 && "tapsPerPhase too small for this ratio");
            for (std::size_t t = 0; p + t * interp_ < numTaps; ++t) {
                taps_[p * phaseLen_ + t] = static_cast<float>(
                    static_cast<double>(proto[p + t * interp_]) / sum);
            }
        }
    }
    hist_.assign(phaseLen_ - 1, 0.0f);
}

std::size_t RationalResampler::process(const float* in, std::size_t nIn,
                                       float* out, std::size_t outCap) {
    assert(outCap >= maxOut(nIn) && "size out with maxOut(nIn)");
    const std::size_t histLen = hist_.size();

    // Concatenate history + block so every dot product below indexes one
    // contiguous buffer — no wrap-around cases at the block boundary, which
    // is exactly where continuity bugs live.
    work_.resize(histLen + nIn);
    std::copy(hist_.begin(), hist_.end(), work_.begin());
    if (nIn != 0) {
        std::copy(in, in + nIn,
                  work_.begin() + static_cast<std::ptrdiff_t>(histLen));
    }

    // Output k of the conceptual chain sits at upsampled index k*M, which
    // decomposes as newest-input-index n0 = floor(kM/L) and branch
    // p = kM mod L; y[k] = sum_t taps[p][t] * x[n0 - t]. Between consecutive
    // outputs the upsampled index advances by M, i.e. p += M with carry into
    // n0 — that carry is the `i += ...` below, and it is what makes one input
    // sample yield several outputs when L > M and skip whole inputs when
    // M > L.
    std::size_t produced = 0;
    std::size_t i = skip_;
    while (i < nIn) {
        if (produced < outCap) {  // documented drop-don't-overrun contract
            const std::size_t newest = histLen + i;
            const float* branch = taps_.data() + phase_ * phaseLen_;
            // Accumulate in double: tap-by-tap float rounding would put
            // block-size-independent (but audible-path) noise on the output.
            double acc = 0.0;
            for (std::size_t t = 0; t < phaseLen_; ++t) {
                acc += static_cast<double>(branch[t]) *
                       static_cast<double>(work_[newest - t]);
            }
            out[produced++] = static_cast<float>(acc);
        }
        const std::size_t step = phase_ + decim_;
        i += step / interp_;
        phase_ = step % interp_;
    }
    // The next output's newest sample lies `skip_` samples into the NEXT
    // block; carrying this (with phase_) is what keeps chunked processing
    // bit-identical to one-shot processing.
    skip_ = i - nIn;

    // Keep the newest histLen samples for the next call. When the block was
    // shorter than the history, the tail of work_ still ends with the right
    // mix of old history and new samples, so one copy covers both cases.
    if (histLen != 0) {
        std::copy(work_.end() - static_cast<std::ptrdiff_t>(histLen),
                  work_.end(), hist_.begin());
    }
    return produced;
}

std::size_t RationalResampler::maxOut(std::size_t nIn) const {
    // nIn inputs span nIn*L upsampled positions; outputs are the multiples of
    // M in that half-open window, of which any placement contains at most
    // ceil(nIn*L/M) — and a freshly-reset stream achieves the bound.
    if (nIn == 0) { return 0; }
    return (nIn * interp_ + decim_ - 1) / decim_;
}

void RationalResampler::reset() {
    std::fill(hist_.begin(), hist_.end(), 0.0f);
    phase_ = 0;
    skip_ = 0;
}

}  // namespace cascade::dsp
