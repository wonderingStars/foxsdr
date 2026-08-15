// VFO channelizer: NCO mix to baseband, anti-alias low-pass, integer decimate.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "dsp/vfo.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace cascade::dsp {

namespace {
// Internal processing chunk. Bounds the scratch buffers regardless of how
// large a block the caller hands in, so memory stays O(kChunk) while the
// per-call loop overhead stays negligible against the FIR work.
constexpr std::size_t kChunk = 8192;
}  // namespace

Vfo::Vfo(double inputRateHz, unsigned decimation, double bandwidthHz)
    : inputRate_(inputRateHz),
      // Folding 0 to 1 keeps the arithmetic below finite even if the release
      // build skips the assert; 0 has no meaningful interpretation anyway.
      decimFactor_(decimation == 0 ? 1u : decimation),
      bandwidth_(clampBandwidth(bandwidthHz)),
      decimator_(designTaps(bandwidth_), decimFactor_) {
    assert(inputRateHz > 0.0);
    assert(decimation >= 1);
}

double Vfo::clampBandwidth(double bw) const {
    const double channelRate = inputRate_ / static_cast<double>(decimFactor_);
    // Upper bound 0.9 * channelRate: leaves at least 0.05 * channelRate of
    // transition band on each side before the channel Nyquist, which a finite
    // filter needs to actually attenuate the aliases. Lower bound keeps the
    // windowed-sinc cutoff strictly positive (design precondition).
    return std::clamp(bw, 0.01 * channelRate, 0.9 * channelRate);
}

std::vector<float> Vfo::designTaps(double bw) const {
    const double channelRate = inputRate_ / static_cast<double>(decimFactor_);
    // Passband edge = bw/2 (the -6 dB cutoff of a windowed-sinc design),
    // normalized to the INPUT rate because the filter runs before decimation.
    const double cutoffNorm = 0.5 * bw / inputRate_;
    // Stopband edge = the channel Nyquist: every input frequency beyond
    // channelRate/2 folds somewhere into the channel after decimation, so the
    // whole channel — not just the passband — is protected from aliases.
    const double stopNorm = 0.5 * channelRate / inputRate_;
    const double transNorm = stopNorm - cutoffNorm;  // >= 0.05/decim by clamp

    // Tap count from the transition width via fred harris' rule of thumb
    // N ~= A / (22 * df), with A ~= 92 dB — the sidelobe floor of the 4-term
    // Blackman-Harris window, i.e. the stopband depth this design can reach.
    // Floor at 11 taps (below that the formula's small-N error dominates) and
    // cap at 16383 as a memory/CPU sanity bound for absurd decimations; the
    // cap only bites when decim > ~190, far beyond this receiver's use.
    std::size_t n = static_cast<std::size_t>(92.0 / (22.0 * transNorm)) + 1;
    n = std::clamp<std::size_t>(n, 11, 16383);
    // windowedSincLowpass requires an odd length (symmetric peak, integer
    // group delay); both clamp bounds are odd so this cannot exceed the cap.
    n |= 1u;
    return windowedSincLowpass(n, cutoffNorm, WindowType::BlackmanHarris);
}

void Vfo::setOffsetHz(double offset) {
    offset_ = offset;
    // Down-conversion: multiply by e^{-j 2 pi (offset/fs) n} so the content at
    // (center + offset) lands at DC. Nco::mix translates by +f, hence -offset.
    // Nco::setFrequency leaves the phase accumulator alone, which is what
    // makes retuning phase-continuous.
    nco_.setFrequency(-offset / inputRate_);
}

double Vfo::offsetHz() const { return offset_; }

void Vfo::setBandwidthHz(double bw) {
    bandwidth_ = clampBandwidth(bw);
    // A fresh FirDecimator: new taps, zero history, grid phase 0. See the
    // header for why history is not carried across a redesign.
    decimator_ = FirDecimator(designTaps(bandwidth_), decimFactor_);
}

double Vfo::channelRateHz() const {
    return inputRate_ / static_cast<double>(decimFactor_);
}

std::size_t Vfo::process(const std::complex<float>* in, std::size_t n,
                         std::complex<float>* out, std::size_t outCap) {
    std::size_t produced = 0;
    std::size_t pos = 0;
    while (pos < n) {
        const std::size_t m = std::min(kChunk, n - pos);
        // Mix into a scratch buffer rather than in-place: the caller's input
        // is const, and Vfo must not mutate it.
        mixBuf_.resize(m);
        nco_.mix(in + pos, mixBuf_.data(), m);

        // Filter+decimate into scratch sized for the worst case, then copy
        // out what fits. The decimator always consumes the whole chunk even
        // when outCap is exhausted — NCO phase, filter history, and grid
        // phase must advance over every input sample or later calls would
        // see a stream with a hole in it.
        chanBuf_.resize(decimator_.outputCapacity(m));
        const std::size_t got =
            decimator_.process(mixBuf_.data(), m, chanBuf_.data());
        const std::size_t take = std::min(got, outCap - produced);
        std::copy(chanBuf_.begin(),
                  chanBuf_.begin() + static_cast<std::ptrdiff_t>(take),
                  out + produced);
        produced += take;
        pos += m;
    }
    return produced;
}

void Vfo::reset() {
    nco_.reset();        // phase to 0; frequency (offset) retained
    decimator_.reset();  // history to zeros, grid back to sample 0
}

}  // namespace cascade::dsp
