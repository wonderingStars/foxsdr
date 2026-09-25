// real_to_iq.cpp - see real_to_iq.hpp for the method and its costs.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "dsp/real_to_iq.hpp"

namespace cascade::dsp {

namespace {

std::vector<float> halfBandTaps() {
    // Cutoff 0.25 of the INPUT rate is the -6 dB point of windowedSincLowpass,
    // i.e. exactly the edge of the band being kept after the -fs/4 shift. The
    // factor 2 (see the header's AMPLITUDE note) is folded into the taps, so
    // the per-sample work is only the mixer and the filter.
    std::vector<float> taps = windowedSincLowpass(RealToIq::kTaps, 0.25, WindowType::BlackmanHarris);
    for (float& t : taps) { t *= 2.0f; }
    return taps;
}

}  // namespace

RealToIq::RealToIq() : fir_(halfBandTaps(), 2) {}

std::size_t RealToIq::process(const float* in, std::size_t n, std::complex<float>* out) {
    if (in == nullptr || out == nullptr || n == 0) { return 0; }
    mixed_.resize(n);
    // x[n] * e^(-j*pi*n/2): phase 0 -> x, 1 -> -j x, 2 -> -x, 3 -> +j x.
    for (std::size_t i = 0; i < n; ++i) {
        const float x = in[i];
        switch (phase_) {
            case 0: mixed_[i] = {x, 0.0f}; break;
            case 1: mixed_[i] = {0.0f, -x}; break;
            case 2: mixed_[i] = {-x, 0.0f}; break;
            default: mixed_[i] = {0.0f, x}; break;
        }
        phase_ = (phase_ + 1u) & 3u;
    }
    return fir_.process(mixed_.data(), n, out);
}

void RealToIq::reset() {
    fir_.reset();
    phase_ = 0;
}

}  // namespace cascade::dsp
