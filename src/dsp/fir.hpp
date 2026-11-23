// FIR low-pass design (windowed sinc) and a streaming decimating FIR filter.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <complex>
#include <cstddef>
#include <vector>

#include "dsp/window.hpp"

namespace cascade::dsp {

// Designs a linear-phase low-pass FIR by the windowed-sinc method.
//
//   numTaps     odd (precondition) — an odd length puts the sinc peak on an
//               integer sample, giving an exactly symmetric impulse response
//               and therefore integer group delay of (numTaps-1)/2 samples.
//   cutoffNorm  cutoff frequency normalized to the sample rate, in (0, 0.5).
//               This is the -6 dB point of the resulting filter.
//   type        window applied in its *symmetric* (filter-design) form, not
//               the periodic form used for spectral analysis.
//
// The taps are normalized so their sum is exactly unity: DC passes with gain 1
// regardless of window choice, so a decimator built from these taps preserves
// signal amplitude in the passband.
std::vector<float> windowedSincLowpass(std::size_t numTaps, double cutoffNorm,
                                       WindowType type);

// Streaming FIR filter + integer decimator for complex<float> samples.
//
// Semantics match offline processing exactly: for input stream x and taps h,
// the output is y[m] = sum_j h[j] * x[m*M - j] (with x[n<0] = 0), i.e. causal
// convolution evaluated on the decimation grid 0, M, 2M, ... The filter keeps
// the last numTaps-1 input samples and the grid phase across process() calls,
// so any split of the input into blocks — including blocks shorter than M —
// produces bit-for-bit the same outputs as one big call.
//
// THE KERNEL (2026-11-23, the demod performance pass): on x86 the dot products of up to eight outputs are computed together with SSE2, in double,
// each output with its own accumulator and the SAME tap order as the scalar loop (tap 0 first, one multiply and one add per tap per component), so
// every output is BIT-IDENTICAL to the scalar loop's; the scalar loop stays for the tail and for other processors, and is the reference path
// (dsp/dsp_reference.hpp) the equivalence tests compare against. No instruction set beyond SSE2 is used, and no fused multiply-add can appear there.
class FirDecimator {
public:
    // taps: FIR coefficients (e.g. from windowedSincLowpass). Must be non-empty.
    // decimation: output one sample per `decimation` inputs. Must be >= 1
    // (1 degenerates to a plain streaming FIR filter).
    FirDecimator(std::vector<float> taps, std::size_t decimation);

    // Filters and decimates `inCount` samples from `in`, writing outputs to
    // `out`. Returns the number of outputs written, which is at most
    // outputCapacity(inCount); `out` must have room for that many. A call may
    // legally produce zero outputs (block shorter than the distance to the
    // next grid point).
    std::size_t process(const std::complex<float>* in, std::size_t inCount,
                        std::complex<float>* out);

    // Upper bound on outputs from a process(in, inCount, out) call, valid for
    // any internal phase — size `out` with this.
    std::size_t outputCapacity(std::size_t inCount) const noexcept {
        return inCount / decim_ + 1;
    }

    // Forgets all history: the next process() behaves like a freshly
    // constructed filter (stream restarts at grid position 0).
    void reset();

    std::size_t decimation() const noexcept { return decim_; }
    const std::vector<float>& taps() const noexcept { return taps_; }

private:
    std::vector<float> taps_;
    // The taps as doubles, each stored twice (h, h), so the several-outputs-at-once kernel loads a tap as one register that multiplies the real and
    // the imaginary part together (the conversion float -> double is exact, so these are the very values the scalar loop converts one at a time).
    std::vector<double> tapsDup_;
    std::size_t decim_;
    // Position of the next input sample on the decimation grid, in [0, decim_).
    // An output is produced exactly when a sample lands on phase 0.
    std::size_t phase_ = 0;
    // The last numTaps-1 input samples (zeros at stream start, so x[n<0] = 0
    // falls out for free), oldest first.
    std::vector<std::complex<float>> hist_;
    // Scratch for hist_ + current block; member so steady-state process()
    // calls don't allocate.
    std::vector<std::complex<float>> work_;
};

}  // namespace cascade::dsp
