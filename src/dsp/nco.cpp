// SPDX-License-Identifier: MIT
#include "dsp/nco.hpp"

#include <cmath>

namespace cascade::dsp {

namespace {
constexpr double kTwoPi = 6.283185307179586476925286766559;
}  // namespace

void Nco::setFrequency(double normalized) {
    // Fold into [-0.5, 0.5). floor() of a dyadic argument is exact, so exact
    // inputs stay exact — which is what makes the long-run closed-form phase
    // comparison in the tests meaningful rather than merely approximate.
    freq_ = normalized - std::floor(normalized + 0.5);
}

double Nco::phase() const noexcept { return kTwoPi * phaseCycles_; }

void Nco::step() noexcept {
    // |freq_| <= 0.5 keeps the sum inside (-1, 1), so a single conditional
    // wrap suffices; the +/-1.0 is exact in binary floating point.
    phaseCycles_ += freq_;
    if (phaseCycles_ >= 0.5) {
        phaseCycles_ -= 1.0;
    } else if (phaseCycles_ < -0.5) {
        phaseCycles_ += 1.0;
    }
}

void Nco::generate(std::complex<float>* out, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        // cos/sin are evaluated on the wrapped double phase each sample, so
        // the argument never grows and per-sample error is run-length
        // independent (see the class comment for why no phasor recurrence).
        const double a = kTwoPi * phaseCycles_;
        out[i] = std::complex<float>(static_cast<float>(std::cos(a)),
                                     static_cast<float>(std::sin(a)));
        step();
    }
}

void Nco::mix(const std::complex<float>* in, std::complex<float>* out,
              std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        const double a = kTwoPi * phaseCycles_;
        const std::complex<float> osc(static_cast<float>(std::cos(a)),
                                      static_cast<float>(std::sin(a)));
        // Reading in[i] before writing out[i] is what makes in == out legal.
        out[i] = in[i] * osc;
        step();
    }
}

}  // namespace cascade::dsp
