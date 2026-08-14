// Rational L/M sample-rate conversion (polyphase windowed-sinc) for real
// float streams — the post-demod audio path (e.g. 200 kHz -> 48 kHz is 6/25).
//
// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <vector>

namespace cascade::dsp {

// Streaming polyphase rational resampler: output rate = input rate * L/M.
//
// Conceptually the chain is upsample-by-L (zero insertion) -> low-pass at the
// upsampled rate -> keep every M-th sample. The low-pass is a windowed sinc
// (fir.hpp windowedSincLowpass) with cutoff min(0.5/L, 0.5/M) of the upsampled
// rate: 0.5/L removes the L-fold spectral images that zero insertion creates,
// 0.5/M removes content that would alias past the OUTPUT Nyquist — whichever
// is smaller binds. The polyphase form computes only the outputs that survive
// the decimation, and only from the nonzero (real input) samples: output k
// needs the single prototype phase p = (k*M) mod L dotted with the most recent
// input samples, so the cost per output is one short FIR, independent of how
// large L and M are.
//
// interp/decim are reduced by their gcd INTERNALLY (callers may pass rates
// verbatim, e.g. 48000/200000): the reduced ratio determines the filter and
// the stream state machine, so RationalResampler(12, 50) is bit-identical to
// RationalResampler(6, 25).
class RationalResampler {
public:
    // interp: L >= 1. decim: M >= 1. tapsPerPhase >= 1 is the number of
    // prototype taps that fire per output sample; the total prototype length
    // is about interp*tapsPerPhase (bumped to odd for the symmetric
    // windowed-sinc design). 16 is a sensible audio default; raise it when a
    // sharper transition band is worth the extra multiplies.
    RationalResampler(unsigned interp, unsigned decim,
                      unsigned tapsPerPhase = 16);

    // Resamples nIn samples from `in`, writing outputs to `out`; returns the
    // number of outputs produced. Filter history and the L/M stream phase
    // carry across calls, so any block split of a stream — including blocks
    // that produce zero outputs — yields bit-identical output to one big call.
    //
    // outCap contract: size `out` with outCap >= maxOut(nIn) (asserted in
    // debug builds); capacity beyond what is produced is left untouched. If a
    // release-build caller undersizes anyway, the call never writes past
    // outCap: all input is still consumed and the stream phase still advances
    // (subsequent timing stays correct) but outputs past outCap are dropped.
    std::size_t process(const float* in, std::size_t nIn, float* out,
                        std::size_t outCap);

    // Tight upper bound on the outputs of process(in, nIn, ...), valid for
    // any internal stream phase — size `out` with this. Equals ceil(nIn*L/M)
    // in reduced form, and is achieved from a freshly-reset state.
    std::size_t maxOut(std::size_t nIn) const;

    // Forgets all history: the next process() behaves like a freshly
    // constructed resampler.
    void reset();

private:
    std::size_t interp_ = 1;    // L, reduced
    std::size_t decim_ = 1;     // M, reduced
    std::size_t phaseLen_ = 1;  // K: taps per polyphase branch (zero-padded)
    // L branches of K taps, branch p at taps_[p*K .. p*K+K): branch p holds
    // prototype taps h[p], h[p+L], h[p+2L], ... zero-padded to K. Each branch
    // is normalized to sum exactly 1 (see resampler.cpp for why).
    std::vector<float> taps_;
    // The last K-1 input samples, oldest first (zeros at stream start, so
    // x[n<0] = 0 falls out for free).
    std::vector<float> hist_;
    // Scratch for hist_ + current block; member so steady-state process()
    // calls don't allocate, contiguous so dot products never wrap.
    std::vector<float> work_;
    // Polyphase branch index of the NEXT output, in [0, L).
    std::size_t phase_ = 0;
    // Input samples of the NEXT block to pass over before the next output is
    // due (the decimation grid can land past the current block's end).
    std::size_t skip_ = 0;
};

}  // namespace cascade::dsp
